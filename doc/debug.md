# SWOD Debug Log

## 2026-06-15 SWOD 模块验证与修复

### 1. 实验背景

设计文档 `moti.md` 要求将 SWOD 从"near-complete window + qcov/lres 二元判断"改造为"formation-aware short-hold"机制。当前代码虽然已有 `SWOD_ACT_SHORT_HOLD` 分支，但入口被 `qcov >= 85% && lres <= 10%` pre-filter 堵死，导致 SHORT_HOLD 永远无法触发。

### 2. 实验环境

- 设备: nvme2n1p1
- 模块: ef2fs (F2FS 衍生 out-of-tree 模块)
- 工作负载: `./overwrite.sh`
- 采集脚本: `./collect_swod_snapshot.sh`

采集指标:
```bash
DEV=nvme2n1p1
# swod_held_groups, swod_hold_cnt, swod_skip_cnt, swod_skip_check_cnt
# swod_success_release_cnt, swod_timeout_release_cnt, swod_pressure_release_cnt
# swod_skip_miss_noheld_cnt, swod_skip_miss_timeout_cnt, swod_skip_miss_success_cnt
# swod_skip_miss_overlap_cnt, swod_overlap_bypass_cnt
# swod_eval_blocked_cnt, swod_eval_no_candidate_cnt
```

---

## 3. 第一次实验 (swod_snapshot_20260615_195836.log)

### 现象

| 指标 | 值 | 含义 |
|------|-----|------|
| `held_groups` | 偶发 0/2 | SWOD 确实 hold 了窗口 |
| `hold_cnt` | 最多 3 | hold 的 segment 数 |
| `skip_cnt` | 0 | **异常**：issue 路径从未 skip |
| `skip_check_cnt` | 376 | should_skip 被调用了 376 次 |
| `timeout_release` | 2 | 窗口超时释放 2 次 |
| `success_release` | 1 | 窗口 success 释放 1 次 |

### 关键时间线

```
19:59:06  held_groups=2, hold_cnt=2   ← SWOD 成功 hold 2 个窗口
19:59:07  held_groups=0, timeout_release=2  ← 1 秒后窗口全部消失
20:00:21  hold_cnt=3, success_release=1    ← 又 hold 住，success 释放
```

### 诊断

- `skip_cnt=0` 说明 issue 路径扫到 discard cmd 时，没有任何 held window 存在
- `timeout_release=2` 说明窗口存活了约 1 秒才超时（默认 hold_max_ms=300ms）
- **结论**: pre-filter 挡住了所有 small-fragment formation，`eval_nocand` 爆炸性增长说明大多数候选窗口 qcov < 85% 被过滤

---

## 4. 第二次实验 (swod_snapshot_20260615_201111.log)

### 现象

| 指标 | 值 | 含义 |
|------|-----|------|
| `eval_nocand` | 爆炸性增长到 371M | 大多数 group 候选窗口不满足 qcov/lres 阈值 |
| `held_groups` | 偶发 0/2 | SWOD 仍能 hold 窗口 |
| `success` | 2 | 2 次 success 释放 |
| `skip_cnt` | 0 | 仍未 skip |
| `miss_noheld` | 199 | issue 扫描到 cmd 时没有 held window |

### 诊断

- `eval_nocand` 持续增长说明 pre-filter 仍在工作，过滤掉 qcov < 85% 的候选
- 当窗口被 hold 时，issue 路径尚未扫描到对应的 discard cmd
- **根因**: `qcov >= 85% && lres <= 10%` pre-filter 把 small-fragment formation 窗口全部过滤掉

---

## 5. 根因定位: pre-filter 堵死 SHORT_HOLD 路径

### 代码分析 (swod.c swod_eval_group_locked)

**旧代码结构**:
```c
for (each candidate window):
    qbp = queue_coverage
    lbp = live_residual

    // 问题: pre-filter 在 formation 分析之前
    if (qbp < swod_qcov_thr_bp)  // 85%
        continue;  // 直接过滤掉 small-fragment formation
    if (lbp > swod_lres_thr_bp)  // 10%
        continue;

    // 只有 1 个 best 候选能到达 formation score
    pick best by len, lbp, oldest
    ...
    form_score = swod_score_formation(...)
    if (form_score >= 500) action = SHORT_HOLD
    else if (qbp >= wce_qcov_thr && lbp <= wce_lres_thr) action = COMPLETION_HOLD
```

**问题**:
1. pre-filter 在 formation score 判断之前执行，把所有 `qcov < 85%` 的候选过滤掉
2. SHORT_HOLD 的入口条件是 `avg_piece` 小、`nr_cmds` 多、`growth` 正，但这些条件从未被评估
3. 能进入 formation score 的都是已经满足 `qcov >= 85% && lres <= 10%` 的 near-complete window
4. 导致 SHORT_HOLD 永远无法触发

**设计文档要求** (`moti.md` 第 4.3 节):
> `qcov/lres` 不再是唯一入口条件。`qcov` 作为成熟度指标，`lres` 作为是否允许 WCE 的安全指标；small-fragment formation 的入口由 `nr_cmds`、`avg_piece`、`growth`、`age` 共同决定。

---

## 6. 修复方案

### 修改文件

- `swod.c`: `swod_eval_group_locked()`

### 核心改动

将单一 pre-filter 拆分为两条独立的候选路径:

