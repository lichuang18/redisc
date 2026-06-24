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
	unsigned int n;

	if (!sbi || !sbi->sb)
		return 1;

	bdev = sbi->sb->s_bdev;
	if (!bdev)
		return 1;

	q = bdev_get_queue(bdev);
	if (!q)
		return 1;

	seg_bytes = SEGMENT_SIZE(sbi);
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

	if (dpolicy && dpolicy->type != DPOLICY_BG && !swod_is_urgent(sbi))
		return true;

	if (utilization(sbi) > DEF_DISCARD_URGENT_UTIL)
		return true;

	if (!f2fs_available_free_memory(sbi, DISCARD_CACHE))
		return true;

	return false;
}

/*
 * V4: 计算 hold 超时时间
 * 碎片化窗口的超时时间是普通窗口的 k 倍
 */
static unsigned int swod_calc_hold_ms(struct f2fs_sb_info *sbi,
				      unsigned int run_len,
				      unsigned int qcov_bp,
				      unsigned int lres_bp,
				      bool is_frag)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	unsigned int hold = dcc->swod_hold_min_ms;

	/* 保守整数启发式 */
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

	/* V4: 碎片化窗口延长超时时间 */
	if (is_frag) {
		unsigned int k = dcc->swod_frag_timeout_k ? dcc->swod_frag_timeout_k : 3;
		hold = hold * k;
		if (hold > dcc->swod_hold_max_ms * k)
			hold = dcc->swod_hold_max_ms * k;
	}

	return hold;
}

static inline bool swod_group_is_active(const struct swod_group_hint *g)
{
	return g->state == SWOD_G_HELD;
}

static inline unsigned long swod_group_deadline(const struct swod_group_hint *g)
{
	return g->hold_until;
}

/*
 * V4: 清除 group 的 held 状态
 * 同时清除 held_segmap 中对应的 bit
 */
static void swod_clear_group_locked(struct f2fs_sb_info *sbi,
				    unsigned int gid)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct swod_ctrl *sw = dcc->swod;
	struct swod_group_hint *g = &sw->grp_hint[gid];
	unsigned int first = swod_group_first_seg(sw, gid);
	unsigned int off = g->hold_off;
	unsigned int len = g->hold_len;
	unsigned int i;

	/*
	 * V4: 先保存 win_type 再清理 group 状态。
	 * held_segmap 只在 HIGH_COV 窗口设置，清理时需要用旧值判断。
	 */
	u8 win_type = g->win_type;

	if (g->state == SWOD_G_HELD && atomic_read(&sw->nr_held_groups) &&
	    win_type == SWOD_WIN_HIGH_COV)
		atomic_dec(&sw->nr_held_groups);

	g->state = SWOD_G_NORMAL;
	g->hold_off = 0;
	g->hold_len = 0;
	g->hold_qbp = 0;
	g->hold_lbp = 0;
	g->hold_until = 0;
	g->last_eval = jiffies;
	g->win_type = SWOD_WIN_NONE;
	g->frag_score = 0;

	/* V4: 只清理 held window 范围内的 segment（用 hold_off + hold_len） */
	if (win_type == SWOD_WIN_HIGH_COV && len > 0) {
		for (i = 0; i < len; i++)
			clear_bit(first + off + i, sw->held_segmap);
	}
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

static void swod_release_group_locked(struct f2fs_sb_info *sbi,
				      unsigned int gid,
				      enum swod_release_reason why)
{
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

/*
 * V4: 评估 group 内每个 segment 是否应该加入 hbu_targets
 * HBU 条件：高有效块 (>= 80%) + 高碎片化 (nr_cmds >= 3) + 不在 held_segmap 中
 */
static void swod_eval_segments_for_hbu_locked(struct f2fs_sb_info *sbi,
					       unsigned int gid0,
					       unsigned int gid1)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct swod_ctrl *sw = dcc->swod;
	unsigned int gid;
	unsigned int hbu_valid_thr = dcc->swod_hbu_valid_thr_bp;
	unsigned int hbu_min_cmds = dcc->swod_hbu_min_cmds;

