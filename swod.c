/* fs/f2fs/swod.c */
#include "swod.h"                                              
#include "segment.h" 
#include "node.h" 
static inline bool swod_enabled(struct discard_cmd_control *dcc)
{
	return dcc && dcc->swod_enable && dcc->swod;
}

static inline unsigned int swod_gid(struct swod_ctrl *sw, unsigned int segno)
{
	return segno / sw->win_segs;
}

static inline unsigned int swod_group_first_seg(struct swod_ctrl *sw,
						unsigned int gid)
{
	return gid * sw->win_segs;
}

static inline unsigned int swod_group_nsegs(struct swod_ctrl *sw,
					    unsigned int gid)
{
	unsigned int first = swod_group_first_seg(sw, gid);

	if (first >= sw->nr_main_segs)
		return 0;
	return min(sw->win_segs, sw->nr_main_segs - first);
}

static inline bool swod_is_urgent(struct f2fs_sb_info *sbi)
{
	return sbi->gc_mode == GC_URGENT_HIGH ||
	       sbi->gc_mode == GC_URGENT_LOW;
}

static inline unsigned int swod_max_hold_len(struct f2fs_sb_info *sbi,
					     struct swod_ctrl *sw,
					     unsigned int gid)
{
	unsigned int n = swod_group_nsegs(sw, gid);

	if (sbi->gc_mode == GC_URGENT_HIGH)
		return min(n, 1U);
	if (sbi->gc_mode == GC_URGENT_LOW)
		return min(n, 2U);
	return n;
}

static unsigned int swod_default_win_segs(struct f2fs_sb_info *sbi)
{
    struct block_device *bdev;
    struct request_queue *q;
    u64 seg_bytes;
	u64 max_bytes;
	// u64 seg_bytes = SEGMENT_SIZE(sbi);
	// u64 max_bytes = (u64)bdev_max_discard_sectors(sbi->sb->s_bdev) << 9;
	unsigned int n;

    if (!sbi || !sbi->sb)
		return 1;
	// if (!max_bytes || !seg_bytes)
	// 	return 1;

    bdev = sbi->sb->s_bdev;
	if (!bdev)
		return 1;

	q = bdev_get_queue(bdev);
	if (!q)
		return 1;

	seg_bytes = SEGMENT_SIZE(sbi);
    /*
	 * Linux 5.15: 直接使用 queue limits 中的软件 discard 上限
	 * max_discard_sectors 的单位是 512B sector
	 */
	max_bytes = (u64)q->limits.max_discard_sectors << 9;

	if (!max_bytes || !seg_bytes)
		return 1;


	n = div_u64(max_bytes, seg_bytes);
	if (!n)
		n = 1;
	if (n > 4)
		n = 4;
	if (n > SWOD_MAX_WIN_SEGS)
		n = SWOD_MAX_WIN_SEGS;
	return n;
}

static inline bool swod_regime_blocked(struct f2fs_sb_info *sbi,
				       struct discard_cmd_control *dcc,
				       struct discard_policy *dpolicy)
{
	if (!swod_enabled(dcc))
		return true;

	/*
	 * Normal path: SWOD only shapes background discard.
	 * Urgent path: still allow SWOD, but urgent windows are shrunk
	 * in swod_eval_group_locked().
	 */
	if (dpolicy && dpolicy->type != DPOLICY_BG && !swod_is_urgent(sbi))
		return true;

	/* stock discard thread already treats this as aggressive regime */
	if (utilization(sbi) > DEF_DISCARD_URGENT_UTIL)
		return true;

	if (!f2fs_available_free_memory(sbi, DISCARD_CACHE))
		return true;

	/*
	 * Do not block new SWOD candidate evaluation just because backlog is large.
	 * High discard_cmd_cnt / undiscard_blks is exactly when shaping is useful.
	 * Existing held ranges are still released by the issue thread when the
	 * runtime policy switches to a bypass regime.
	 */
	return false;
}

static unsigned int swod_calc_hold_ms(struct f2fs_sb_info *sbi,
				      unsigned int run_len,
				      unsigned int qcov_bp,
				      unsigned int lres_bp)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	unsigned int hold = dcc->swod_hold_min_ms;

	/* 保守整数启发式：run 越长、qcov 越高、lres 越低，可以多等一点 */
	if (run_len > 1)
		hold += (run_len - 1) * 10;

	if (qcov_bp > dcc->swod_qcov_thr_bp)
		hold += (qcov_bp - dcc->swod_qcov_thr_bp) / 100;

	if (dcc->swod_lres_thr_bp > lres_bp)
		hold += (dcc->swod_lres_thr_bp - lres_bp) / 100;

	if (dcc->swod_hold_scale_bp)
		hold = div_u64((u64)hold * dcc->swod_hold_scale_bp, SWOD_BP_ONE);

	if (hold < dcc->swod_hold_min_ms)
		hold = dcc->swod_hold_min_ms;
	if (hold > dcc->swod_hold_max_ms)
		hold = dcc->swod_hold_max_ms;
	return hold;
}