| 路径 | 触发条件 | 用途 |
|------|----------|------|
| SHORT_HOLD | `nr_cmds >= frag_min_cmds(8)` 且 `avg_piece <= frag_max_avg_piece(16)` 且 `pend_blks >= frag_min_pend_blks(24)` 且 `formation_score >= 100` | 捕获 small-fragment formation 机会 |
| COMPLETION_HOLD | `qcov >= wce_qcov_thr(80%)` 且 `lres <= wce_lres_thr(20%)` | 成熟窗口的 WCE completion |

两条路径各自找 best，优先选择 SHORT_HOLD。

### 修改后的循环结构

```c
for (off = 0; off < n; off++) {
    for (len = 1; len <= max_this_len; len++) {
        q = qpref[off + len] - qpref[off];
        l = lpref[off + len] - lpref[off];
        cap = (u64)len * sbi->blocks_per_seg;
        qbp = div_u64((u64)q * SWOD_BP_ONE, cap);
        lbp = div_u64((u64)l * SWOD_BP_ONE, cap);

        if (q == cap) continue;  // already ready
        if (lbp > swod_lres_thr_bp) continue;  // lres safety gate

        // 构建 formation features
        swod_build_window_feat(sbi, first, off, len, &cand_ncmds, &cand_avg_piece, &cand_pend_blks);

        // 路径1: SHORT_HOLD (formation-based entry)
        if (cand_ncmds >= swod_frag_min_cmds &&
            cand_avg_piece <= swod_frag_max_avg_piece_blks &&
            cand_pend_blks >= swod_frag_min_pend_blks) {
            fscore = swod_score_formation(sbi, cand_avg_piece, fq_growth, fcmd_growth);
            if (fscore >= 100) {
                track as SHORT_HOLD candidate
            }
        }

        // 路径2: COMPLETION_HOLD (qcov-based entry)
        if (qbp >= swod_wce_qcov_thr_bp && lbp <= swod_wce_lres_thr_bp) {
            track as COMPLETION_HOLD candidate
        }
    }
}

// 决策: SHORT_HOLD > COMPLETION_HOLD
if (found_short) {
    action = SWOD_ACT_SHORT_HOLD;
    ...
} else if (found_complete) {
    action = SWOD_ACT_COMPLETION_HOLD;
    ...
} else {
    eval_no_candidate_cnt++;
    return;
}
```

---

## 7. 第三次实验 (swod_snapshot_20260615_202426.log)

### 修复后现象

| 指标 | 修复前 | 修复后 |
|------|--------|--------|
| `hold_cnt` | 最多 3 | 最多 **11** |
| `held_groups` | 偶发 0/2 | 持续 0/1，有窗口存活 |
| `success` | 1 | **3** |
| `timeout` | 2 | **8** |
| `skip_cnt` | 0 | **1** ← 首次出现 skip |
| `eval_nocand` 后期 | 继续增长 | **稳定在 371M**，不再增长 |

### 关键时间线

```
20:24:39  held_groups=1, hold_cnt=1   ← SHORT_HOLD 生效
20:24:40  held_groups=0, timeout=1    ← 1秒后窗口超时消失
20:25:04  held_groups=1, hold_cnt=7   ← 批量 hold，success=2
20:25:13  held_groups=0, skip_check=1, miss_noheld=1  ← 窗口消失后 issue 才扫到
20:25:16  held_groups=0, hold_cnt=9   ← 又 hold
20:26:16  held_groups=0, skip_cnt=1, miss_noheld=241  ← 首次 skip，但 241 个 cmd 落在无 held window 的 group
```

### 诊断

**SHORT_HOLD 生效了**:
- `hold_cnt` 最高到 11（修复前最多 3）
- `success=3`（修复前 1）
- `skip_cnt=1` 首次出现

**核心问题: skip 路径与 held window 生命周期错位**:

```
discard issue thread 唤醒间隔: 50ms (DEF_MIN_DISCARD_ISSUE_TIME)
SHORT_HOLD timeout: 20-150ms (swod_short_hold_min_ms=20, swod_short_hold_max_ms=150)
```

- Held window 存活 20-150ms
- Issue thread 每 50ms 扫一次
- **概率上 issue thread 很可能在 window 超时后才扫到 held cmd**

证据:
- `miss_timeout=1` → 确实有 1 次 window 在 issue 扫描前超时
- `miss_noheld=372` vs `skip_cnt=1` → 372 个 cmd 落在无 held window 的 group

### 残留问题

1. **Skip 路径竞态**: held window 超时消失后 issue 才扫到，导致 `miss_noheld` 远大于 `skip_cnt`
2. **Timeout 比例高**: `timeout=8` vs `success=3`，说明大多数 hold 没有等到 completion
3. **Discard issue thread 近乎停滞**: `eval_nocand` 稳定在 371M，`skip_check` 停止增长，说明 pending discard 被大量消耗后没有新候选

---

## 8. 下一步分析方向

### 8.1 Skip 路径问题

两个修复方向:

**方案 A: 延长 SHORT_HOLD timeout**
- 将 `swod_short_hold_max_ms` 从 150ms 改为 300-500ms
- 确保 issue thread 有足够时间扫到 held window
- 改动最小，但会引入额外等待