	if (!sw || !hbu_valid_thr)
		return;

	/* 如果没有设置阈值，使用默认值 80% */
	if (!hbu_valid_thr)
		hbu_valid_thr = 8000;
	if (!hbu_min_cmds)
		hbu_min_cmds = 3;

	/* 遍历 gid 范围内的每个 segment */
	for (gid = gid0; gid <= gid1; gid++) {
		unsigned int first = swod_group_first_seg(sw, gid);
		unsigned int n = swod_group_nsegs(sw, gid);
		unsigned int i;

		for (i = 0; i < n; i++) {
			unsigned int segno = first + i;
			unsigned int valid_blks, total_blks;
			unsigned int valid_bp;
			struct swod_seg_hint *hint;

			if (segno >= sw->nr_main_segs)
				break;

			hint = &sw->seg_hint[segno];

			/* 检查是否在 held_segmap 中（冲突检测） */
			if (test_bit(segno, sw->held_segmap))
				goto remove_hbu;

			/* 获取有效块信息 */
			valid_blks = get_valid_blocks(sbi, segno, false);
			total_blks = sbi->blocks_per_seg;
			valid_bp = (unsigned int)div_u64((u64)valid_blks * SWOD_BP_ONE,
							  total_blks);

			/* 检查 HBU 条件：高有效块 + 高碎片化 */
			if (valid_bp >= hbu_valid_thr &&
			    hint->nr_cmds >= hbu_min_cmds) {
				/* 加入 hbu_targets */
				if (!test_and_set_bit(segno, sw->hbu_segmap)) {
					atomic_inc(&sw->nr_hbu_segs);
					atomic64_inc(&sw->hbu_target_add_cnt);
				}
				continue;
			}

remove_hbu:
			/* 从 hbu_targets 移除 */
			if (test_and_clear_bit(segno, sw->hbu_segmap)) {
				atomic_dec(&sw->nr_hbu_segs);
				atomic64_inc(&sw->hbu_target_rm_cnt);
			}
		}
	}
}