static inline bool swod_group_is_active(const struct swod_group_hint *g)
{
	return g->state == SWOD_G_HELD || g->state == SWOD_G_PARKED;
}

static inline unsigned long swod_group_deadline(const struct swod_group_hint *g)
{
	return g->state == SWOD_G_PARKED ? g->park_until : g->hold_until;
}

static unsigned int swod_calc_park_ms(struct f2fs_sb_info *sbi,
				      unsigned int run_len,
				      unsigned int qcov_bp,
				      unsigned int lres_bp)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	unsigned int park = swod_calc_hold_ms(sbi, run_len, qcov_bp, lres_bp);

	park <<= 2;
	if (park < dcc->swod_hold_max_ms)
		park = dcc->swod_hold_max_ms;
	if (park < SWOD_WCE_PARK_MIN_MS)
		park = SWOD_WCE_PARK_MIN_MS;
	if (park > SWOD_WCE_PARK_MAX_MS)
		park = SWOD_WCE_PARK_MAX_MS;
	return park;
}

static inline unsigned int swod_park_budget(struct discard_cmd_control *dcc)
{
	return max(4U, dcc->swod_max_held_groups >> 1);
}

static void swod_clear_group_locked(struct f2fs_sb_info *sbi,
				    unsigned int gid)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct swod_ctrl *sw = dcc->swod;
	struct swod_group_hint *g = &sw->grp_hint[gid];
	unsigned int first = swod_group_first_seg(sw, gid);
	unsigned int nsegs = swod_group_nsegs(sw, gid);
	unsigned int i;

	if (g->state == SWOD_G_HELD && sw->nr_held_groups)
		sw->nr_held_groups--;
	else if (g->state == SWOD_G_PARKED && sw->nr_parked_groups)
		sw->nr_parked_groups--;

	g->state = SWOD_G_NORMAL;
	g->hold_off = 0;
	g->hold_len = 0;
	g->hold_qbp = 0;
	g->hold_lbp = 0;
	g->park_score = 0;
	g->hold_until = 0;
	g->park_until = 0;
	g->last_eval = jiffies;

	for (i = 0; i < nsegs; i++) {
		clear_bit(first + i, sw->hold_segmap);
		clear_bit(first + i, sw->park_segmap);
	}
}

static bool swod_collect_window_locked(struct f2fs_sb_info *sbi,
				       unsigned int gid,
				       unsigned int *qbp,
				       unsigned int *lbp,
				       unsigned int *cold_cnt,
				       unsigned long *oldest)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct swod_ctrl *sw = dcc->swod;
	struct swod_group_hint *g = &sw->grp_hint[gid];
	unsigned int first = swod_group_first_seg(sw, gid);
	unsigned int i, q = 0, l = 0, cold = 0;
	unsigned long oldest_j = 0;
	u64 cap;

	if (!swod_group_is_active(g) || !g->hold_len)
		return false;

	for (i = 0; i < g->hold_len; i++) {
		unsigned int segno = first + g->hold_off + i;
		struct seg_entry *se;

		if (segno >= sw->nr_main_segs)
			return false;

		q += sw->seg_hint[segno].pend_blks;
		l += get_valid_blocks(sbi, segno, false);
		if (sw->seg_hint[segno].oldest_jiffies &&
		    (!oldest_j ||
		     time_before(sw->seg_hint[segno].oldest_jiffies, oldest_j)))
			oldest_j = sw->seg_hint[segno].oldest_jiffies;

		se = get_seg_entry(sbi, segno);
		if (IS_COLD(se->type))
			cold++;
	}

	cap = (u64)g->hold_len * sbi->blocks_per_seg;
	if (!cap)
		return false;

	if (qbp)
		*qbp = div_u64((u64)q * SWOD_BP_ONE, cap);
	if (lbp)
		*lbp = div_u64((u64)l * SWOD_BP_ONE, cap);
	if (cold_cnt)
		*cold_cnt = cold;
	if (oldest)
		*oldest = oldest_j;
	return true;
}

