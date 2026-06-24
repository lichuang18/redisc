# V4 设计实现调试记录

## 未修复问题

### 1. nr_held_groups 的竞态（A 延续）

**位置**：`swod.c:178-180`、`swod.c:1124-1125`

**问题代码**：
```c
// swod_clear_group_locked (swod.c:178-180)
if (atomic_read(&sw->nr_held_groups) &&
    win_type == SWOD_WIN_HIGH_COV)
    atomic_dec(&sw->nr_held_groups);

// f2fs_swod_notify_gc_done (swod.c:1124-1125)
if (atomic_read(&sw->nr_held_groups) > 0 && win_type == SWOD_WIN_HIGH_COV)
    atomic_dec(&sw->nr_held_groups);
```

**问题分析**：
- `atomic_read` 后跟 `atomic_dec` 不是原子的
- 竞态场景：
  ```
  时刻    线程A (clear_group)        线程B (notify_gc_done)
  t1     atomic_read = 1
  t2                                 atomic_read = 1
  t3     atomic_dec → 0
  t4                                 atomic_dec → -1 (欠债)
  ```
- 影响：`nr_held_groups` 可能变成负数，导致决策错误

**风险评估**：中等。正常情况下 `swod_clear_group_locked` 和 `f2fs_swod_notify_gc_done` 不会同时作用于同一个 group，因为 held_segmap 的 set/clear 操作会互斥。但如果 GC 和 periodic scan 或 pressure release 并发，仍可能触发。

---

### 2. nr_hbu_segs 的竞态（B 延续）

**位置**：`swod.c:408-410`、`swod.c:660-661`、`swod.c:1076`

**问题分析**：
- 部分位置使用 `test_and_clear_bit` + `atomic_dec`：
  ```c
  // swod_eval_segments_for_hbu_locked (swod.c:408-410)
  if (test_and_clear_bit(segno, sw->hbu_segmap)) {
      atomic_dec(&sw->nr_hbu_segs);  // 非原子
      atomic64_inc(&sw->hbu_target_rm_cnt);
  }
  ```
- `test_and_clear_bit` 是原子的，但后续的 `atomic_dec` 不是
- 如果多个路径同时移除同一个 segment 的 hbu bit，`atomic_dec` 可能多次执行

**影响**：统计不准确，但不影响核心逻辑（hbu_segmap 的 set/clear 本身是正确的）

**风险评估**：低。HBU 逻辑主要是优化性的，统计偏差不影响功能正确性。

---

### 3. held_segmap/hbu_segmap 读写竞态（C 延续）

**位置**：
- `swod_eval_segments_for_hbu_locked()`：读 `held_segmap` 并写 `hbu_segmap`
- `swod_eval_group_locked()`：写 `held_segmap` 并可能清理 `hbu_segmap`
- `f2fs_swod_hbu_alloc()`：读 `held_segmap` 并读 `hbu_segmap`

**问题分析**：
```
时间线：
CPU0 (discard/periodic_scan)              CPU1 (HBU 分配)
----------------------------------------
swod_eval_segments_for_hbu_locked:
  test_bit(A, held_segmap) → 0
  // 此时 A 不在 held
                                          f2fs_swod_hbu_alloc:
                                          test_bit(A, held_segmap) → 0
                                          return A (但实际 A 即将被 held)

  set_bit(A, hbu_segmap)
  // A 同时在两个 bitmap！

swod_eval_group_locked:
  set_bit(A, held_segmap)  // 冲突！

// 后续 swod_eval_group_locked 会从 hbu_segmap 移除冲突的 A
```

**影响**：短时间内可能将 segment 同时放入两个 segmap，但会快速纠正。
- 如果此时 HBU 分配到该 segment，可能浪费机会
- 但不会导致数据损坏

**风险评估**：低。这是 "last writer wins" 场景，最终状态正确，短期不一致是设计上的已知窗口。

---

### 4. scan_progress 竞态（D 延续）

**位置**：`swod.c:934-946`

**问题代码**：
```c
gid = sw->scan_progress;  // 无保护读
// ...
if (gid >= sw->nr_groups)
    sw->scan_progress = 0;  // 无保护写
else
    sw->scan_progress = gid;
```

**问题分析**：
- 读-修改-写 操作不是原子的
- 如果多个 CPU 同时调用 `f2fs_swod_periodic_scan`：
  ```
  CPU0: read gid=10, CPU1: read gid=10
  CPU0: write gid=15, CPU1: write gid=15
  // 进度正确，但有重复评估（本次扫描中 group 10 被评估两次）
  ```
- 如果在判断和写入之间 `nr_groups` 变化（虽然不太可能），可能出现越界

**影响**：评估可能不均匀（某些 group 被多次评估，某些被跳过），但不会导致数据损坏。

**风险评估**：低。`periodic_scan` 不是高频路径，且 `nr_groups` 在 filesystem 生命周期内基本不变。

---

### 5. discard 路径 vs GC 路径竞态（F 延续）

**位置**：
- `f2fs_swod_should_skip_locked()`（discard 路径，持有 `cmd_lock`）
- `f2fs_swod_notify_gc_done()`（GC 路径，无锁）

**问题分析**：
```
时间线：
CPU0 (discard)                              CPU1 (GC)
----------------------------------------
swod_should_skip_locked:
  检查 gid A 的 grp_hint
  grp_hint[A].state == HELD  // 是 held
                                                notify_gc_done(A):
                                                clear_bit(seg, held_segmap)
                                                grp_hint[A].state = NORMAL

  计算 held_first, held_last
  return true (skip)
  // 但此时 A 已经不再是 held 了！
```

**关键点**：
- `swod_should_skip_locked` 持有 `cmd_lock`（`segment.c` 中的 mutex）
- `f2fs_swod_notify_gc_done` 不持有任何锁，只操作 held_segmap 和 grp_hint
- 两者之间没有互斥保护

**影响**：
- discard 可能被错误地跳过，导致 discard 延迟
- skip 返回 true 时，该 discard cmd 会被推迟到下一个 issue 周期
- 不会导致数据损坏，因为最终 discard 还是会被处理

**风险评估**：中等。discard 延迟可能导致短期内 pending discard 积累，但不会导致文件系统问题。

**可能的修复方向**：
1. 在 `f2fs_swod_notify_gc_done` 中也检查 `cmd_lock`（可能引入死锁风险）
2. 使用 RCU 保护 grp_hint 的读取
3. 在 `swod_should_skip_locked` 中增加 "double-check" 逻辑：在返回 true 之前再检查一次 held_segmap

---

## 已修复/非问题项

### E. grp_hint 读写竞态

**重新评估结论**：非问题

- `f2fs_swod_notify_gc_done` 只操作 `held_segmap` 和 `grp_hint` 中特定 group 的状态
- `swod_should_skip_locked` 读取 `grp_hint` 时持有 `cmd_lock`
- 两者之间的竞态已在问题 F 中分析

---

## 参考

- `/doc/new_design_v4.md` - V4 设计文档
- `swod.c` - SWOD 核心实现
- `segment.c` - discard 生命周期
- `gc.c` - GC 路径与 WCE