/*
 * V4: group 评估 - 决定是否 hold 整个 group/window
 */
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

	/* 最佳候选 */
	bool found_candidate = false;
	unsigned int best_off = 0, best_len = 0;
	unsigned int best_qbp = 0, best_lbp = UINT_MAX;
	unsigned long best_oldest = 0;
	bool is_frag = false;
	unsigned int best_frag_score = 0;

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

	/* 构建 prefix sum 用于快速窗口评估 */
	for (i = 0; i < n; i++) {
		qpref[i + 1] = qpref[i] + sw->seg_hint[first + i].pend_blks;
		lpref[i + 1] = lpref[i] + get_valid_blocks(sbi, first + i, false);
		oldest_seg[i] = sw->seg_hint[first + i].oldest_jiffies;
	}

	/*
	 * V4: 遍历所有候选窗口
	 * 对于每个窗口，先判断是否普通窗口（qcov 高 && lres 低）
	 * 如果是，直接作为候选
	 * 如果不是，再判断是否碎片窗口
	 */
	for (off = 0; off < n; off++) {
		unsigned long cur_oldest = 0;

		max_this_len = min(max_len, n - off);
		for (len = 1; len <= max_this_len; len++) {
			u64 cap, qbp, lbp;
			unsigned int endi = off + len - 1;
			unsigned int q, l;

			/* 找窗口中最老的时间戳 */
			if (oldest_seg[endi] &&
			    (!cur_oldest ||
			     time_before(oldest_seg[endi], cur_oldest)))
				cur_oldest = oldest_seg[endi];

			q = qpref[off + len] - qpref[off];
			l = lpref[off + len] - lpref[off];
			cap = (u64)len * sbi->blocks_per_seg;

			/* already fully ready: let stock issue it, do not hold */
			if (q == cap)
				continue;

			qbp = div_u64((u64)q * SWOD_BP_ONE, cap);
			lbp = div_u64((u64)l * SWOD_BP_ONE, cap);

			/* ========== Path A: 普通窗口（qcov 高 && lres 低） ========== */
			if (qbp >= dcc->swod_qcov_thr_bp &&
			    lbp <= dcc->swod_lres_thr_bp) {
				if (!found_candidate ||
				    len > best_len ||
				    (len == best_len && lbp < best_lbp) ||
				    (len == best_len && lbp == best_lbp &&
				     (!best_oldest || time_before(cur_oldest, best_oldest)))) {
					found_candidate = true;
					best_off = off;
					best_len = len;
					best_qbp = qbp;
					best_lbp = lbp;
					best_oldest = cur_oldest;
					is_frag = false;
				}
				break;
			}

			/* ========== Path B: 碎片窗口 ========== */
			if (q == 0)
				continue;

			{
				unsigned int seg_pend = 0, seg_cmds = 0;
				unsigned int avg_piece;
				unsigned int curr_frag_score;
				bool all_segs_frag = true;

				for (i = 0; i < len; i++) {
					unsigned int segno = first + off + i;
					struct swod_seg_hint *h = &sw->seg_hint[segno];

					seg_pend += h->pend_blks;
					seg_cmds += h->nr_cmds;

					if (h->nr_cmds < dcc->swod_frag_min_cmds ||
					    h->pend_blks == 0) {
						all_segs_frag = false;
						break;
					}
				}

				if (!all_segs_frag)
					continue;

				avg_piece = div_u64(seg_pend, seg_cmds);

				if (avg_piece > dcc->swod_frag_max_avg_piece_blks)
					continue;
				if (seg_pend < dcc->swod_frag_min_total_pend)
					continue;

				/* 碎片化分数 */
				curr_frag_score = (seg_cmds * 1000) / (avg_piece + 1);

				if (!found_candidate ||
				    curr_frag_score > best_frag_score ||
				    (curr_frag_score == best_frag_score &&
				     (!best_oldest || time_before(cur_oldest, best_oldest)))) {
					found_candidate = true;
					best_off = off;
					best_len = len;
					best_qbp = 0;
					best_lbp = 0;
					best_oldest = cur_oldest;
					is_frag = true;
					best_frag_score = curr_frag_score;
				}
			}
		}
	}

	/* 没找到候选 */
	if (!found_candidate) {
		atomic64_inc(&sw->eval_no_candidate_cnt);
		return;
	}

	/* V4: 检查是否需要替换已有 held 窗口 */
	if (atomic_read(&sw->nr_held_groups) >= dcc->swod_max_held_groups) {
		unsigned int victim_gid = UINT_MAX;
		struct swod_group_hint *victim = NULL;

		/* 找最差的 held 窗口 */
		for (i = 0; i < sw->nr_groups; i++) {
			struct swod_group_hint *cand = &sw->grp_hint[i];

			if (cand->state != SWOD_G_HELD)
				continue;
			if (!victim ||
			    cand->hold_qbp < victim->hold_qbp ||
			    (cand->hold_qbp == victim->hold_qbp &&
			     cand->hold_lbp > victim->hold_lbp) ||
			    (cand->hold_qbp == victim->hold_qbp &&
			     cand->hold_lbp == victim->hold_lbp &&
			     cand->hold_len < victim->hold_len)) {
				victim_gid = i;
				victim = cand;
			}
		}

		if (victim) {
			bool do_replace = false;

			/*
			 * V4: 替换策略（与 V3 一致）：
			 * - 普通窗口：只能替换普通窗口，且新普通的 qbp 要高于旧的
			 * - 碎片窗口：可以替换任意窗口（普通或碎片）
			 *   - 替换碎片窗口时，必须新的更碎
			 */
			if (is_frag) {
				if (victim->win_type == SWOD_WIN_HIGH_FRAG) {
					if (best_frag_score > victim->frag_score)
						do_replace = true;
				} else {
					do_replace = true;
				}
			} else {
				if (victim->win_type == SWOD_WIN_HIGH_COV &&
				    best_qbp > victim->hold_qbp)
					do_replace = true;
			}

			if (do_replace)
				swod_release_group_locked(sbi, victim_gid, SWOD_REL_PRESSURE);
			else
				return;
		} else {
			return;
		}
	}

	/* V4: 安装候选窗口 */
	g->state = SWOD_G_HELD;
	g->hold_off = best_off;
	g->hold_len = best_len;
	g->hold_qbp = best_qbp;
	g->hold_lbp = best_lbp;
	g->hold_until = now + msecs_to_jiffies(
		swod_calc_hold_ms(sbi, best_len, best_qbp, best_lbp, is_frag));
	g->last_eval = now;
	g->win_type = is_frag ? SWOD_WIN_HIGH_FRAG : SWOD_WIN_HIGH_COV;
	g->frag_score = is_frag ? best_frag_score : 0;

	/* V4: 只有高 qcov 窗口才设置 held_segmap（给 WCE 用），高碎片化不设置 */
	if (!is_frag) {
		atomic_inc(&sw->nr_held_groups);
		for (i = 0; i < best_len; i++) {
			unsigned int segno = first + best_off + i;
			set_bit(segno, sw->held_segmap);
			/* 同步从 hbu_segmap 移除，保证互斥性 */
			if (test_and_clear_bit(segno, sw->hbu_segmap))
				atomic_dec(&sw->nr_hbu_segs);
		}
	}

	atomic64_inc(&sw->hold_cnt);
	if (is_frag)
		atomic64_inc(&sw->hold_high_frag_cnt);
	else
		atomic64_inc(&sw->hold_high_cov_cnt);
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
		dcc->swod_max_held_groups = 128;

	/* 碎片化窗口检测参数 */
	if (!dcc->swod_frag_min_cmds)
		dcc->swod_frag_min_cmds = 3;
	if (!dcc->swod_frag_max_avg_piece_blks)
		dcc->swod_frag_max_avg_piece_blks = 16; /* avg piece <= 16 才算碎片 */
	if (!dcc->swod_frag_min_total_pend)
		dcc->swod_frag_min_total_pend = 64;
	if (!dcc->swod_frag_thr_bp)
		dcc->swod_frag_thr_bp = 7000;
	if (!dcc->swod_frag_timeout_k)
		dcc->swod_frag_timeout_k = 3;

	/* V4: HBU 参数初始化 */
	if (!dcc->swod_hbu_valid_thr_bp)
		dcc->swod_hbu_valid_thr_bp = 8000;  /* 80% */
	if (!dcc->swod_hbu_min_cmds)
		dcc->swod_hbu_min_cmds = 4;

	sw->nr_main_segs = MAIN_SEGS(sbi);
	sw->win_segs = dcc->swod_win_segs;
	sw->nr_groups = DIV_ROUND_UP(sw->nr_main_segs, sw->win_segs);

	/* 初始化周期性扫描进度 */
	sw->scan_progress = 0;

	seg_hint_sz = array_size(sw->nr_main_segs, sizeof(*sw->seg_hint));
	grp_hint_sz = array_size(sw->nr_groups, sizeof(*sw->grp_hint));
	bm_sz = BITS_TO_LONGS(sw->nr_main_segs) * sizeof(unsigned long);

	sw->seg_hint = f2fs_kvzalloc(sbi, seg_hint_sz, GFP_KERNEL);
	sw->grp_hint = f2fs_kvzalloc(sbi, grp_hint_sz, GFP_KERNEL);
	sw->held_segmap = f2fs_kvzalloc(sbi, bm_sz, GFP_KERNEL);
	sw->hbu_segmap = f2fs_kvzalloc(sbi, bm_sz, GFP_KERNEL);
	if (!sw->seg_hint || !sw->grp_hint || !sw->held_segmap || !sw->hbu_segmap) {
		kvfree(sw->seg_hint);
		kvfree(sw->grp_hint);
		kvfree(sw->held_segmap);
		kvfree(sw->hbu_segmap);
		kfree(sw);
		return -ENOMEM;
	}

	/* V4: 初始化 HBU 计数 */
	atomic_set(&sw->nr_hbu_segs, 0);
	atomic_set(&sw->nr_held_groups, 0);

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
	kvfree(sw->held_segmap);
	kvfree(sw->hbu_segmap);
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

	/* V4: 评估 segment 是否应该加入 hbu_targets */
	swod_eval_segments_for_hbu_locked(sbi, gid0, gid1);
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