static unsigned int swod_calc_park_score(unsigned int len,
					 unsigned int qbp,
					 unsigned int lbp,
					 unsigned int cold_cnt,
					 unsigned long oldest,
					 unsigned long now)
{
	unsigned long age_ms = 0;

	if (oldest && !time_after(oldest, now))
		age_ms = jiffies_to_msecs(now - oldest);
	if (age_ms > 1000)
		age_ms = 1000;

	return cold_cnt * 100000U + len * 10000U + qbp * 2U +
	       (SWOD_BP_ONE - min(lbp, SWOD_BP_ONE)) + age_ms;
}

static void swod_account_release(struct swod_ctrl *sw,
				 enum swod_release_reason why)
{
	switch (why) {
	case SWOD_REL_SUCCESS:
		atomic64_inc(&sw->success_release_cnt);
		break;
	case SWOD_REL_TIMEOUT:
		atomic64_inc(&sw->timeout_release_cnt);
		break;
	case SWOD_REL_PRESSURE:
	default:
		atomic64_inc(&sw->pressure_release_cnt);
		break;
	}
}

static void swod_drop_group_locked(struct f2fs_sb_info *sbi,
				   unsigned int gid,
				   enum swod_release_reason why)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct swod_ctrl *sw = dcc->swod;

	if (!sw)
		return;

	swod_account_release(sw, why);
	swod_clear_group_locked(sbi, gid);
}

static bool swod_try_park_group_locked(struct f2fs_sb_info *sbi,
				       unsigned int gid,
				       enum swod_release_reason why,
				       unsigned long now)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct swod_ctrl *sw = dcc->swod;
	struct swod_group_hint *g = &sw->grp_hint[gid];
	unsigned int qbp, lbp, cold_cnt, limit, budget;
	unsigned long oldest;
	unsigned int score, i;
	unsigned int victim_gid = UINT_MAX;
	struct swod_group_hint *victim = NULL;
	unsigned int first;

	if (!sw || g->state != SWOD_G_HELD)
		return false;
	if (why != SWOD_REL_TIMEOUT && why != SWOD_REL_PRESSURE)
		return false;
	if (!swod_collect_window_locked(sbi, gid, &qbp, &lbp,
					&cold_cnt, &oldest))
		return false;

	/* still needs pending holes, but should already be easy to clean */
	if (qbp < dcc->swod_qcov_thr_bp)
		return false;
	limit = min_t(unsigned int, SWOD_WCE_PARK_LRES_CAP_BP,
		      dcc->swod_lres_thr_bp + 1000);
	if (!lbp || lbp > limit)
		return false;

	score = swod_calc_park_score(g->hold_len, qbp, lbp,
				    cold_cnt, oldest, now);
	budget = swod_park_budget(dcc);
	if (sw->nr_parked_groups >= budget) {
		for (i = 0; i < sw->nr_groups; i++) {
			struct swod_group_hint *cand = &sw->grp_hint[i];

			if (cand->state != SWOD_G_PARKED)
				continue;
			if (!victim || cand->park_score < victim->park_score) {
				victim_gid = i;
				victim = cand;
			}
		}
		if (!victim || score <= victim->park_score)
			return false;
		swod_drop_group_locked(sbi, victim_gid, SWOD_REL_PRESSURE);
	}

	first = swod_group_first_seg(sw, gid);
	if (sw->nr_held_groups)
		sw->nr_held_groups--;
	for (i = 0; i < g->hold_len; i++) {
		clear_bit(first + g->hold_off + i, sw->hold_segmap);
		set_bit(first + g->hold_off + i, sw->park_segmap);
	}

	g->state = SWOD_G_PARKED;
	g->hold_qbp = qbp;
	g->hold_lbp = lbp;
	g->park_score = score;
	g->hold_until = 0;
	g->park_until = now + msecs_to_jiffies(
		swod_calc_park_ms(sbi, g->hold_len, qbp, lbp));
	g->last_eval = now;
	sw->nr_parked_groups++;
	return true;
}

static void swod_release_group_locked(struct f2fs_sb_info *sbi,
				      unsigned int gid,
				      enum swod_release_reason why)
{
	if (swod_try_park_group_locked(sbi, gid, why, jiffies))
		return;
	swod_drop_group_locked(sbi, gid, why);
}

static bool swod_window_ready_locked(struct f2fs_sb_info *sbi,
				     unsigned int gid)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct swod_ctrl *sw = dcc->swod;
	struct swod_group_hint *g = &sw->grp_hint[gid];
	unsigned int first = swod_group_first_seg(sw, gid);
	unsigned int i;

	if (!swod_group_is_active(g) || !g->hold_len)
		return false;

	for (i = 0; i < g->hold_len; i++) {
		unsigned int segno = first + g->hold_off + i;

		if (segno >= sw->nr_main_segs)
			return false;

		if (sw->seg_hint[segno].pend_blks != sbi->blocks_per_seg)
			return false;

		if (get_valid_blocks(sbi, segno, false) != 0)
			return false;
	}
	return true;
}

