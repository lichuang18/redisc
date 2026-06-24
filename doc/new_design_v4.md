# ReDisc 新设计方案 (V4)

## 0. 核心设计思想

**SWOD 一次评估，同时产出两个互斥的候选集合**：

| 集合 | 筛选条件 | 用途 |
|------|----------|------|
| **hold_targets** | 低有效块 + 高 discard 覆盖率 | SWOD hold → WCE GC 回收 |
| **hbu_targets** | 高有效块 + 高碎片化 | HBU OPU（减少碎片化 discard cmd） |

```
┌─────────────────────────────────────────────────────────────┐
│                 SWOD 评估决策矩阵                          │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│         有效块比例高 (>= 80%)        有效块比例低 (<= 20%)  │
│              │                            │                 │
│              ▼                            ▼                 │
│    ┌─────────────────────┐    ┌─────────────────────┐       │
│    │   hbu_targets      │    │   hold_targets     │       │
│    │   高有效块 segment │    │   低有效块 segment │       │
│    │   + 高碎片化       │    │                     │       │
│    │                     │    │                     │       │
│    │  → HBU OPU         │    │  → SWOD hold       │       │
│    │    填满无效块空间   │    │    → WCE GC        │       │
│    │    减少 discard cmd│    │    → discard 高效  │       │
│    └─────────────────────┘    └─────────────────────┘       │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

---

## 1. SWOD 评估（核心：产生两个候选集合）

### 1.1 评估输入

每次 discard cmd 变化时，评估受影响的 segment/group。

评估使用的信息：
- `seg_hint[segno]`：该 segment 的 pending discard 信息
- `qcov`：pending discard 覆盖率
- `lres`：live residual blocks（有效块数）
- `nr_cmds`：discard cmd 数量（碎片化指标）

### 1.2 评估决策树

```
对于每个 segment/window：

  ┌──────────────────────────────────────────────────────────┐
  │                                                          │
  │  1. 有效块比例 >= HBU_VALID_THR (80%) ?                 │
  │     ├── YES → 2. 碎片化检查                            │
  │     │                                                    │
  │     └── NO → 3. discard 覆盖率 >= HOLD_QCOV_THR (85%) ? │
  │                ├── YES → 4. 碎片化程度检查              │
  │                │                                         │
  │                └── NO → 普通 segment（不处理）           │
  │                                                          │
  └──────────────────────────────────────────────────────────┘

  2. 碎片化检查（高有效块 segment）：

      ┌────────────────────────────────────────────────────┐
      │                                                     │
      │  nr_cmds >= HBU_MIN_CMDS (默认 4) ?              │
      │                                                     │
      │     ├── YES → 加入 hbu_targets                     │
      │     │        (高有效块 + 高碎片化)                  │
      │     │        HBU OPU 填满无效块空间                │
      │     │                                                │
      │     └── NO → 普通 segment（不处理）                 │
      │                                                     │
      └────────────────────────────────────────────────────┘

  3. discard 覆盖率检查（低有效块 segment）：

      ┌────────────────────────────────────────────────────┐
      │                                                     │
      │  discard 覆盖率 >= HOLD_QCOV_THR (85%) ?           │
      │                                                     │
      │     ├── NO → 普通 segment（不处理）                 │
      │     │                                                │
      │     └── YES → 4. 碎片化程度检查                    │
      │                                                     │
      └────────────────────────────────────────────────────┘

  4. 碎片化程度检查（低有效块 + 高 qcov segment）：

      ┌────────────────────────────────────────────────────┐
      │                                                     │
      │  每个 segment 的 nr_cmds >= frag_min_cmds (默认 3)  │
      │  + 平均 cmd 大小 <= avg_piece (默认 16 blocks) ?   │
      │                                                     │
      │     ├── YES → 加入 hold_targets (类型 B: 碎片化)   │
      │     │        超时时间 = base_timeout × k (k=3)     │
      │     │                                                │
      │     └── NO → 加入 hold_targets (类型 A: 高覆盖)     │
      │              超时时间 = base_timeout (50~300ms)     │
      │                                                     │
      └────────────────────────────────────────────────────┘