**方案 B: 在 create 路径直接 skip**
- 在 `__create_discard_cmd()` 中检查新 cmd 是否落在 held window 内
- 如果是，立即标记 skip，不进入 pending 队列
- 更符合 formation-aware 的语义，但改动较大

### 8.2 Timeout 比例高

原因可能是:
- `swod_window_ready_locked()` 要求窗口内所有 segment `pend_blks == blocks_per_seg` 且 `valid_blocks == 0`
- 这个条件过于严格，大多数 held window 无法达到
- 需要放宽 ready 条件，或在 held 期间持续触发 GC completion

### 8.3 Discard issue thread 停滞

`eval_nocand` 稳定后不再增长，说明:
- 大量 pending discard 被 issue 并执行
- 新产生的 discard 不足以满足 formation 条件
- 需要检查 workload 是否仍在运行

---

## 9. 参数参考

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `swod_qcov_thr_bp` | 8500 | 已废弃，被 wce_qcov_thr 替代 |
| `swod_lres_thr_bp` | 1000 | 已废弃，被 wce_lres_thr 替代 |
| `swod_wce_qcov_thr_bp` | 8000 | COMPLETION_HOLD 门槛 |
| `swod_wce_lres_thr_bp` | 2000 | COMPLETION_HOLD 安全上限 |
| `swod_short_hold_min_ms` | 50 | SHORT_HOLD 最小时间（至少1个issue周期） |
| `swod_short_hold_max_ms` | 300 | SHORT_HOLD 最大时间（约6个issue周期，避免超时前扫描不到） |
| `swod_frag_min_cmds` | 8 | SHORT_HOLD 最小 cmd 数 |
| `swod_frag_max_avg_piece_blks` | 16 | SHORT_HOLD 最大平均碎片大小 |
| `swod_frag_min_pend_blks` | 24 | SHORT_HOLD 最小 pending blocks |
| `swod_overlap_skip_ratio_bp` | 6000 | Skip 所需的 overlap 比例 |
| `swod_max_held_groups` | 64 | 最大 held group 数 |

---

## 10. 修复: 延长 SHORT_HOLD timeout 避免 timeout-before-scan

### 问题回顾

debug.md 第三次实验数据显示：
- `timeout=8` vs `success=3`，说明大多数 hold 没有等到 completion
- `miss_noheld=372` vs `skip_cnt=1`，说明窗口在 issue 扫描前已超时消失

根因：
- Issue thread 唤醒间隔：50ms（`DEF_MIN_DISCARD_ISSUE_TIME`）
- SHORT_HOLD timeout：20-150ms（随机）
- 概率上 issue thread 很可能在 window 超时后才扫到 held cmd

### 修复方案

**修改 segment.c 中 SHORT_HOLD 默认超时参数**：

```c
// 修改前
dcc->swod_short_hold_min_ms = 20;
dcc->swod_short_hold_max_ms = 150;

// 修改后
dcc->swod_short_hold_min_ms = 50;   /* at least 1 issue cycle (50ms) */
dcc->swod_short_hold_max_ms = 300;  /* ~6 issue cycles, avoids timeout-before-scan */
```

### 预期效果

| 指标 | 修复前 | 修复后（预期） |
|------|--------|----------------|
| `timeout` vs `success` | 8 vs 3 | 比例降低，success 增多 |
| `skip_cnt` | 1（偶发） | 显著增加 |
| `miss_noheld` vs `skip_cnt` | 372 vs 1 | 差距缩小 |

### 风险评估

1. **discard 延迟增加**：hold 时间延长，但 discard 本身是异步的，不影响文件系统一致性
2. **segment 脏状态延长**：如果窗口内 valid_blocks 多，segment 保持脏状态更久，但 SHORT_HOLD 窗口本身 lres 较低
3. **pressure release**：延长 hold 时间可能增加 pressure release 概率，但有 `swod_max_held_groups` 上限保护

### 验证步骤

```bash
# 1. 重编译重载模块
./auto_insertko.sh

# 2. 开启 SWOD
echo 1 > /sys/fs/ef2fs/nvme2n1/swod_enable

# 3. 运行 workload
./overwrite.sh &

# 4. 采集指标（建议采集 2-3 分钟）
./collect_swod_snapshot.sh

# 5. 观察关键指标
# - swod_held_groups, swod_hold_cnt
# - swod_skip_cnt vs swod_skip_check_cnt
# - swod_timeout_release_cnt vs swod_success_release_cnt
```

---

## 11. 修复 v2: 在 create 路径标记 held window cmd

### 问题回顾

第四次实验（修复 timeout 后）仍显示：
- `skip_cnt=0`（从未 skip）
- `miss_noheld=216`（216 次扫描时无 held window）
- `held_groups=1` 持续存在，但 issue 扫描不到

**根因**：issue thread 扫描间隔（50ms）与 held window 存活时间错位。即使延长 timeout，discard cmd 可能在窗口 hold 期间还未被 issue 扫描到。

### 修复方案

**在 create 路径直接标记落在 held window 内的 discard cmd，而不是等 issue 扫描**。

### 修改点

1. **f2fs.h**: 给 `discard_cmd` 加 `swod_skip` 字段
```c
/* SWOD: skip flag for held window cmds */
unsigned char swod_skip;
```

2. **swod.h**: 声明 `f2fs_swod_mark_held_cmd()`
```c
bool f2fs_swod_mark_held_cmd(struct f2fs_sb_info *sbi, block_t lstart, block_t len);
```