static struct discard_cmd *swod_rb_lower_bound(struct rb_root_cached *root,
					       block_t blk)
{
	struct rb_node *node = root->rb_root.rb_node;
	struct rb_node *best = NULL;

	while (node) {
		struct discard_cmd *dc = rb_entry(node, struct discard_cmd, rb_node);

		if (dc->di.lstart + dc->di.len <= blk) {
			node = node->rb_right;
		} else {
			best = node;
			node = node->rb_left;
		}
	}
	return rb_entry_safe(best, struct discard_cmd, rb_node);
}

static void swod_rebuild_groups_locked(struct f2fs_sb_info *sbi,
				       unsigned int gid0,
				       unsigned int gid1)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct swod_ctrl *sw = dcc->swod;
	unsigned int g, start_seg, end_seg;
	block_t scan_start, scan_end;
	struct discard_cmd *dc;

	if (!sw || gid0 > gid1)
		return;

	start_seg = swod_group_first_seg(sw, gid0);
	end_seg = min(sw->nr_main_segs,
		      swod_group_first_seg(sw, gid1) + sw->win_segs);

	/* clear per-seg hints in [start_seg, end_seg) */
	for (g = start_seg; g < end_seg; g++) {
		sw->seg_hint[g].pend_blks = 0;
		sw->seg_hint[g].nr_cmds = 0;
		sw->seg_hint[g].oldest_jiffies = 0;
	}

	if (start_seg >= end_seg)
		return;

	scan_start = START_BLOCK(sbi, start_seg);
	scan_end   = START_BLOCK(sbi, end_seg);

	dc = swod_rb_lower_bound(&dcc->root, scan_start);
	while (dc && dc->di.lstart < scan_end) {
		struct rb_node *next = rb_next(&dc->rb_node);
		block_t from, to, cur;

		/* SWOD only summarizes D_PREP-visible pending range */
		if (dc->state != D_PREP) {
			dc = rb_entry_safe(next, struct discard_cmd, rb_node);
			continue;
		}

		from = max(dc->di.lstart, scan_start);
		to   = min(dc->di.lstart + dc->di.len, scan_end);
		cur  = from;

		while (cur < to) {
			unsigned int segno = GET_SEGNO(sbi, cur);
			block_t seg_end = START_BLOCK(sbi, segno + 1);
			block_t piece = min(to, seg_end) - cur;
			struct swod_seg_hint *h = &sw->seg_hint[segno];

			h->pend_blks += piece;
			h->nr_cmds++;
			if (!h->oldest_jiffies ||
			    time_before(dc->enq_jiffies, h->oldest_jiffies))
				h->oldest_jiffies = dc->enq_jiffies;
			cur += piece;
		}
		dc = rb_entry_safe(next, struct discard_cmd, rb_node);
	}
}