```

### 1.3 两类 hold_targets

| 类型 | 条件 | 超时时间 | held_segmap |
|------|------|----------|-------------|
| **A: 高覆盖率** | qcov >= 85%, lres <= 10% | 50~300ms | ✅ 设置 |
| **B: 高碎片化** | nr_cmds >= 3, avg_cmd <= 16 | 150~900ms | ❌ 不设置 |

**关键区别**：只有高覆盖率窗口才设置 `held_segmap`，高碎片化窗口不设置（但仍然被 hold，skip 生效）。

### 1.4 hbu_targets 特征

| 特征 | 阈值 | 代码对应 | 说明 |
|------|------|----------|------|
| 有效块比例 | >= 80% | `swod_hbu_valid_thr_bp` | segment 接近全满 |
| nr_cmds 数量 | >= 4 | `swod_hbu_min_cmds` | 高碎片化（多个小 discard cmd） |
| discard 状态 | 不在 held_segmap | `swod_eval_segments_for_hbu_locked()` | 没有被 SWOD hold |

---

## 2. 数据结构设计

### 2.1 结构体定义

```c
// swod.h

enum swod_window_type {
    SWOD_WIN_NONE = 0,
    SWOD_WIN_HIGH_COV,      /* 高覆盖率窗口 */
    SWOD_WIN_HIGH_FRAG,     /* 高碎片化窗口 */
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
    /* 两个互斥的 bitmap */
    unsigned long *held_segmap;          /* bitmap: 被 SWOD hold 的 segment（低有效块 + 高 qcov）*/
    unsigned long *hbu_segmap;            /* bitmap: 适合 HBU OPU 的 segment（高有效块 + 高碎片化）*/

    /* HBU 目标计数 */
    atomic_t nr_hbu_segs;

    /* 两类 hold 窗口统计 */
    atomic64_t hold_high_cov_cnt;    /* 高覆盖率 hold 次数 */
    atomic64_t hold_high_frag_cnt;   /* 高碎片化 hold 次数 */

    /* HBU 统计 */
    atomic64_t hbu_target_add_cnt;    /* 加入 hbu_targets 次数 */
    atomic64_t hbu_target_rm_cnt;     /* 从 hbu_targets 移除次数 */
    atomic64_t hbu_ipu_pick_cnt;      /* HBU OPU 分配函数被调用次数 */
    atomic64_t hbu_alloc_success_cnt; /* HBU OPU 分配成功次数 */
    atomic64_t hbu_alloc_skip_held_cnt;  /* HBU 跳过 held segment 次数 */
    atomic64_t hbu_alloc_skip_full_cnt;   /* HBU 跳过已满 segment 次数 */
};
```

### 2.2 bitmap 操作

```c
// ==================== held_segmap 操作 ====================

// 判断 segment 是否在 held 中
bool f2fs_swod_seg_held(struct f2fs_sb_info *sbi, unsigned int segno) {
    return test_bit(segno, sw->held_segmap);
}

// held_segmap 的设置/清除在 swod_eval_group_locked() 和
// swod_clear_group_locked() 中内联完成

// ==================== hbu_segmap 操作 ====================

// 评估 segment 是否加入 hbu_targets（高有效块 + 高碎片化）
// 实际函数：swod_eval_segments_for_hbu_locked()
// 条件：valid_bp >= swod_hbu_valid_thr_bp && nr_cmds >= swod_hbu_min_cmds
// 注意：会检查 held_segmap 冲突，被 held 的 segment 不会加入 hbu_targets