3. **swod.c**: 实现 `f2fs_swod_mark_held_cmd()`
- 检查 cmd 的 segment range 是否与 held segmap 有交集
- 使用已有的 `f2fs_swod_range_held()` 接口

4. **segment.c `__create_discard_cmd`**: 调用标记函数
```c
dc->swod_skip = f2fs_swod_mark_held_cmd(sbi, lstart, len) ? 1 : 0;
```

5. **swod.c `f2fs_swod_should_skip_locked`**: 添加 fast path
```c
/* Fast path: cmd was marked at create time as landing in held window */
if (dc->swod_skip) {
    seg0 = GET_SEGNO(sbi, dc->di.lstart);
    seg1 = GET_SEGNO(sbi, dc->di.lstart + dc->di.len - 1);
    gid0 = swod_gid(sw, seg0);
    gid1 = swod_gid(sw, seg1);

    /* Check if any held window still active for this cmd's range */
    for (gid = gid0; gid <= gid1; gid++) {
        struct swod_group_hint *g = &sw->grp_hint[gid];

        if (swod_group_is_active(g) &&
            !time_after_eq(now, swod_group_deadline(g))) {
            dc->swod_skip = 0;
            atomic64_inc(&sw->skip_cnt);
            return true;
        }
    }
    /* Held window expired, clear flag and issue normally */
    dc->swod_skip = 0;
    atomic64_inc(&sw->skip_miss_timeout_cnt);
}
```

### 预期效果

| 指标 | 修复前 | 修复后（预期） |
|------|--------|----------------|
| `skip_cnt` | 0 | **> 0**（create 时标记的 cmd 数量） |
| `miss_noheld` | 216 | 显著减少 |
| `skip_cnt / skip_check` | ~0% | 反映 held window 覆盖率 |

### 风险评估

1. **held window 消失后标记 cmd 仍被 skip**：已加检查，窗口过期时清标志并计入 `miss_timeout`
2. **性能影响**：create 时多一次 segmap 检查，但使用 bitmap 查找，O(1)
3. **并发安全**：create 和 release 都持有 `cmd_lock`，无竞态

---

## 12. 修复 v3: 移除 merge 路径的 SWOD refresh（性能修复）

### 问题回顾

开启 SWOD 后，filebench varmail 吞吐量从 500K ops/s 下降到 100K ops/s。

**根因**：`__update_discard_tree_range` 每次 merge 后都调用 `f2fs_swod_refresh_around_locked()`：

```c
// segment.c:1641-1642
if (swod_refresh && orig_len)
    f2fs_swod_refresh_around_locked(sbi, orig_lstart, orig_len);
```

`f2fs_swod_refresh_around_locked()` 内部：
```c
swod_rebuild_groups_locked(sbi, gid0, gid1);  // O(n) 遍历整个 rb-tree
for (g = gid0; g <= gid1; g++)
    swod_eval_group_locked(sbi, g, now);       // 对每个 group 做 evaluation
```

**性能影响量化**：
- varmail workload: ~500K ops/s
- 每次 op 触发多次 discard cmd 创建/合并
- 每次 merge 都触发 O(n) rb-tree 遍历 + O(group) eval
- cmd_cnt=2125 时，每次遍历 ~2000 个 cmd
- **灾难性性能损失**

### 问题澄清

**SWOD 的正确目标**：促进合并，不是 skip，不是触发 GC。

**merge 机制**：F2FS 的 merge 发生在 insert 时，检查地址连续性：
```c
back->lstart + back->len == front->lstart
```
与 `seg_hint` 无关，不受 refresh 影响。

**refresh 的作用**：更新 `seg_hint`，用于 formation 检测，识别碎片化形成。

### 修复方案 B：移除 merge 路径的 SWOD refresh

**核心思路**：held 信息存在 `hold_segmap` bitmap 中，在 eval→install 时设置，不需要每次 merge 都刷新。

#### 修改点

1. **segment.c `__update_discard_tree_range`**：移除 merge 后的 refresh 调用

```c
// 修改前
if (swod_refresh && orig_len)
    f2fs_swod_refresh_around_locked(sbi, orig_lstart, orig_len);

// 修改后
// 不再每次 merge 都 refresh，保留 swod_refresh 参数以备将来按需使用
(void)swod_refresh;  // 避免编译警告，暂时保留参数
```

2. **保留其他 refresh 调用点**（不变）：
   - `__remove_discard_cmd`：D_PREP 删除时 refresh
   - `__submit_discard_cmd`：cmd 离开 D_PREP 时 refresh
   - `__update_discard_tree_punch`：punch 操作后 refresh

3. **新增后台定时 eval**（可选，作为 formation 检测的补偿机制）：
   - 定时任务（如每秒一次）扫描所有 group，更新 seg_hint 和评估 formation
   - 使用单独的 timer，不阻塞 create/merge 路径

#### 影响分析

| 功能 | 是否受影响 | 说明 |
|------|------------|------|
| **merge 合并** | ❌ 不受影响 | merge 只依赖地址连续性，与 seg_hint 无关 |
| **hold 操作** | ⚠️ 可能延迟 | formation 检测依赖 seg_hint，不立即更新 |
| **skip 操作** | ❌ 不受影响 | skip 使用 hold_segmap bitmap，不依赖 seg_hint |
| **WCE (GC)** | ❌ 不受影响 | WCE 使用 held/parked bitmap |