static void swod_eval_group_locked(struct f2fs_sb_info *sbi,
				   unsigned int gid,
				   unsigned long now)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct swod_ctrl *sw = dcc->swod;
	struct swod_group_hint *g = &sw->grp_hint[gid];
	unsigned int first = swod_group_first_seg(sw, gid);
	unsigned int n = swod_group_nsegs(sw, gid);
	unsigned int qpref[SWOD_MAX_WIN_SEGS + 1] = {0};
	unsigned int lpref[SWOD_MAX_WIN_SEGS + 1] = {0};
	unsigned long oldest_seg[SWOD_MAX_WIN_SEGS] = {0};
	bool found = false;
	unsigned int best_off = 0, best_len = 0;
	unsigned int best_qbp = 0, best_lbp = UINT_MAX;
	unsigned long best_oldest = 0;
	unsigned int max_len = swod_max_hold_len(sbi, sw, gid);
	unsigned int i, off, len, max_this_len;

	if (!sw || !n)
		return;

	if (swod_regime_blocked(sbi, dcc, NULL)) {
		atomic64_inc(&sw->eval_blocked_cnt);
		if (swod_group_is_active(g))
			swod_drop_group_locked(sbi, gid, SWOD_REL_PRESSURE);
		return;
	}

	/* urgent mode can only keep a smaller window; re-evaluate from scratch */
	if (swod_group_is_active(g) && g->hold_len > max_len)
		swod_drop_group_locked(sbi, gid, SWOD_REL_PRESSURE);

	if (swod_group_is_active(g)) {
		if (swod_window_ready_locked(sbi, gid)) {
			swod_release_group_locked(sbi, gid, SWOD_REL_SUCCESS);
			return;
		}
		if (time_after_eq(now, swod_group_deadline(g))) {
			swod_release_group_locked(sbi, gid, SWOD_REL_TIMEOUT);
			return;
		}
		return;
	}

	for (i = 0; i < n; i++) {
		qpref[i + 1] = qpref[i] + sw->seg_hint[first + i].pend_blks;
		lpref[i + 1] = lpref[i] + get_valid_blocks(sbi, first + i, false);
		oldest_seg[i] = sw->seg_hint[first + i].oldest_jiffies;
	}

	for (off = 0; off < n; off++) {
		unsigned long cur_oldest = 0;

		max_this_len = min(max_len, n - off);
		for (len = 1; len <= max_this_len; len++) {
			u64 cap, qbp, lbp;
			unsigned int endi = off + len - 1;
			unsigned int q, l;

			if (oldest_seg[endi] &&
			    (!cur_oldest ||
			     time_before(oldest_seg[endi], cur_oldest)))
				cur_oldest = oldest_seg[endi];

			q = qpref[off + len] - qpref[off];
			l = lpref[off + len] - lpref[off];
			cap = (u64)len * sbi->blocks_per_seg;

			qbp = div_u64((u64)q * SWOD_BP_ONE, cap);
			lbp = div_u64((u64)l * SWOD_BP_ONE, cap);

			/* already fully ready: let stock issue it, do not hold */
			if (q == cap)
				continue;

			if (qbp < dcc->swod_qcov_thr_bp)
				continue;
			if (lbp > dcc->swod_lres_thr_bp)
				continue;

			if (!found ||
			    len > best_len ||
			    (len == best_len && lbp < best_lbp) ||
			    (len == best_len && lbp == best_lbp &&
			     (!best_oldest || time_before(cur_oldest, best_oldest)))) {
				found = true;
				best_off = off;
				best_len = len;
				best_qbp = qbp;
				best_lbp = lbp;
				best_oldest = cur_oldest;
			}
		}
	}

	if (!found) {
		atomic64_inc(&sw->eval_no_candidate_cnt);
		return;
	}

	if (sw->nr_held_groups >= dcc->swod_max_held_groups) {
		unsigned int victim_gid = UINT_MAX;
		struct swod_group_hint *victim = NULL;

		for (i = 0; i < sw->nr_groups; i++) {
			struct swod_group_hint *cand = &sw->grp_hint[i];

			if (cand->state != SWOD_G_HELD)
				continue;
			if (!victim || cand->hold_qbp < victim->hold_qbp ||
			    (cand->hold_qbp == victim->hold_qbp &&
			     cand->hold_lbp > victim->hold_lbp) ||
			    (cand->hold_qbp == victim->hold_qbp &&
			     cand->hold_lbp == victim->hold_lbp &&
			     cand->hold_len < victim->hold_len)) {
				victim_gid = i;
				victim = cand;
			}
		}

		if (!victim)
			return;
		if (best_qbp < victim->hold_qbp)
			return;
		if (best_qbp == victim->hold_qbp && best_lbp > victim->hold_lbp)
			return;
		if (best_qbp == victim->hold_qbp && best_lbp == victim->hold_lbp &&
		    best_len <= victim->hold_len)
			return;

		swod_release_group_locked(sbi, victim_gid, SWOD_REL_PRESSURE);
	}

	g->state = SWOD_G_HELD;
	g->hold_off = best_off;
	g->hold_len = best_len;
	g->hold_qbp = best_qbp;
	g->hold_lbp = best_lbp;
	g->park_score = 0;
	g->hold_until = now + msecs_to_jiffies(
		swod_calc_hold_ms(sbi, best_len, best_qbp, best_lbp));
	g->park_until = 0;
	g->last_eval = now;
	sw->nr_held_groups++;

	for (i = 0; i < best_len; i++) {
		set_bit(first + best_off + i, sw->hold_segmap);
		clear_bit(first + best_off + i, sw->park_segmap);
	}

	atomic64_inc(&sw->hold_cnt);
}

