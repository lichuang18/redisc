/* fs/f2fs/swod.h */
#ifndef __F2FS_SWOD_H
#define __F2FS_SWOD_H

#include <linux/f2fs_fs.h>
#include "f2fs.h"

#define SWOD_BP_ONE          10000U
#define SWOD_MAX_WIN_SEGS    16

enum swod_group_state {
	SWOD_G_NORMAL = 0,
	SWOD_G_HELD   = 1,
};

enum swod_release_reason {
	SWOD_REL_SUCCESS = 0,
	SWOD_REL_TIMEOUT,
	SWOD_REL_PRESSURE,
};

struct swod_seg_hint {
	unsigned int pend_blks;          /* D_PREP-visible pending blocks in seg */
	unsigned int nr_cmds;            /* number of D_PREP cmds touching seg */
	unsigned long oldest_jiffies;    /* oldest enqueue time among D_PREP cmds */
};

struct swod_group_hint {
	u8 state;                        /* NORMAL / HELD */
	u16 hold_off;                    /* offset inside group */
	u16 hold_len;                    /* nr segs of held sub-window */
	u16 hold_qbp;                    /* qcov score of held window */
	u16 hold_lbp;                    /* lres score of held window */
	unsigned long hold_until;        /* jiffies deadline */
	unsigned long last_eval;         /* optional debug */
};

struct swod_ctrl {
	unsigned int nr_main_segs;
	unsigned int win_segs;
	unsigned int nr_groups;
	unsigned int nr_held_groups;

	struct swod_seg_hint *seg_hint;      /* [nr_main_segs] */
	struct swod_group_hint *grp_hint;    /* [nr_groups] */

	/* fast path + interface to motive-2 mechanism */
	unsigned long *hold_segmap;          /* bitmap over segno */

	atomic64_t hold_cnt;
	atomic64_t skip_cnt;
	atomic64_t skip_check_cnt;
	atomic64_t skip_miss_noheld_cnt;
	atomic64_t skip_miss_timeout_cnt;
	atomic64_t skip_miss_success_cnt;
	atomic64_t skip_miss_overlap_cnt;
	atomic64_t eval_blocked_cnt;
	atomic64_t eval_no_candidate_cnt;
	atomic64_t success_release_cnt;
	atomic64_t timeout_release_cnt;
	atomic64_t pressure_release_cnt;
    // for wce
    atomic64_t gc_pick_bg_cnt;
	atomic64_t gc_pick_fg_cnt;
	atomic64_t gc_fallback_cnt;
    /* ---------- selective fragment-IPU stats ---------- */
	atomic64_t frag_ipu_pick_cnt;
	atomic64_t frag_ipu_skip_target_cnt;
	atomic64_t frag_ipu_skip_hot_cnt;
	atomic64_t frag_ipu_skip_age_cnt;
	atomic64_t frag_ipu_skip_shape_cnt;
};

int  f2fs_swod_init(struct f2fs_sb_info *sbi);
void f2fs_swod_destroy(struct f2fs_sb_info *sbi);

void f2fs_swod_refresh_around_locked(struct f2fs_sb_info *sbi,
				     block_t lstart, block_t len);

bool f2fs_swod_should_skip_locked(struct f2fs_sb_info *sbi,
				  struct discard_cmd *dc,
				  struct discard_policy *dpolicy,
				  unsigned long now);

void f2fs_swod_release_all(struct f2fs_sb_info *sbi,
			   enum swod_release_reason why);
void f2fs_swod_sweep_timeout(struct f2fs_sb_info *sbi, unsigned long now);

bool f2fs_swod_has_held(struct f2fs_sb_info *sbi);
bool f2fs_swod_range_held(struct f2fs_sb_info *sbi, unsigned int segno,
			  unsigned int nr_segs);

bool f2fs_swod_seg_held(struct f2fs_sb_info *sbi, unsigned int segno);

bool f2fs_swod_should_frag_ipu(struct inode *inode,
			       struct f2fs_io_info *fio);
#endif