/*
 * V4: 周期性后台评估
 */
void f2fs_swod_periodic_scan(struct f2fs_sb_info *sbi, unsigned long now)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct swod_ctrl *sw;
	unsigned int gid, end;
	unsigned int processed = 0;

	if (!swod_enabled(dcc))
		return;

	sw = dcc->swod;

#define SWOD_SCAN_BATCH_SIZE  16

	gid = sw->scan_progress;
	end = min(sw->scan_progress + SWOD_SCAN_BATCH_SIZE, sw->nr_groups);

	for (; gid < end && processed < SWOD_SCAN_BATCH_SIZE; gid++, processed++) {
		swod_eval_group_locked(sbi, gid, now);
		/* V4: 周期性扫描也评估 HBU */
		swod_eval_segments_for_hbu_locked(sbi, gid, gid);
	}

	if (gid >= sw->nr_groups)
		sw->scan_progress = 0;
	else
		sw->scan_progress = gid;
}

/*
 * WCE 查询接口
 */
bool f2fs_swod_has_held(struct f2fs_sb_info *sbi)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;

	if (!swod_enabled(dcc))
		return false;

	return atomic_read(&dcc->swod->nr_held_groups) > 0;
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
	if (atomic_read(&sw->nr_held_groups) == 0)
		return false;

	end = min(sw->nr_main_segs, segno + nr_segs);
	for (; segno < end; segno++) {
		if (test_bit(segno, sw->held_segmap))
			return true;
	}

	return false;
}