int f2fs_swod_init(struct f2fs_sb_info *sbi)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct swod_ctrl *sw;
	size_t seg_hint_sz, grp_hint_sz, bm_sz;

	if (!dcc)
		return -EINVAL;

	if (dcc->swod)
		return 0;

	sw = f2fs_kzalloc(sbi, sizeof(*sw), GFP_KERNEL);
	if (!sw)
		return -ENOMEM;

	if (!dcc->swod_win_segs)
		dcc->swod_win_segs = swod_default_win_segs(sbi);
	if (dcc->swod_win_segs > SWOD_MAX_WIN_SEGS)
		dcc->swod_win_segs = SWOD_MAX_WIN_SEGS;

	if (!dcc->swod_qcov_thr_bp)
		dcc->swod_qcov_thr_bp = 8500;
	if (!dcc->swod_lres_thr_bp)
		dcc->swod_lres_thr_bp = 1000;
	if (!dcc->swod_hold_min_ms)
		dcc->swod_hold_min_ms = 50;
	if (!dcc->swod_hold_max_ms)
		dcc->swod_hold_max_ms = 300;
	if (!dcc->swod_cmd_pressure)
		dcc->swod_cmd_pressure = 4096;
	if (!dcc->swod_blk_pressure)
		dcc->swod_blk_pressure = 1 << 20;
	if (!dcc->swod_max_held_groups)
		dcc->swod_max_held_groups = 64;

	sw->nr_main_segs = MAIN_SEGS(sbi);
	sw->win_segs = dcc->swod_win_segs;
	sw->nr_groups = DIV_ROUND_UP(sw->nr_main_segs, sw->win_segs);

	seg_hint_sz = array_size(sw->nr_main_segs, sizeof(*sw->seg_hint));
	grp_hint_sz = array_size(sw->nr_groups, sizeof(*sw->grp_hint));
	bm_sz = BITS_TO_LONGS(sw->nr_main_segs) * sizeof(unsigned long);

	sw->seg_hint = f2fs_kvzalloc(sbi, seg_hint_sz, GFP_KERNEL);
	sw->grp_hint = f2fs_kvzalloc(sbi, grp_hint_sz, GFP_KERNEL);
	sw->hold_segmap = f2fs_kvzalloc(sbi, bm_sz, GFP_KERNEL);
	sw->park_segmap = f2fs_kvzalloc(sbi, bm_sz, GFP_KERNEL);
	if (!sw->seg_hint || !sw->grp_hint || !sw->hold_segmap ||
	    !sw->park_segmap) {
		kvfree(sw->seg_hint);
		kvfree(sw->grp_hint);
		kvfree(sw->hold_segmap);
		kvfree(sw->park_segmap);
		kfree(sw);
		return -ENOMEM;
	}

	dcc->swod = sw;
	return 0;
}

void f2fs_swod_destroy(struct f2fs_sb_info *sbi)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct swod_ctrl *sw;

	if (!dcc || !dcc->swod)
		return;

	sw = dcc->swod;
	kvfree(sw->seg_hint);
	kvfree(sw->grp_hint);
	kvfree(sw->hold_segmap);
	kvfree(sw->park_segmap);
	kfree(sw);
	dcc->swod = NULL;
}

void f2fs_swod_refresh_around_locked(struct f2fs_sb_info *sbi,
				     block_t lstart, block_t len)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct swod_ctrl *sw = dcc->swod;
	unsigned int seg0, seg1, gid0, gid1, g;
	unsigned long now = jiffies;

	if (!swod_enabled(dcc) || !len)
		return;

	seg0 = GET_SEGNO(sbi, lstart);
	seg1 = GET_SEGNO(sbi, lstart + len - 1);

	gid0 = swod_gid(sw, seg0);
	gid1 = swod_gid(sw, seg1);

	if (gid0 > 0)
		gid0--;
	if (gid1 + 1 < sw->nr_groups)
		gid1++;

	swod_rebuild_groups_locked(sbi, gid0, gid1);

	for (g = gid0; g <= gid1; g++)
		swod_eval_group_locked(sbi, g, now);
}