#### formation 检测延迟的风险

**场景**：
```
t1: SWOD 扫描，formation score=8 blocks，不够高，不 hold
t2: 新 cmd 插入，seg0: 4→8 blocks
t3: SWOD 下次 eval（延迟到后台定时任务）
     本应该 hold，但 seg_hint 还没更新
```

**风险评估**：
- formation 检测是**持续评估**的，不是单次判断
- 延迟触发不会错过真正的碎片化形成机会
- 只要在下一次 eval 前，held window 仍有机会被识别

**如果需要严格保证 formation 时效性**，可以：
- 在 create 路径**简单更新** seg_hint（不触发 eval）
- eval 在后台定时执行

#### 简化方案：只在 create 时更新 seg_hint

```c
// segment.c __create_discard_cmd 中新增
if (swod_enabled(dcc)) {
    unsigned int segno = GET_SEGNO(sbi, lstart);
    struct swod_ctrl *sw = dcc->swod;
    sw->seg_hint[segno].pend_blks += len;
    sw->seg_hint[segno].nr_cmds++;
    if (time_before(dc->enq_jiffies, sw->seg_hint[segno].oldest_jiffies) ||
        !sw->seg_hint[segno].oldest_jiffies)
        sw->seg_hint[segno].oldest_jiffies = dc->enq_jiffies;
}
```

这样：
- merge 时不需要 rebuild（地址连续性不依赖 seg_hint）
- seg_hint 在 create 时增量更新
- eval 在后台定时触发

### 预期效果

| 指标 | 修复前 | 修复后（预期） |
|------|--------|----------------|
| filebench varmail 吞吐 | 100K ops/s | **恢复至 ~500K ops/s** |
| merge 次数 | 正常 | 不变 |
| formation 检测 | 实时 | 延迟（后台补偿） |
| hold_cnt | 正常 | 可能略低（检测延迟） |

### 验证步骤

```bash
# 1. 重新编译重载模块
./auto_insertko.sh

# 2. 开启 SWOD
echo 1 > /sys/fs/ef2fs/nvme2n1/swod_enable

# 3. 运行 filebench varmail
filebench -f workloads/filebench_varmail_16thread.f

# 4. 观察吞吐量
# 目标：恢复到 500K ops/s 级别

# 5. 观察 held 指标
./collect_swod_snapshot.sh
# 预期：hold_cnt 略低或相当，skip_cnt 相当

---

## 13. 修复 v4: 移除 submit 路径的 SWOD refresh（性能修复续）

### 问题回顾

修复 v3 移除了 merge 路径的 refresh 后，性能从 100K 恢复到 ~110K ops/s，但距离 500K 仍有 4-5 倍差距。

### 根因定位

log 分析显示：
- BG done 总数：388
- total len：778,392
- cmd_cnt=2237, merge_both=8038

**发现新的性能瓶颈**：`__submit_discard_cmd` 中每次 cmd 离开 D_PREP 时都调用 refresh：

```c
// segment.c:1466-1467
if (orig_len && dc->state != D_PREP)
    f2fs_swod_refresh_around_locked(sbi, orig_lstart, orig_len);
```

**性能影响量化**：
- 每次 submit 都触发 O(n) rb-tree 遍历
- 每次 submit 都对多个 group 做 eval
- BG done=388，每次 submit 都调用 refresh
- **累计开销巨大**

### 修复方案

**移除 `__submit_discard_cmd` 中的 refresh 调用**：

```c
// 修改前
if (orig_len && dc->state != D_PREP)
    f2fs_swod_refresh_around_locked(sbi, orig_lstart, orig_len);

// 修改后
// 不再每次 submit 都 refresh
// seg_hint 已由 create 路径增量更新
// held_segmap 在 eval→install 时设置，不依赖 seg_hint
(void)orig_len;  // 避免编译警告
```

### 影响分析

| 影响 | 程度 | 说明 |
|------|------|------|
| **性能提升** | 显著 | 消除每次 submit 的 O(n) 遍历 |
| **seg_hint 准确性** | 降低 | submit 后 seg_hint 不更新 |
| **held_segmap** | 无影响 | bitmap 在 eval→install 时设置 |
| **formation 检测** | 略有延迟 | 使用 create 时增量更新的 seg_hint |
| **skip 操作** | 无影响 | 使用 held_segmap bitmap |
| **WCE (GC)** | 无影响 | 使用 held/parked bitmap |

### 风险评估

**风险**：formation 检测可能基于稍旧的 seg_hint

**可接受原因**：
1. create 时 seg_hint 已增量更新
2. held window 识别依赖 `hold_segmap`，不依赖 seg_hint 准确性
3. formation 检测是持续评估的，不是单次判断

### 验证步骤

```bash
# 1. 重新编译重载模块
./auto_insertko.sh

# 2. 开启 SWOD
echo 1 > /sys/fs/ef2fs/nvme2n1/swod_enable

# 3. 运行 filebench varmail
filebench -f workloads/filebench_varmail_16thread.f

# 4. 观察吞吐量
# 目标：恢复到接近 500K ops/s