// 判断 segment 是否在 hbu_targets 中
bool f2fs_swod_seg_in_hbu(struct f2fs_sb_info *sbi, unsigned int segno) {
    return test_bit(segno, sw->hbu_segmap);
}
```

### 2.3 held_segmap 维护规则

| 事件 | 操作 | 说明 |
|------|------|------|
| SWOD 评估选中高 qcov 窗口 | 设置 bit + nr_held_groups++ | 进入 hold 状态，WCE 可选 |
| SWOD 评估选中高碎片化窗口 | 不设置 bit，不修改 nr_held_groups | 进入 hold 状态（skip 生效），WCE 不可选 |
| WCE 选中 held segment | 清除 bit + nr_held_groups-- | GC 回收，不再 hold |
| 超时/pressure release | 根据 win_type 清除 | 只有高覆盖率才清除 bit 和 nr_held_groups |

### 2.4 两类 bitmap 的关系

```
┌─────────────────────────────────────────────────────────────┐
│                   held_segmap 和 hbu_segmap                 │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│  held_segmap:                                               │
│  ├── 设置时机：segment 被 SWOD hold（高 qcov）            │
│  ├── 清除时机：timeout / pressure / GC 回收               │
│  ├── 使用者：WCE（GC victim 候选）                        │
│  └── 特征 segment：低有效块 + 高 qcov                     │
│                                                              │
│  hbu_segmap:                                                │
│  ├── 设置时机：SWOD 评估发现高有效块 + 高碎片化 segment   │
│  ├── 清除时机：segment 被 GC / 无空闲空间 / 被 SWOD hold  │
│  ├── 使用者：HBU（OPU 分配决策）                          │
│  └── 特征 segment：高有效块 (>= 80%) + 高碎片化 (nr_cmds >= 4) │
│                                                              │
│  ⭐ 关键约束：held_segmap 和 hbu_segmap 交集为空          │
│     因为一个 segment 要么低有效块（hold），要么高有效块（HBU）│
│                                                              │
└─────────────────────────────────────────────────────────────┘
```

---

## 3. WCE 设计（GC 目标选择）

### 3.1 目标集合

WCE 只操作 `held_segmap`（从 hold_targets 来的 segment）。

### 3.2 优先级说明

```
WCE 优先级：

  1. 优先目标：高覆盖率窗口 (held_segmap + 类型 A)
     └── 条件：discard 成本已经很低
     └── 任何时候都可以选

  2. 次要目标：高碎片化窗口（held_segmap 中不存在！）
     └── 高碎片化窗口虽然被 SWOD hold（skip 生效），但不设置 held_segmap
     └── WCE 不会主动选择它们，等待自然成熟后降级到普通处理

  3. 降级：无 held 目标时降级到 stock F2FS
```

### 3.3 实现要点

```c
// gc.c - get_victim_by_default()

// WCE：遍历 held_segmap，找第一个 dirty segment
// 只有高覆盖率窗口才设置 held_segmap，所以不需要额外检查 win_type
unsigned int segno = f2fs_swod_pick_held_dirty(sbi, dirty_bitmap, max_segno);
if (segno > 0) {
    // 找到了，进行合法性检查（sec_usage_check 等）
    // 通过则选中，否则 fallback
    atomic64_inc(&sw->gc_pick_bg_cnt);  // 或 gc_pick_fg_cnt
} else {
    // 没有找到 held dirty segment，fallback 到 stock
    fallback_to_stock_picker(sbi, gc_type, &p);
    atomic64_inc(&sw->gc_fallback_cnt);
}
```

---

## 4. HBU 设计（OPU 分配）

### 4.1 核心约束

**HBU 只在 hbu_targets 中选择 OPU 分配目标。**

关键改变：写入分配到 segment 的无效块空间（OPU），而不是原地覆盖（IPU）。

### 4.2 OPU 分配流程

```
HBU 分配决策：

  1. HBU 启用？swod_hbu_enable == 1
     └── NO → NULL_SEGNO（回退到正常分配）

  2. 从 hbu_targets bitmap 中顺序查找 segment
     └── 使用 find_first_bit / find_next_bit

  3. 检查该 segment 是否在 held_segmap 中？
     └── 是 → 跳过，找下一个 hbu_targets segment

  4. 检查 segment 是否还有空闲无效块？
     └── 否 → 从 hbu_targets 移除，找下一个

  5. 使用该 segment 进行 OPU 分配
     └── 从 segment 中找空闲无效块地址分配

  6. 如果 segment 变满，交给 SWOD 后台评估移除