bool f2fs_swod_should_skip_locked(struct f2fs_sb_info *sbi,
				  struct discard_cmd *dc,
				  struct discard_policy *dpolicy,
				  unsigned long now)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct swod_ctrl *sw = dcc->swod;
	unsigned int seg0, seg1, gid0, gid1, gid;
	bool saw_active = false;

	if (!swod_enabled(dcc))
		return false;

	atomic64_inc(&sw->skip_check_cnt);

	if (dc->state != D_PREP)
		return false;

	seg0 = GET_SEGNO(sbi, dc->di.lstart);
	seg1 = GET_SEGNO(sbi, dc->di.lstart + dc->di.len - 1);
	gid0 = swod_gid(sw, seg0);
	gid1 = swod_gid(sw, seg1);

	for (gid = gid0; gid <= gid1; gid++) {
		struct swod_group_hint *g = &sw->grp_hint[gid];
		unsigned int held_first, held_last;

		if (!swod_group_is_active(g))
			continue;
		saw_active = true;

		if (time_after_eq(now, swod_group_deadline(g))) {
			atomic64_inc(&sw->skip_miss_timeout_cnt);
			swod_release_group_locked(sbi, gid, SWOD_REL_TIMEOUT);
			continue;
		}

		if (swod_window_ready_locked(sbi, gid)) {
			atomic64_inc(&sw->skip_miss_success_cnt);
			swod_release_group_locked(sbi, gid, SWOD_REL_SUCCESS);
			continue;
		}

		held_first = swod_group_first_seg(sw, gid) + g->hold_off;
		held_last  = held_first + g->hold_len - 1;

		/*
		 * Hold any cmd overlapping an active sub-window.
		 * This covers cross-group cmds and partial overlaps too.
		 */
		if (seg1 < held_first || seg0 > held_last) {
			atomic64_inc(&sw->skip_miss_overlap_cnt);
			continue;
		}

		atomic64_inc(&sw->skip_cnt);
		return true;
	}

	if (!saw_active)
		atomic64_inc(&sw->skip_miss_noheld_cnt);
	return false;
}

static void __f2fs_swod_release_all_locked(struct f2fs_sb_info *sbi,
					   enum swod_release_reason why)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct swod_ctrl *sw = dcc->swod;
	unsigned int gid;

	if (!sw)
		return;

	for (gid = 0; gid < sw->nr_groups; gid++) {
		if (swod_group_is_active(&sw->grp_hint[gid]))
			swod_drop_group_locked(sbi, gid, why);
	}
}

void f2fs_swod_release_all(struct f2fs_sb_info *sbi,
			   enum swod_release_reason why)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;

	if (!swod_enabled(dcc))
		return;

	mutex_lock(&dcc->cmd_lock);
	__f2fs_swod_release_all_locked(sbi, why);
	mutex_unlock(&dcc->cmd_lock);
}

void f2fs_swod_sweep_timeout(struct f2fs_sb_info *sbi, unsigned long now)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct swod_ctrl *sw;
	unsigned int gid;

	if (!swod_enabled(dcc))
		return;

	sw = dcc->swod;
	mutex_lock(&dcc->cmd_lock);
	for (gid = 0; gid < sw->nr_groups; gid++) {
		struct swod_group_hint *g = &sw->grp_hint[gid];

		if (!swod_group_is_active(g))
			continue;
		if (time_after_eq(now, swod_group_deadline(g)))
			swod_release_group_locked(sbi, gid, SWOD_REL_TIMEOUT);
	}
	mutex_unlock(&dcc->cmd_lock);
}

// bool f2fs_swod_seg_held(struct f2fs_sb_info *sbi, unsigned int segno)
// {
// 	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;

// 	if (!swod_enabled(dcc))
// 		return false;
// 	if (segno >= dcc->swod->nr_main_segs)
// 		return false;

// 	return test_bit(segno, dcc->swod->hold_segmap);
// }



/*
 * GC-side WCE lookup is advisory only, so lockless bitmap reads are enough.
 * Races only change the bias of victim selection and will fall back to the
 * stock picker when no target survives the normal GC checks.
 */
bool f2fs_swod_has_held(struct f2fs_sb_info *sbi)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;

	if (!swod_enabled(dcc))
		return false;

	return !!READ_ONCE(dcc->swod->nr_held_groups);
}

bool f2fs_swod_range_held(struct f2fs_sb_info *sbi, unsigned int segno,
			  unsigned int nr_segs)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct swod_ctrl *sw;
	unsigned int end;

	if (!swod_enabled(dcc))
		return false;

	sw = dcc->swod;
	if (!nr_segs || segno >= sw->nr_main_segs)
		return false;
	if (!READ_ONCE(sw->nr_held_groups))
		return false;

	end = min(sw->nr_main_segs, segno + nr_segs);
	for (; segno < end; segno++) {
		if (test_bit(segno, sw->hold_segmap))
			return true;
	}

	return false;
}

bool f2fs_swod_has_wce_target(struct f2fs_sb_info *sbi)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct swod_ctrl *sw;

	if (!swod_enabled(dcc))
		return false;

	sw = dcc->swod;
	return !!READ_ONCE(sw->nr_held_groups) || !!READ_ONCE(sw->nr_parked_groups);
}