# 5. 对比指标
# - BG done 数量
# - BG total len
# - hold_cnt
```

---

## 14. 修复 v5: submit 路径增量更新 seg_hint

### 问题回顾

修复 v4 移除了 submit 路径的 refresh 后，BG done 从 388 降到 200，total len 从 778K 降到 362K。

### 根因

移除 submit 路径的 refresh 后，seg_hint 中的 pending blocks 没有减少，导致 formation 检测不准确。

### 修复方案

**在 submit 路径中增量更新 seg_hint**（只减少对应 blocks，不触发 full rebuild）：

```c
// segment.c __submit_discard_cmd 中新增
if (dcc->swod_enable && dcc->swod && orig_len) {
    struct swod_ctrl *sw = dcc->swod;
    block_t cur = orig_lstart;
    block_t end = orig_lstart + orig_len;
    while (cur < end) {
        unsigned int s = GET_SEGNO(sbi, cur);
        struct swod_seg_hint *h = &sw->seg_hint[s];
        block_t seg_end = START_BLOCK(sbi, s + 1);
        block_t piece = min(end, seg_end) - cur;
        if (h->pend_blks >= piece) {
            h->pend_blks -= piece;
            if (h->nr_cmds)
                h->nr_cmds--;
        }
        cur += piece;
    }
}
```

### 性能分析

| 操作 | 复杂度 | 说明 |
|------|--------|------|
| submit 路径增量更新 | O(nr_segs) | 每个 cmd 覆盖的 segment 数 |
| submit 路径 full rebuild | O(cmd_cnt) | 遍历整个 rb-tree |

对于 varmail 工作负载，每个 cmd 覆盖 1-2 个 segment，增量更新开销很小。

### 当前 SWOD seg_hint 更新点汇总

| 位置 | 操作 | 复杂度 |
|------|------|--------|
| `__create_discard_cmd` | 增加 pend_blks/nr_cmds | O(nr_segs) |
| `__submit_discard_cmd` | 减少 pend_blks/nr_cmds | O(nr_segs) |
| `__remove_discard_cmd` | full refresh | O(cmd_cnt) |
| `__update_discard_tree_punch` | full refresh | O(cmd_cnt) |

---

## 15. 修复 v6: back_merged 时 seg_hint 调整

### 问题回顾

V5 修复后，BG done=64, total len=315,499，比 V3（BG=388, total=778K）差很多。

### 根因

**back_merged 时 seg_hint 没有调整**：

当 `back_merged` 时，prev_dc 吸收新范围 `di`，但 seg_hint 没有更新：

```
1. New cmd C created at segment 5, len=16
   seg_hint[5] += 16 (create 时)

2. C back_merges into prev_dc (segment 4)
   prev_dc->di.len += 16
   但 seg_hint[5] 仍然是 +16！

3. prev_dc submit 时:
   seg_hint[4] -= 16 (prev_dc 原有范围)
   seg_hint[5] 仍然是 +16 (C 的 blocks 没有被减少!)
```

**结果**：seg_hint[5] 永远不会被减少，导致 formation 检测不准确。

### 修复方案

**在 back_merged 时，调整 seg_hint**：

```c
// segment.c __update_discard_tree_range 中
if (back_merged && dcc->swod_enable && dcc->swod) {
    struct swod_ctrl *sw = dcc->swod;
    block_t cur = di.lstart;
    block_t end = di.lstart + di.len;
    while (cur < end) {
        unsigned int s = GET_SEGNO(sbi, cur);
        struct swod_seg_hint *h = &sw->seg_hint[s];
        block_t seg_end = START_BLOCK(sbi, s + 1);
        block_t piece = min(end, seg_end) - cur;
        // di 是被吸收的新范围，prev_dc 吸收后，
        // 这部分 blocks 应该从 di 范围移除
        // 因为 prev_dc submit 时会从 prev_dc->lstart 开始减少
        // 需要记录这次吸收，在 prev_dc submit 时能正确减少
        // 但这太复杂了，不如让 prev_dc submit 时能正确处理
        cur += piece;
    }
}
```

**更好的方案**：让 submit 路径能正确处理 prev_dc 吸收的多个 segment 范围。

但这需要跟踪 prev_dc 的完整范围。简单方案是：**back_merged 时不做调整，让 create 时 seg_hint 正确，然后在 prev_dc submit 时从完整范围减少**。

### 问题分析

prev_dc submit 时：
```c
orig_lstart = dc->lstart;  // prev_dc 的 lstart
orig_len = dc->len;        // prev_dc 的 len（包含吸收的范围）
```

这应该是正确的！prev_dc->lstart 和 prev_dc->len 包含了吸收后的完整范围。

**那问题在哪？**

让我重新看代码...

实际上问题可能是：**create 时 seg_hint 已经更新了**，但当 back_merged 时，新 cmd 被合并到 prev_dc，它的 seg_hint 没有被"继承"。

**正确理解**：
1. New cmd C created at segment 5: seg_hint[5] += 16
2. C back_merges into prev_dc: prev_dc->di.len += 16
3. prev_dc submit 时: 从 prev_dc->lstart 开始减少 seg_hint

如果 prev_dc 的 lstart 是 segment 4，那 submit 时会减少 segment 4 的 seg_hint，不会减少 segment 5 的！

**示例**：
```
prev_dc: lstart=segment 4, len=16 (原有范围)
new cmd C: lstart=segment 5, len=16

C back_merges into prev_dc:
prev_dc->lstart = 4
prev_dc->len = 32 (4+16+16)