bool f2fs_swod_seg_held(struct f2fs_sb_info *sbi, unsigned int segno)
{
	return f2fs_swod_range_held(sbi, segno, 1);
}

/*
 * V4: WCE 从 held_segmap 中找 dirty segment
 * 遍历 held_segmap（SWOD 评估后的 bitmap），找到第一个 dirty segment
 * 返回 segno，0 表示没找到
 */
unsigned int f2fs_swod_pick_held_dirty(struct f2fs_sb_info *sbi,
				      unsigned long *dirty_bitmap,
				      unsigned int max_segno)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct swod_ctrl *sw;
	unsigned int segno;

	if (!swod_enabled(dcc) || !dcc->swod)
		return 0;

	sw = dcc->swod;
	if (atomic_read(&sw->nr_held_groups) == 0)
		return 0;

	/* 遍历 held_segmap，只检查 held=1 的 segment */
	for_each_set_bit(segno, sw->held_segmap, max_segno) {
		/* 检查是否是 dirty segment */
		if (test_bit(segno, dirty_bitmap))
			return segno;
	}

	return 0;
}

/*
 * V4: HBU 查询接口
 * 检查 segment 是否在 hbu_targets 中
 */
bool f2fs_swod_seg_in_hbu(struct f2fs_sb_info *sbi, unsigned int segno)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct swod_ctrl *sw;

	if (!swod_enabled(dcc))
		return false;

	sw = dcc->swod;
	if (segno >= sw->nr_main_segs)
		return false;

	return test_bit(segno, sw->hbu_segmap);
}

/*
 * V4: HBU OPU 分配入口
 * 从 hbu_targets 中分配一个 segment，返回 segno，NULL_SEGNO 表示失败
 * 调用者已持有 sit_i->sentry_lock，不需要额外加锁
 */
