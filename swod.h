/* fs/f2fs/swod.h */
#ifndef __F2FS_SWOD_H
#define __F2FS_SWOD_H

#include <linux/f2fs_fs.h>
#include "f2fs.h"

#define SWOD_BP_ONE                10000U
#define SWOD_MAX_WIN_SEGS          16

/* V4: 窗口类型枚举 */
enum swod_window_type {
	SWOD_WIN_NONE = 0,
	SWOD_WIN_HIGH_COV,      /* 高覆盖率窗口 */
	SWOD_WIN_HIGH_FRAG,     /* 高碎片化窗口 */
};

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
	/* V4: segment 级别的碎片化评估 */
	unsigned int frag_score;         /* 碎片化分数 */
	bool is_frag;                   /* 是否碎片化 */
};

struct swod_group_hint {
	u8 state;                        /* NORMAL / HELD */
	u16 hold_off;                    /* offset inside group */
	u16 hold_len;                    /* nr segs of held sub-window */
	u16 hold_qbp;                    /* qcov score of held window */
	u16 hold_lbp;                    /* lres score of held window */
	unsigned long hold_until;        /* jiffies deadline for SWOD hold */
	unsigned long last_eval;         /* optional debug */
	/* V4: 窗口类型 */
	u8 win_type;                     /* WIN_NONE / WIN_HIGH_COV / WIN_HIGH_FRAG */
	u16 frag_score;                  /* 碎片化分数（avg_piece），普通窗口为 0 */
};

struct swod_ctrl {
	unsigned int nr_main_segs;
	unsigned int win_segs;
	unsigned int nr_groups;
	atomic_t nr_held_groups;              /* atomic: 当前 held 窗口数 */
	unsigned int scan_progress;        /* 周期性扫描进度 */

	struct swod_seg_hint *seg_hint;      /* [nr_main_segs] */
	struct swod_group_hint *grp_hint;    /* [nr_groups] */

	/* 两个互斥的 bitmap */
	unsigned long *held_segmap;          /* bitmap: 被 SWOD hold 的 segment（低有效块 + 高 qcov）*/
	unsigned long *hbu_segmap;            /* bitmap: 适合 HBU OPU 的 segment（高有效块 + 高碎片化）*/

	/* HBU 目标计数 */
	atomic_t nr_hbu_segs;

	/* SWOD 统计 */
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

	/* V4: 两类 hold 窗口统计 */
	atomic64_t hold_high_cov_cnt;    /* 高覆盖率 hold 次数 */
	atomic64_t hold_high_frag_cnt;   /* 高碎片化 hold 次数 */

	/* WCE 统计 */
	atomic64_t gc_pick_bg_cnt;
	atomic64_t gc_pick_fg_cnt;
	atomic64_t gc_fallback_cnt;       /* 降级到 stock 次数 */

	/* HBU 统计 */
	atomic64_t hbu_target_add_cnt;    /* 加入 hbu_targets 次数 */
	atomic64_t hbu_target_rm_cnt;     /* 从 hbu_targets 移除次数 */
	atomic64_t hbu_ipu_pick_cnt;      /* HBU OPU 分配函数被调用次数 */
	atomic64_t hbu_alloc_success_cnt; /* HBU OPU 分配成功次数 */
	atomic64_t hbu_alloc_skip_held_cnt;  /* HBU 跳过 held segment 次数 */
	atomic64_t hbu_alloc_skip_full_cnt;   /* HBU 跳过已满 segment 次数 */
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

/* 周期性后台评估：扫描所有 group，找新的候选窗口 */
void f2fs_swod_periodic_scan(struct f2fs_sb_info *sbi, unsigned long now);

/* WCE 查询接口 */
bool f2fs_swod_has_held(struct f2fs_sb_info *sbi);
bool f2fs_swod_range_held(struct f2fs_sb_info *sbi, unsigned int segno,
			  unsigned int nr_segs);
bool f2fs_swod_seg_held(struct f2fs_sb_info *sbi, unsigned int segno);

/* V4: WCE 从 held_segmap 中找 dirty segment */
unsigned int f2fs_swod_pick_held_dirty(struct f2fs_sb_info *sbi,
				      unsigned long *dirty_bitmap,
				      unsigned int max_segno);

/* V4: WCE 选中 held segment 后调用，清除 held 标记 */
void f2fs_swod_notify_gc_done(struct f2fs_sb_info *sbi, unsigned int segno);

/* V4: HBU OPU 分配决策 */
bool f2fs_swod_seg_in_hbu(struct f2fs_sb_info *sbi, unsigned int segno);

/* V4: HBU OPU 分配入口 - 从 hbu_targets 分配 segment，返回 segno，0 表示失败 */
unsigned int f2fs_swod_hbu_alloc(struct f2fs_sb_info *sbi, int type);

#endif