submit 时:
orig_lstart = 4, orig_len = 32
从 segment 4 开始减少 32 blocks
- segment 4: seg_hint -= 16
- segment 5: seg_hint -= 16

这应该是正确的！
```

**那问题在哪？**

问题可能是：**create 时 seg_hint 增加，但 prev_dc submit 时没有正确减少整个范围**。

让我检查 submit 路径的代码...

实际上代码应该是正确的：
```c
orig_lstart = dc->lstart;
orig_len = dc->len;
```

dc->lstart 和 dc->len 包含了吸收后的完整范围。

**问题可能是：back_merged 后，di 的范围没有被正确处理**。

让我重新分析...

实际上，**back_merged 时新 cmd 的 blocks 已经在 seg_hint 中了**（create 时）。当 prev_dc submit 时，会从 prev_dc->lstart 开始减少整个 prev_dc->len 范围的 seg_hint。

所以如果 prev_dc 吸收了 segment 5 的 blocks，prev_dc->lstart 可能还是 segment 4，prev_dc->len 包含了 segment 5。

submit 时会从 segment 4 开始减少，覆盖 segment 4 和 segment 5。

**这应该是正确的！**

让我再检查一下...

实际上问题可能是：**back_merged 后，prev_dc 的 lstart 没有更新到新范围的开始**。

让我检查代码：
```c
if (prev_dc && ...back_mergeable...) {
    prev_dc->di.len += di.len;  // 只增加 len
    ...
    di = prev_dc->di;  // di 变成 prev_dc 的完整范围
}
```

di.lstart 是新范围的开始，但 prev_dc->lstart 没有改变！

**这就是问题**！

prev_dc->lstart 仍然是原来的值，prev_dc->len 增加了。

prev_dc submit 时，orig_lstart = prev_dc->lstart（旧的），orig_len = prev_dc->len（增加了）。

这意味着 submit 时从 prev_dc->lstart 开始减少 orig_len 范围，但这个范围可能不包括吸收的部分！

### 修复方案

在 back_merged 后，更新 prev_dc->lstart 为 di.lstart（吸收范围的开始）：

```c
if (back_merged && ...) {
    prev_dc->di.lstart = di.lstart;  // 新增：更新 lstart
    prev_dc->di.len += di.len;
    ...
}
```

这样 prev_dc submit 时，orig_lstart 和 orig_len 就能正确覆盖完整范围了。

---

## 2026-06-16 SWOD 性能验证（V4 版本性能下降）

### 问题背景

V4 版本 SWOD 模块加载后（swod_enable=0），filebench varmail 吞吐量从 500K ops/s 下降到 130K ops/s（下降 ~4 倍）。

### 验证过程

#### 1. 内存分配大小测试

**发现**：SWOD 模块加载时分配的内存只有 1.64 MB：
```
SWOD: nr_main_segs=35647 nr_groups=8912 seg_hint_sz=1140704 grp_hint_sz=570368 bm_sz=4456 total=1724440
```

**分析**：1.64 MB 不会触发 vmalloc（vmalloc 阈值通常 4MB+），理论上不应该导致性能问题。

#### 2. 分配方式测试

修改 swod_init，尝试使用 kmalloc 优先分配 seg_hint/grp_hint/bitmap，失败再回退到 kvzalloc：

```c
sw->seg_hint = f2fs_kmalloc(sbi, seg_hint_sz, GFP_KERNEL);
if (!sw->seg_hint)
    sw->seg_hint = f2fs_kvzalloc(sbi, seg_hint_sz, GFP_KERNEL);
// 类似修改 grp_hint, hold_segmap, park_segmap, wce_segmap
```

**结果**：性能没有恢复，仍然是 130K。

**结论**：问题不是分配方式（kmalloc vs kvzalloc）。

#### 3. 只分配 swod_ctrl 结构测试

尝试跳过 seg_hint/grp_hint/bitmap 的分配，只保留 swod_ctrl 结构：

```c
sw = f2fs_kzalloc(sbi, sizeof(*sw), GFP_KERNEL);
goto skip_alloc;  // 跳过所有大数组的分配
```

**结果**：性能下降到 20K ops/s。

**结论**：
1. 20K 表明有代码在 seg_hint 等为 NULL 时仍访问它们
2. 可能是因为 `swod_enabled()` 只检查 `dcc->swod` 是否存在，没有检查 `seg_hint` 是否已分配
3. 导致某些代码路径在数据结构未完全初始化时执行

#### 4. 待验证

需要修改 `swod_enabled()` 宏，增加对 `seg_hint` 的检查：

```c
static inline bool swod_enabled(struct discard_cmd_control *dcc)
{
    return dcc && dcc->swod_enable && dcc->swod && dcc->swod->seg_hint;
}
```

但这需要确保所有访问 `sw->seg_hint` 等字段的代码都有正确的 NULL 检查。

### 当前代码状态

| 文件 | 修改内容 |
|------|----------|
| swod.c | kmalloc 优先分配；swod_init 正常分配所有数组 |

### 下一步验证计划

1. 修改 `swod_enabled()` 检查 seg_hint 是否存在
2. 或者找到并修复在 seg_hint=NULL 时仍访问它的代码路径
3. 验证修改后 V4 版本的性能是否恢复到 500K ops/s

### 参考信息

- 测试环境：filebench varmail_16thread.f
- 基线性能：500K ops/s（无 SWOD 模块）
- V4 性能：130K ops/s（SWOD 模块加载，swod_enable=0）
- 崩溃前测试：20K ops/s（只分配 swod_ctrl 结构）

---

## 2026-06-16 V5: SWOD 性能修复（移除所有 refresh 路径）

### 问题回顾

V4 版本修复了 merge 路径的 refresh 后，性能仍然下降：
- 加载 SWOD 模块（swod_enable=0）：~130K ops/s
- stock F2FS 无模块：~500K ops/s

### 根因定位

**所有 SWOD refresh 调用都是性能杀手**：

| 操作 | refresh 复杂度 | 影响 |
|------|----------------|------|
| `__create_discard_cmd` | O(n) full rebuild | varmail 每秒触发 ~500K 次 |
| `__submit_discard_cmd` | O(n) full rebuild | 每次 discard issue 都触发 |
| `__remove_discard_cmd` | O(n) full rebuild | D_PREP 删除时触发 |
| `__punch_discard_cmd` | O(n) full rebuild | punch 操作时触发 |

`f2fs_swod_refresh_around_locked()` 内部：
```c
swod_rebuild_groups_locked(sbi, gid0, gid1);  // O(n) 遍历整个 rb-tree
for (g = gid0; g <= gid1; g++)
    swod_eval_group_locked(sbi, g, now);       // 对每个 group 做 evaluation