bool f2fs_swod_range_wce_target(struct f2fs_sb_info *sbi, unsigned int segno,
				unsigned int nr_segs)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct swod_ctrl *sw;
	unsigned int end;

	if (!swod_enabled(dcc))
		return false;

	sw = dcc->swod;
	if (!nr_segs || segno >= sw->nr_main_segs)
		return false;
	if (!READ_ONCE(sw->nr_held_groups) && !READ_ONCE(sw->nr_parked_groups))
		return false;

	end = min(sw->nr_main_segs, segno + nr_segs);
	for (; segno < end; segno++) {
		if (test_bit(segno, sw->hold_segmap) ||
		    test_bit(segno, sw->park_segmap))
			return true;
	}

	return false;
}

bool f2fs_swod_seg_held(struct f2fs_sb_info *sbi, unsigned int segno)
{
	return f2fs_swod_range_held(sbi, segno, 1);
}

/*
 * Advisory-only selector for a very narrow LFS-side fragment IPU exception.
 * It is only used while SWOD currently has held windows, and never for
 * segments that belong to a held target window.
    不决定 IPU/OPU；
    只判断“这个 old block 对应的 seg 是否是一个老、小、碎、非 target、非热的 fragment 候选”
*/
bool f2fs_swod_should_frag_ipu(struct inode *inode,
			       struct f2fs_io_info *fio)
{
	struct f2fs_sb_info *sbi = fio->sbi;
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct swod_ctrl *sw;
	unsigned int segno, pend_blks, nr_cmds, age_thr;
	unsigned long oldest, age_ms;

	if (!dcc || !swod_enabled(dcc) || !dcc->swod_frag_ipu_enable)
		return false;
	if (!f2fs_lfs_mode(sbi))
		return false;
	if (!fio || !__is_valid_data_blkaddr(fio->old_blkaddr))
		return false;

	/*
	 * SFI 是 WCE 的辅助路径，只在系统当前真的有 held windows 时启用。
	 */
	if (!f2fs_swod_has_held(sbi))
		return false;

	sw = dcc->swod;
	segno = GET_SEGNO(sbi, fio->old_blkaddr);
	if (segno >= sw->nr_main_segs)
		return false;

	/*
	 * target window 绝不走这个辅助 IPU，避免和 WCE completion 打架。
	 */
	if (f2fs_swod_seg_held(sbi, segno)) {
		atomic64_inc(&sw->frag_ipu_skip_target_cnt);
		return false;
	}

	/*
	 * 热数据直接跳过；温/冷数据更适合这条路径。
	 */
	if (dcc->swod_frag_ipu_skip_hot &&
	    (file_is_hot(inode) || is_inode_flag_set(inode, FI_HOT_DATA))) {
		atomic64_inc(&sw->frag_ipu_skip_hot_cnt);
		return false;
	}

	/*
	 * 基于 segment 碎片化程度的判断（SFI 改进 v2）
	 * 不能有 discard_granularity 特权
	 * 必须 segment 上有足够多的 pending discard 才允许 IPU
	 * 设置为 discard_granularity 的 4 倍，防止 len=1 的 dc 凑够 nr_cmds 就满足条件
	 */
	pend_blks = READ_ONCE(sw->seg_hint[segno].pend_blks);
	nr_cmds = READ_ONCE(sw->seg_hint[segno].nr_cmds);

	/*
	 * 条件1：该 segment 必须有足够的碎片化
	 * pend_blks >= discard_granularity * 4，防止 len=1 的 dc 满足条件
	 */
	if (!pend_blks || pend_blks < dcc->discard_granularity * 4) {
		atomic64_inc(&sw->frag_ipu_skip_shape_cnt);
		return false;
	}

	/*
	 * 条件2：碎片必须来自多个 cmd 的合并
	 * 否则说明该 segment 的碎片不是真正的 fragment
	 */
	if (nr_cmds < dcc->swod_frag_ipu_min_cmds) {
		atomic64_inc(&sw->frag_ipu_skip_shape_cnt);
		return false;
	}

	oldest = READ_ONCE(sw->seg_hint[segno].oldest_jiffies);
	if (!oldest || time_after(oldest, jiffies)) {
		atomic64_inc(&sw->frag_ipu_skip_age_cnt);
		return false;
	}

	age_ms = jiffies_to_msecs(jiffies - oldest);

	/*
	 * 冷数据更容易放行；非冷数据要更老才值得这次 IPU。
	 */
	age_thr = dcc->swod_frag_ipu_age_ms;
	if (!file_is_cold(inode))
		age_thr <<= 1;

	if (age_ms < age_thr) {
		atomic64_inc(&sw->frag_ipu_skip_age_cnt);
		return false;
	}

	atomic64_inc(&sw->frag_ipu_pick_cnt);
	return true;
}
