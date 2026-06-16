/* fs/f2fs/swod.h */
#ifndef __F2FS_SWOD_H
#define __F2FS_SWOD_H

#include <linux/f2fs_fs.h>
#include "f2fs.h"

#define SWOD_BP_ONE                10000U
#define SWOD_MAX_WIN_SEGS          16
#define SWOD_WCE_PARK_MIN_MS       500U
#define SWOD_WCE_PARK_MAX_MS       2000U
#define SWOD_WCE_PARK_LRES_CAP_BP  2000U

enum swod_group_state {
	SWOD_G_NORMAL = 0,
	SWOD_G_HELD   = 1,
	SWOD_G_PARKED = 2,
};

enum swod_action {
	SWOD_ACT_BYPASS = 0,
	SWOD_ACT_SHORT_HOLD,
	SWOD_ACT_COMPLETION_HOLD,
};

enum swod_release_reason {
	SWOD_REL_SUCCESS = 0,
	SWOD_REL_TIMEOUT,
	SWOD_REL_PRESSURE,
	SWOD_REL_CAPTURE,       /* formation-aware: captured follow-up */
	SWOD_REL_MATURE,        /* formation-aware: window matured */
	SWOD_REL_BYPASS,        /* formation-aware: no growth or too large */
};

struct swod_seg_hint {
	unsigned int pend_blks;          /* D_PREP-visible pending blocks in seg */
	unsigned int nr_cmds;            /* number of D_PREP cmds touching seg */
	unsigned long oldest_jiffies;    /* oldest enqueue time among D_PREP cmds */

	/* new: formation tracking */
	unsigned int last_pend_blks;
	unsigned int last_nr_cmds;
	unsigned long last_sample_jiffies;
};

struct swod_group_hint {
	u8 state;                        /* NORMAL / HELD / PARKED */
	u8 action;                       /* BYPASS / SHORT_HOLD / COMPLETION_HOLD */
	u8 wce_eligible;                 /* only COMPLETION_HOLD with high maturity */
	u16 hold_off;                    /* offset inside group */
	u16 hold_len;                    /* nr segs of held sub-window */
	u16 hold_qbp;                    /* qcov score of held/parked window */
	u16 hold_lbp;                    /* lres score of held/parked window */
	u32 park_score;                  /* priority once handed to WCE */
	unsigned long hold_until;        /* jiffies deadline for SWOD hold */
	unsigned long park_until;        /* jiffies deadline for WCE parked hold */
	unsigned long last_eval;         /* optional debug */

	/* formation snapshot at hold start */
	u16 start_qbp;
	u16 start_lbp;
	u16 start_nr_cmds;
	u16 start_avg_piece;

	/* latest evaluated snapshot */
	u16 last_qbp;
	u16 last_lbp;
	u16 last_nr_cmds;
	u16 last_avg_piece;
	s16 last_q_growth;       /* qcov delta in bp */
	s16 last_cmd_growth;     /* cmd count delta */
};

struct swod_ctrl {
	struct f2fs_sb_info *sbi;          /* backpointer */
	unsigned int nr_main_segs;
	unsigned int win_segs;
	unsigned int nr_groups;
	unsigned int nr_held_groups;
	unsigned int nr_parked_groups;
	atomic_t nr_wce_groups;

	struct swod_seg_hint *seg_hint;      /* [nr_main_segs] */
	struct swod_group_hint *grp_hint;    /* [nr_groups] */

	/* fast path + interface to motive-2 mechanism */
	unsigned long *hold_segmap;          /* bitmap over segno for SWOD-held */
	unsigned long *park_segmap;          /* bitmap over segno for WCE-parked */
	unsigned long *wce_segmap;           /* bitmap for completion-eligible targets only */

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
	/* for wce */
	atomic64_t gc_pick_bg_cnt;
	atomic64_t gc_pick_fg_cnt;
	atomic64_t gc_fallback_cnt;
	/* ---------- selective fragment-IPU stats ---------- */
	atomic64_t frag_ipu_pick_cnt;
	atomic64_t frag_ipu_skip_target_cnt;
	atomic64_t frag_ipu_skip_hot_cnt;
	atomic64_t frag_ipu_skip_age_cnt;
	atomic64_t frag_ipu_skip_shape_cnt;
	/* ---------- formation-aware stats ---------- */
	atomic64_t short_hold_cnt;
	atomic64_t completion_hold_cnt;
	atomic64_t capture_followup_cnt;
	atomic64_t capture_blocks;
	atomic64_t coverage_gain_blocks;
	atomic64_t start_nr_cmds_sum;
	atomic64_t release_nr_cmds_sum;
	atomic64_t wce_eligible_cnt;
	atomic64_t wce_blocked_low_maturity_cnt;
	atomic64_t wce_blocked_high_lres_cnt;
	atomic64_t overlap_bypass_cnt;
	atomic64_t policy_bypass_cnt;
	atomic64_t capture_release_cnt;
	atomic64_t mature_release_cnt;
	atomic64_t bypass_release_cnt;

	/* V6: background eval task */
	struct delayed_work eval_work;      /* periodic eval task */
	unsigned int eval_batch_size;       /* groups per eval batch */
	unsigned int eval_next_gid;         /* round-robin cursor */
	atomic_t eval_running;             /* prevent concurrent eval */
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

/* Check if new cmd lands inside a held window, mark for skip at create time */
bool f2fs_swod_mark_held_cmd(struct f2fs_sb_info *sbi, block_t lstart, block_t len);

bool f2fs_swod_has_held(struct f2fs_sb_info *sbi);
bool f2fs_swod_range_held(struct f2fs_sb_info *sbi, unsigned int segno,
			  unsigned int nr_segs);

bool f2fs_swod_has_wce_target(struct f2fs_sb_info *sbi);
bool f2fs_swod_range_wce_target(struct f2fs_sb_info *sbi,
				unsigned int segno,
				unsigned int nr_segs);
bool f2fs_swod_seg_wce_eligible(struct f2fs_sb_info *sbi, unsigned int segno);

bool f2fs_swod_seg_held(struct f2fs_sb_info *sbi, unsigned int segno);

bool f2fs_swod_should_frag_ipu(struct inode *inode,
			       struct f2fs_io_info *fio);

/* V6: background eval interface */
void f2fs_swod_eval_work(struct work_struct *work);
void f2fs_swod_queue_eval(struct f2fs_sb_info *sbi);
void f2fs_swod_cancel_eval(struct f2fs_sb_info *sbi);
#endif
