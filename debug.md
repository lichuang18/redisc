# SWOD debug summary

## Goal
排查 SWOD 在当前 workload 下为何看起来没有有效生效，重点关注 held / skip / success / timeout / pressure 链路是否闭环。

## Initial symptom
最初测试中：
- `swod_hold_cnt` 非零，说明 held 能建出来
- 但 `swod_skip_cnt / swod_success_release_cnt / swod_timeout_release_cnt / swod_pressure_release_cnt` 一度全为 0
- `swod_held_groups` 还能长时间保持非零

这说明旧实现里存在 held 生命周期管理问题。

## Problems identified
### 1. Held timeout was passive only
旧代码中 timeout 只在以下路径检查：
- `swod_eval_group_locked()`
- `f2fs_swod_should_skip_locked()`

如果 held 后续没有再次命中 eval 或 skip 检查，就不会因为超时自动释放。

### 2. Rebuild silently cleared held state
旧代码 `swod_rebuild_groups_locked()` 会先对目标 group 调用 `swod_clear_group_locked()`，直接清掉 held 状态和 bitmap，但不走 `swod_release_group_locked()`，导致：
- `held_groups` 会变化
- `success/timeout/pressure` 计数不变
- 指标失真

### 3. Merge counters semantics were ambiguous
原来的：
- `discard_back_merge_cnt`
- `discard_front_merge_cnt`
- `discard_both_merge_cnt`

不是互斥统计，容易误解。后来已改为：
- `discard_back_only_merge_cnt`
- `discard_front_only_merge_cnt`
- `discard_both_merge_cnt`

并调整 `__update_discard_tree_range()` 中统计逻辑，使三类互斥。

## Code changes already made
### A. SWOD lifecycle fixes
1. **Removed silent held clearing in rebuild**
   - `swod_rebuild_groups_locked()` 不再先清 `grp_hint/held`
   - 只重建 `seg_hint`

2. **Added active timeout sweep**
   - 新增 `f2fs_swod_sweep_timeout()`
   - 在 discard thread 中调用，主动释放过期 held

3. **Moved timeout sweep later**
   - 从 issue 前移到 `__issue_discard_cmd()` 后，避免 held 在本轮 issue 前被提前 sweep 掉

### B. Held replacement strategy
实现了固定容量 held 下的“替换最差 held”策略：
- `swod_group_hint` 增加 `hold_qbp / hold_lbp`
- 当 `nr_held_groups >= swod_max_held_groups` 时，不再直接 return
- 改为比较新候选与当前最差 held，优者替换之
- timeout 仍保留为兜底机制

### C. Skip diagnostics
为 `f2fs_swod_should_skip_locked()` 增加了诊断计数：
- `skip_check_cnt`
- `skip_miss_noheld_cnt`
- `skip_miss_timeout_cnt`
- `skip_miss_success_cnt`
- `skip_miss_overlap_cnt`

这些都已接入 sysfs 和 `collect_swod.sh`。

### D. Relaxed skip gating
已移除 `should_skip_locked()` 开头对 `swod_regime_blocked()` 的直接短路。
当前已有 held 的 skip 判断不再被 blocked 条件提前 return。

### E. Hold duration tuning support
增加了 `swod_hold_scale_bp`（默认 `10000`）作为整体 hold 时长缩放因子，便于后续进一步放大 hold 时间。

## What recent CSVs showed
### Phase 1
- `hold_cnt > 0`
- `held_groups > 0`
- 但 `skip/success/timeout/pressure = 0`

=> 说明 held 建出来了，但生命周期计数失真（对应 rebuild 清理 + timeout 被动触发问题）。

### Phase 2
修复 lifecycle 后：
- `hold_cnt > 0`
- `timeout_release_cnt > 0`
- `pressure_release_cnt > 0`
- `skip_cnt = 0`

=> 说明 held 的创建与释放已成立，但 skip 路径仍未打通。

### Phase 3
增加 skip 诊断后：
- `skip_check_cnt` 很高
- `skip_cnt = 0`
- `skip_miss_noheld/timeout/success/overlap = 0`

=> 说明函数虽被调用，但在更早阶段返回。
于是移除了 `should_skip_locked()` 中对 `swod_regime_blocked()` 的短路。

### Phase 4 (latest)
最新 CSV 显示：
- `skip_check_cnt` 很高
- `skip_miss_noheld_cnt == skip_check_cnt`
- `hold_cnt = 0`
- `held_groups = 0`

=> 当前问题已收敛到：**held 创建路径没有成功产出 held**。

## Current hypothesis
在最新版本中，skip 路径已经基本可观测化；当前最可疑的是 held 创建侧：
1. `swod_regime_blocked()` 经常返回 true，导致 `swod_eval_group_locked()` 直接 return
2. 或者当前 workload 下默认阈值过严，导致 `found == false`

为此已进一步增加 held 创建诊断计数：
- `swod_eval_blocked_cnt`
- `swod_eval_no_candidate_cnt`

并已接入 sysfs 与 `collect_swod.sh`，下一轮测试即可区分：
- 是长期被 blocked 挡住
- 还是通过 blocked 但始终找不到候选窗口

## Current files changed in this debugging round
- `swod.c`
- `swod.h`
- `segment.c`
- `f2fs.h`
- `debug.c`
- `sysfs.c`
- `collect_swod.sh`
- `scripts/time_tpcc.sh`

## Next step after restart
1. 重新加载最新 `ef2fs.ko`
2. 用最新 `collect_swod.sh` 采样
3. 重点查看：
   - `swod_hold_cnt`
   - `swod_held_groups`
   - `swod_skip_cnt`
   - `swod_timeout_release_cnt`
   - `swod_pressure_release_cnt`
   - `swod_skip_check_cnt`
   - `swod_skip_miss_noheld_cnt`
   - `swod_skip_miss_timeout_cnt`
   - `swod_skip_miss_success_cnt`
   - `swod_skip_miss_overlap_cnt`
   - `swod_eval_blocked_cnt`
   - `swod_eval_no_candidate_cnt`

## Expected interpretation of next CSV
- 如果 `eval_blocked_cnt` 持续涨：创建侧被 blocked 挡住
- 如果 `eval_no_candidate_cnt` 持续涨：阈值/候选选择过严
- 如果 held 开始出现且 skip 仍为 0，再继续分析 overlap / group 粒度问题