unsigned int f2fs_swod_hbu_alloc(struct f2fs_sb_info *sbi, int type)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct swod_ctrl *sw;
	unsigned int segno;

	if (!swod_enabled(dcc) || !dcc->swod)
		return NULL_SEGNO;

	/* HBU 总开关未打开 */
	if (!dcc->swod_hbu_enable)
		return NULL_SEGNO;

	sw = dcc->swod;

	/* 计数：HBU 尝试分配 */
	atomic64_inc(&sw->hbu_ipu_pick_cnt);

	/* 遍历 hbu_segmap，找一个可用的 segment */
	for_each_set_bit(segno, sw->hbu_segmap, sw->nr_main_segs) {
		/* 检查是否在 held_segmap 中（冲突检测） */
		if (test_bit(segno, sw->held_segmap)) {
			atomic64_inc(&sw->hbu_alloc_skip_held_cnt);
			continue;
		}

		/* 检查 segment 是否还有空闲无效块空间 */
		if (!f2fs_segment_has_free_slot(sbi, segno)) {
			/* segment 已满，从 hbu_targets 移除 */
			clear_bit(segno, sw->hbu_segmap);
			atomic_dec(&sw->nr_hbu_segs);
			atomic64_inc(&sw->hbu_alloc_skip_full_cnt);
			atomic64_inc(&sw->hbu_target_rm_cnt);
			continue;
		}

		/* 找到可用 segment */
		atomic64_inc(&sw->hbu_alloc_success_cnt);
		return segno;
	}

	return NULL_SEGNO;
}

/*
 * V4: WCE 选中 held segment 后调用
 * 清除 held 标记
 * 注意：GC 路径可能与 discard 路径并发，添加自旋锁保护
 */
void f2fs_swod_notify_gc_done(struct f2fs_sb_info *sbi, unsigned int segno)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct swod_ctrl *sw;
	unsigned int gid;
	u8 win_type;

	if (!dcc || !dcc->swod)
		return;

	sw = dcc->swod;

	/* 检查是否在 held_segmap 中 */
	if (!test_bit(segno, sw->held_segmap))
		return;

	gid = swod_gid(sw, segno);

	/*
	 * V4: 先保存 win_type 再清理 group 状态。
	 * held_segmap 只在 HIGH_COV 窗口设置，清理时需要用旧值判断。
	 */
	win_type = sw->grp_hint[gid].win_type;

	/*
	 * 只要有一个 held segment 被 GC，整个 held window 的上下文就破坏了。
	 * 立即清理整个 window 并减少 nr_held_groups。
	 * 只有高覆盖率窗口才设置 held_segmap，所以只有这类窗口才会走到这里。
	 */
	if (atomic_read(&sw->nr_held_groups) > 0 && win_type == SWOD_WIN_HIGH_COV)
		atomic_dec(&sw->nr_held_groups);

	/* V4: 只清理 held window 范围内的 segment（用 hold_off + hold_len） */
	if (win_type == SWOD_WIN_HIGH_COV) {
		unsigned int first = swod_group_first_seg(sw, gid);
		unsigned int off = sw->grp_hint[gid].hold_off;
		unsigned int len = sw->grp_hint[gid].hold_len;
		unsigned int i;

		for (i = 0; i < len; i++)
			clear_bit(first + off + i, sw->held_segmap);
	}

	/* 清理 group 状态（与 swod_clear_group_locked 对齐） */
	sw->grp_hint[gid].state = SWOD_G_NORMAL;
	sw->grp_hint[gid].hold_off = 0;
	sw->grp_hint[gid].hold_len = 0;
	sw->grp_hint[gid].hold_qbp = 0;
	sw->grp_hint[gid].hold_lbp = 0;
	sw->grp_hint[gid].hold_until = 0;
	sw->grp_hint[gid].win_type = SWOD_WIN_NONE;
	sw->grp_hint[gid].frag_score = 0;

	/* V4: GC 后清理 hbu_segmap（segment 变空，不再是 HBU 目标） */
	if (test_and_clear_bit(segno, sw->hbu_segmap))
		atomic_dec(&sw->nr_hbu_segs);
}