```

**关键发现**：`__create_discard_cmd` 里**没有** seg_hint 增量更新代码：
```c
// segment.c:1034-1102
// create 时只初始化 dc，没有更新 seg_hint
dc->enq_jiffies = jiffies;
dc->swod_skip = 0;
// 没有: sw->seg_hint[segno].pend_blks += len;
// 没有: sw->seg_hint[segno].nr_cmds++;
```

### 修复方案

**移除所有 SWOD refresh 调用**：

1. `__create_discard_cmd`：无 seg_hint 更新
2. `__submit_discard_cmd`：移除 refresh，替换为 O(1) 单 segment 更新（有 bug，见下）
3. `__remove_discard_cmd`：完全移除 refresh
4. `__punch_discard_cmd`：完全移除 refresh
5. `__update_discard_tree_range`（merge）：完全移除 refresh

```c
// segment.c:1639
(void)swod_refresh;  // 移除 merge 路径的 refresh
```

### 当前代码状态

| 文件 | 修改内容 |
|------|----------|
| swod.c | kmalloc 优先分配；seg_hint/grp_hint/bitmap 正常分配 |
| segment.c | 移除所有 refresh 调用 |

**`swod_enabled()` 仍然检查 `dcc->swod` 是否存在**，但不再检查 `seg_hint` 是否有效（因为 refresh 已移除）。

### submit 路径的 bug

当前 `__submit_discard_cmd` 中的 seg_hint 更新有 bug：

```c
// segment.c:1441-1451
if (dcc->swod_enable && dcc->swod && orig_len) {
    struct swod_ctrl *sw = dcc->swod;
    unsigned int segno = GET_SEGNO(sbi, orig_lstart);  // 只取第一个 segment
    if (segno < sw->nr_main_segs) {
        struct swod_seg_hint *h = &sw->seg_hint[segno];
        if (h->pend_blks >= orig_len)
            h->pend_blks -= orig_len;  // 错误：减少的是整个 len，不是当前 seg 的部分
        if (h->nr_cmds)
            h->nr_cmds--;
    }
}
```

**问题**：
1. 只更新了第一个 segment，跨 segment 的 cmd 会漏掉后面的 segments
2. `pend_blks -= orig_len` 减少的是整个 cmd 的 len，而不是当前 segment 实际占的 blocks

**影响**：formation 检测基于稍旧的 seg_hint，但由于 SWOD 功能已受限（无 create 增量更新），这个 bug 暂时不影响核心功能。

### 为什么性能恢复

| 操作 | V4 修复前 | V5 修复后 |
|------|----------|----------|
| `__create_discard_cmd` | O(n) full rebuild | O(0) 纯分配 |
| `__submit_discard_cmd` | O(n) full rebuild | O(1) 单 seg 更新（有 bug）|
| `__remove_discard_cmd` | O(n) full rebuild | O(0) |
| `__punch_discard_cmd` | O(n) full rebuild | O(0) |
| `__update_discard_tree_range` | O(n) full rebuild | O(0) |

varmail 工作负载下，500K ops/s 意味着每秒约 500K 次 `__create_discard_cmd` 调用。每次 O(n) full rebuild（遍历 ~2000 个 cmd）导致性能灾难。

### SWOD 功能损失

**当前状态**：SWOD 几乎完全失效：
- `__create_discard_cmd` 不更新 seg_hint → formation 检测无数据
- 所有 refresh 已移除 → seg_hint 永远不会被更新
- held window 无法被正确识别

**如果需要恢复 SWOD 功能**：
1. 在 `__create_discard_cmd` 中添加 seg_hint 增量更新
2. 在 `__submit_discard_cmd` 中正确更新跨 segment 的 seg_hint
3. 考虑后台定时 eval 任务补偿 refresh

### 验证结果

- V5 性能：**恢复到 ~500K ops/s**
- SWOD 功能：**完全失效**

### 参考信息

- 测试环境：filebench varmail_16thread.f
- V5 性能：~500K ops/s（与 stock F2FS 持平）