```

### 4.3 HBU 分配实现（segment.c）

```c
// 在 get_new_segment() 中，当需要分配新 segment 时

if (fio && fio->temp != HOT && old_blkaddr == NULL_ADDR) {
    hbu_segno = f2fs_swod_hbu_alloc(sbi, type);
} else {
    hbu_segno = NULL_SEGNO;
}

if (hbu_segno != NULL_SEGNO) {
    /* HBU 成功：使用 HBU segment */
    unsigned int free_blkoff;
    unsigned int orig_next_segno = curseg->next_segno;

    /* 切换 curseg 到 HBU segment */
    curseg->next_segno = hbu_segno;
    reset_curseg(sbi, type, 1);
    curseg->alloc_type = LFS;

    /* 恢复 orig_next_segno，脏 segment 用完后回归主线 */
    curseg->next_segno = orig_next_segno;

    /* 找到 HBU segment 的第一个空闲块位置 */
    free_blkoff = __next_free_blkoff(sbi, curseg->segno, 0);
    curseg->next_blkoff = free_blkoff;

    /* 更新 *new_blkaddr 为 HBU segment 的空闲块 */
    *new_blkaddr = START_BLOCK(sbi, curseg->segno) + free_blkoff;
} else {
    /* HBU 失败：使用原始分配流程 */
    if (need_new_seg(sbi, type))
        new_curseg(sbi, type, false);
    else
        change_curseg(sbi, type);
    stat_inc_seg_type(sbi, curseg);
}
```

### 4.4 为什么这样设计不会冲突

| segment 状态 | held_segmap | hbu_segmap | HBU 行为 |
|--------------|-------------|------------|----------|
| 被 SWOD hold | ✅ | ❌ | HBU 跳过（找下一个） |
| 高有效块 + 高碎片 | ❌ | ✅ | HBU 分配 |
| 普通 segment | ❌ | ❌ | HBU 不参与（回退） |

**核心保证**：`held_segmap` 和 `hbu_segmap` 交集为空，因为评估时一个 segment 只能进入一个集合。

---

## 5. 评估触发与更新

### 5.1 触发时机

与现有设计一致：
- discard cmd 创建 / 合并 / punch / 提交时触发
- 周期性后台扫描（100ms）

### 5.2 评估时同时维护两个集合

```c
swod_eval_segment_locked(segno, now) {
    struct swod_seg_hint *hint = &sw->seg_hint[segno];
    unsigned int valid_ratio = get_valid_ratio(segno);
    unsigned int qcov = hint->qcov;
    unsigned int nr_cmds = hint->nr_cmds;

    // 先从旧集合移除（segment 状态可能变化）
    swod_remove_held(sw, segno);
    swod_remove_hbu_target(sw, segno);

    // 决策：高有效块 → hbu_targets（高碎片化）
    if (valid_ratio >= sw->hbu_valid_thr_bp) {  // 80%
        if (nr_cmds >= sw->swod_hbu_min_cmds) {  // >= 4
            swod_add_hbu_target(sw, segno);
        }
        return;
    }

    // 决策：低有效块 + 高 discard 覆盖率 → hold_targets
    if (qcov >= sw->hold_qcov_thr_bp) {  // 85%
        if (frag >= sw->frag_thr_bp) {  // 碎片化
            swod_add_held(sw, segno, SWOD_WIN_HIGH_FRAG, frag_timeout);
        } else {
            swod_add_held(sw, segno, SWOD_WIN_HIGH_COV, base_timeout);
        }
    }
    // 否则：普通 segment，不加入任何集合
}
```

---

## 6. sysfs 参数

### 6.1 SWOD 基础参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| swod_enable | 0 | 总开关 |
| swod_hold_min_ms | 50 | 最小 hold 时间 |
| swod_hold_max_ms | 300 | 最大 hold 时间 |
| swod_max_held_groups | 128 | 最大并发 hold 窗口数 |

### 6.2 SWOD 评估参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| swod_qcov_thr_bp | 8500 | hold qcov 阈值 (85%) |
| swod_lres_thr_bp | 1000 | hold lres 阈值 (10%) |
| swod_frag_thr_bp | 7000 | 碎片化阈值 (70%) |
| swod_frag_max_avg_piece_blks | 16 | 平均 cmd 大小阈值，碎片化判断用 |
| swod_frag_timeout_k | 3 | 碎片化超时倍数 |

### 6.3 HBU 参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| swod_hbu_enable | 0 | HBU 总开关 |
| swod_hbu_valid_thr_bp | 8000 | HBU 有效块阈值 (80%) |
| swod_hbu_min_cmds | 4 | HBU nr_cmds 阈值（碎片化指标） |

### 6.4 WCE 参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| swod_completion_enable | 0 | WCE 总开关 |
| swod_gc_bg_enable | 1 | BG GC WCE |
| swod_gc_fg_enable | 1 | FG GC WCE |

---

## 7. 统计项

### 7.1 SWOD 统计

| 统计项 | 类型 | 说明 |
|--------|------|------|
| swod_held_groups | gauge | 当前 hold 窗口数 |
| swod_hold_cnt | counter | 总 hold 次数 |
| swod_hold_high_cov_cnt | counter | 高覆盖率 hold 次数 |
| swod_hold_high_frag_cnt | counter | 高碎片化 hold 次数 |

### 7.2 HBU 统计

| 统计项 | 类型 | 说明 |
|--------|------|------|
| swod_hbu_target_cnt | gauge | hbu_targets 当前数量 |
| swod_hbu_target_add_cnt | counter | 加入 hbu_targets 次数 |
| swod_hbu_target_rm_cnt | counter | 从 hbu_targets 移除次数 |
| swod_hbu_ipu_pick_cnt | counter | HBU OPU 分配函数被调用次数 |
| swod_hbu_alloc_success_cnt | counter | HBU OPU 分配成功次数 |
| swod_hbu_alloc_skip_held_cnt | counter | 因 held_segmap 冲突跳过 |
| swod_hbu_alloc_skip_full_cnt | counter | 因 segment 满移除 |

### 7.3 WCE 统计

| 统计项 | 类型 | 说明 |
|--------|------|------|
| swod_gc_pick_bg_cnt | counter | BG GC 选中 held 次数 |
| swod_gc_pick_fg_cnt | counter | FG GC 选中 held 次数 |
| swod_gc_fallback_cnt | counter | 降级到 stock 次数 |

---

## 8. 完整流程图

```
┌─────────────────────────────────────────────────────────────────┐
│                        SWOD 评估触发                           │
│              (discard cmd 变化 或 周期性扫描)                  │
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│                     segment 评估决策                           │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  有效块比例 >= 80% ?                                            │
│       │                                                         │
│       ├── YES → nr_cmds >= 4 ?                                 │
│       │          │                                              │
│       │          ├── YES → hbu_targets                          │
│       │          │      (高有效块 + 高碎片化)                   │
│       │          │      HBU OPU 分配                            │
│       │          │                                              │
│       │          └── NO → 普通 segment（不处理）               │
│       │                                                         │
│       └── NO → qcov >= 85% ?                                   │
│                    │                                            │
│                    ├── YES → 碎片化检查                        │
│                    │         │                                  │
│                    │         ├── frag >= 70% → 高碎片化 hold    │
│                    │         │   (不设 held_segmap, WCE 不选)  │
│                    │         │                                  │
│                    │         └── frag < 70% → 高覆盖率 hold    │
│                    │             (设 held_segmap, WCE 优先选)  │
│                    │                                            │
│                    └── NO → 普通 segment（不处理）              │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
                    │                    │
                    ▼                    ▼
         ┌──────────────────┐     ┌──────────────────┐
         │   hbu_targets   │     │  held_segmap    │
         └────────┬────────┘     └────────┬────────┘
                  │                       │
                  ▼                       ▼
         ┌──────────────────┐     ┌──────────────────┐
         │   HBU 决策      │     │   WCE 决策       │
         │   (segment.c)  │     │   (gc.c)        │
         │                │     │                  │
         │ 从 hbu_targets │     │ BG/FG GC victim  │
         │ 顺序选 segment │     │ 选择 held segment │
         │                │     │                  │
         │ 检查 held      │     │ → 优先选 GC      │
         │ 冲突，跳过     │     │                  │
         │                │     │                  │
         │ → OPU 分配    │     │                  │
         └──────────────────┘     └──────────────────┘
                  │                       │
                  ▼                       ▼
         ┌──────────────────────────────────────────────────┐
         │                    最终效果                       │
         │                                                    │
         │  高有效块 + 高碎片 → HBU OPU → 填满无效块        │
         │    → nr_cmds 减少 → discard cmd 减少              │
         │                                                    │
         │  低有效块 + 高 qcov → SWOD hold → WCE GC        │
         │    → discard 高效                                  │
         │                                                    │
         └──────────────────────────────────────────────────┘
```

---

## 9. 可行性验证

### 9.1 设计完整性检查

| 检查项 | 状态 | 说明 |
|--------|------|------|
| SWOD 产生两个互斥集合 | ✅ | held_segmap 和 hbu_segmap |
| hold_targets 和 hbu_targets 互斥 | ✅ | 基于有效块比例天然互斥 |
| WCE 只操作 held_segmap | ✅ | 不影响 hbu_targets |
| HBU 只操作 hbu_segmap | ✅ | 不影响 held_segmap |
| HBU 跳过 held segment | ✅ | 冲突检测逻辑 |

### 9.2 关键不变量

1. **互斥性**：`held_segmap` 和 `hbu_segmap` 交集为空
2. **完整性**：每个 segment 评估后必属于三者之一：`held_segmap`、`hbu_segmap`、或两者都不在
3. **正确性**：HBU 只在 `hbu_segmap` 中选，WCE 只在 `held_segmap` 中选
4. **无冲突**：HBU 分配时检查 held_segmap，跳过 held segment

---

## 10. 实现计划

### Phase 1: 数据结构扩展 ✅
- [x] `sfi_segmap` 重命名为 `hbu_segmap`
- [x] `nr_sfi_segs` 重命名为 `nr_hbu_segs`
- [x] 新增 `swod_hbu_enable` 参数
- [x] 新增 `swod_hbu_valid_thr_bp` 参数
- [x] 新增 `swod_hbu_min_cmds` 参数（默认值 4）

### Phase 2: SWOD 评估重构 ✅
- [x] 修改评估逻辑：hbu_targets 条件改为 nr_cmds >= 阈值
- [x] 更新 bitmap 操作函数命名
- [x] 更新统计计数命名
- [x] 新增 `swod_frag_max_avg_piece_blks` 参数（默认值 16）

### Phase 3: HBU 分配逻辑实现 ✅
- [x] 实现 `f2fs_swod_hbu_alloc()` 函数
- [x] 集成到 segment 分配路径
- [x] 实现冲突检测（检查 held_segmap）

### Phase 4: sysfs 和统计 ✅
- [x] 添加 HBU 相关参数（`swod_hbu_enable`, `swod_hbu_valid_thr_bp`, `swod_hbu_min_cmds`）
- [x] 添加 HBU 相关统计项

---

## 11. 与原 SFI 设计的关键区别

| 方面 | 原 SFI 设计 | 新 HBU 设计 |
|------|------------|-------------|
| 名称 | SFI (Segment Fitness for IPU) | HBU (Hole Block Utilization) |
| 机制 | IPU（原地覆盖） | OPU（分配新地址到无效块空间） |
| hbu_targets 条件 | 高有效块 + **低碎片化** (nr_cmds <= 2) | 高有效块 + **高碎片化** (nr_cmds >= 4) |
| 优化目标 | 减少 discard 产生 | 减少碎片化 discard cmd |
| 分配冲突处理 | 无 | 检查 held_segmap，跳过 held segment |

---

## 12. 参考

- `/doc/new_design.md` - V3 设计（已过时）
- `/doc/debug.md` - Bug 修复记录
- `/readme.md` - 设计总览
- `/desgin.md` - 设计动机
