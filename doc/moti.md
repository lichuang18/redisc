# Formation-aware SWOD 修改方案：面向小碎片形成过程的 discard 合并优化

目标：利用更多短期局部合并机会，减少 premature issue，提高 BG discard 的命令形态，而不是简单延迟或静态节流。

本文档用于指导当前 redisc/F2FS 代码从“near-complete window + WCE completion”为核心的设计，调整为更贴合新 motivation 的“formation-aware discard shaping”。文档重点是可执行的设计修改、代码落点、边界情况、统计指标和评估方式。

## 0. 结论摘要

当前 repo 的设计已经具备一个较好的基础：在 F2FS pending discard 队列之上维护 segment-window 摘要，并通过 hold 暂缓部分 discard command 的发射。但是现有版本的机会判定过于依赖 `qcov >= Tq` 与 `lres <= Tl`，更偏向“接近完成的窗口”。这与新的动机观察存在错位：新的动机说明，许多 small discard command 是局部 segment-window 正在形成过程中的早期碎片，真正要捕获的是短期 follow-up 与 window coverage growth。

因此，设计应从“保留高 qcov/低 lres 的窗口并交给 WCE”调整为“跟踪 small fragments 在 segment-window 内的短期演化，短暂保留仍在形成的窗口以捕获更多合并机会，只在窗口足够成熟时才进入 completion/WCE 路径”。

| 维度 | 旧设计 | 修改后设计 |
| --- | --- | --- |
| 核心目标 | 保住 near-complete window，等待/推动 completion | 利用更多 small-fragment formation 机会，在发射前优化 command formation |
| 基本单位 | segment group 内一个满足 qcov/lres 阈值的 held 子窗口 | segment-window 内 small fragments 的形成状态与短期增长趋势 |
| 机会判定 | `qcov >= Tq && lres <= Tl` | fragment density、avg piece、age、growth、qcov/lres safety 共同判断 |
| hold 语义 | held window 通常服务 WCE completion | 区分 short formation hold 与 completion hold |
| WCE 关系 | held target set 直接服务 WCE target-first | 只有成熟窗口设置 `wce_eligible`，small-fragment early window 不进入 WCE |
| 评估重点 | hold/release、GC pick/fallback | 捕获 follow-up、coverage gain、BG command count/length、UMOUNT residual |

## 1. 设计目标与非目标

### 1.1 总目标

修改后的核心目标是：利用更多 short-term local coalescing opportunities，在 discard command 发射前改善 small fragments 的形成形态，减少 premature issue，使最终 BG 发送的 discard command 更少、更长、更规整，并且不把工作简单推迟到 UMOUNT。

- 将 small discard command 视为“局部 segment-window 形成过程中的早期碎片”，而不是孤立 issue 单元。
- 在 pending discard 队列中识别仍在成长的 local window，并用有界短暂 hold 捕获 nearby follow-up。
- 通过 window-level maturity 判断决定何时释放，避免无限等待和 pending backlog。
- 仅当窗口已经足够成熟、live residual 风险足够低时，才允许其成为 WCE/GC completion target。
- 保持 stock F2FS 最终 materialization 与 issue 的边界，不主动伪造跨 live block 的大 discard。

### 1.2 非目标

- 不是为了让所有 workload 都显著提升性能；目标是减少可利用机会中的 small discard fragmentation，并保证不恶化。
- 不是简单延长 discard issue interval；静态等待会同时延迟有价值和无价值的小命令。
- 不是把所有 held window 都交给 WCE；fragment window 可能有 batching 价值，但不一定适合 GC completion。
- 不是重写 F2FS allocator、SSR、IPU 主路径或前台写入落点。
- 不是把 gap 很近的 ranges 强行合并；中间可能存在 live block，必须保持安全边界。

## 2. 当前 repo 设计诊断

根据当前 redisc README 与代码组织，现有实现的主线是：SWOD 在 pending discard 队列之上识别并 hold 高价值窗口，WCE 围绕 held windows 做 target-first completion，SFI 作为 non-target zone 的窄辅助路径。README 中明确说明 SWOD 的核心是识别机会窗口、暂缓发射、向 WCE 暴露 held target set，最终 materialization 与 issue 仍由 stock F2FS 负责。

### 2.1 现有机制的优点

- 已经建立了 segment-window 这一合适抽象，不再只以单条 discard command 为中心。
- 已经维护 per-segment 摘要：`pend_blks`、`nr_cmds`、`oldest_jiffies`，这些可以扩展为 formation tracking 的基础。
- 已经在 issue path 中接入 `f2fs_swod_should_skip_locked()`，具备对 D_PREP command 做 hold/skip 的控制点。
- 已经具备 bounded waiting、pressure release、urgent/force fallback 等安全边界。
- WCE 的 target-first 逻辑可以作为成熟窗口的 optional completion path 保留。
- sysfs 统计已覆盖 hold、skip、release、GC pick/fallback，可以继续扩展。

### 2.2 与新 motivation 的错位

| 问题 | 现有行为 | 为什么不适配新动机 | 修改方向 |
| --- | --- | --- | --- |
| 机会判定过窄 | 主要依赖高 qcov、低 lres | 只能抓 near-complete window，漏掉仍在形成但尚未高 coverage 的 small-fragment window | 引入 formation features：fragment density、avg_piece、growth、age |
| hold 语义单一 | held window 主要服务 WCE completion | small fragments 的主要机会是 short-term follow-up，不一定适合 GC completion | 拆分 short formation hold 与 completion hold |
| WCE target 过宽 | held/parked target 容易被 WCE 使用 | low-coverage fragment window 交给 GC 可能增加 copy cost | 新增 `wce_eligible` 与 `wce_segmap` |
| hold 时间偏粗 | 默认 ms 级，受 qcov/lres/len 影响 | motivation 显示机会在 10us-1ms 内形成，过长 hold 可能积累 backlog | short hold 用较短预算，growth stops 或 pressure 即释放 |
| 统计不足 | 只看 hold/skip/release | 无法证明 hold 期间捕获 follow-up 或 coverage gain | 新增 capture/gain 统计 |

## 3. 修改后的总体架构

修改后设计仍然保留 SWOD/WCE/SFI 的基本代码框架，但需要重新定义主路径：SWOD 的主路径不再是“只找 near-complete window”，而是“对 small-fragment formation 进行短期建模并做 bounded shaping”。WCE 从主路径降为成熟窗口的 completion path，SFI 作为可选辅助路径保留。

### 3.1 新设计主线

- Window abstraction：以 segment-window 为单位观察 small fragments，而不是单条 command。
- Formation tracking：记录 window 内 pending fragments 的数量、大小、年龄和短期增长。
- Maturity estimation：判断当前 small command 是否仍是早期碎片，或 window 是否已经停止增长/足够成熟。
- Bounded shaping：仅对有短期合并潜力的 window 做短暂 hold，捕获 nearby follow-up。
- Safe release：窗口成熟、增长停止、超时、压力上升或 urgent/force policy 触发时释放。
- Completion gating：只有成熟度高、live residual 低的 window 才暴露给 WCE。

### 3.2 新状态语义

| 状态/动作 | 含义 | 是否 skip issue | 是否进入 WCE | 典型释放条件 |
| --- | --- | --- | --- | --- |
| BYPASS | 没有可利用形成机会，或系统处于压力/urgent/force | 否 | 否 | 直接走 stock F2FS |
| SHORT_HOLD | small fragments 密集且仍在增长，目标是捕获 follow-up | 是，短暂 | 否 | growth stops / timeout / pressure / captured enough |
| COMPLETION_HOLD | window 已较成熟，可能通过等待或 WCE 进一步规整 | 是 | 可选，需 `wce_eligible` | ready / timeout / pressure |
| RELEASE | 释放 held window，交回 stock F2FS issue/materialization | 否 | 否 | 更新 counters 与 window outcome |

## 4. 关键模型：从 qcov/lres 到 formation-aware features

### 4.1 保留的基础指标

| 指标 | 来源 | 用途 |
| --- | --- | --- |
| `pend_blks` | per-segment D_PREP pending blocks | 表示 queue-visible discard demand |
| `nr_cmds` | 覆盖该 segment 的 pending command 数 | 表示碎片密度 |
| `oldest_jiffies` | 最早进入 pending 的时间 | 估计等待年龄 |
| `qcov` | window pending blocks / capacity | 表示当前可见覆盖率，作为 maturity 指标 |
| `lres` | window valid blocks / capacity | 表示 completion 成本/安全风险 |

### 4.2 新增 formation features

| 指标 | 定义 | 设计意义 |
| --- | --- | --- |
| `avg_piece` | `pending_blocks / max(nr_cmds,1)` | 平均碎片大小；越小越像 small-fragment formation |
| `frag_density` | `nr_cmds / window_len` 或归一化后的 command density | 单位窗口内碎片密度 |
| `age_ms` | 当前时间 - oldest fragment time | 避免过早判断，防止刚入队就 hold 太多 |
| `q_growth` | 本次 qcov - 上次 qcov | 判断 window 是否仍在形成 |
| `cmd_growth` | 本次 nr_cmds - 上次 nr_cmds | 判断是否继续有 follow-up 进入 |
| `stagnation` | 多次 refresh 中 qcov/cmds 不再增长 | 判断可以释放，避免无效等待 |
| `overlap_ratio` | 待 issue command 与 held window 的重叠比例 | 防止大 command 擦边被整体 hold |

### 4.3 新 opportunity score

不建议一开始引入复杂机器学习或难以解释的模型。建议使用轻量启发式 score，便于内核实现和论文解释。

```text
formation_score(window) =
    + W_frag  * normalized_frag_density
    + W_small * small_piece_bonus(avg_piece)
    + W_grow  * positive_growth(q_growth, cmd_growth)
    + W_age   * bounded_age_score(age_ms)
    - W_live  * excessive_live_residual_penalty(lres)
    - W_press * system_pressure_penalty
```

其中 `qcov/lres` 不再是唯一入口条件。`qcov` 作为成熟度指标，`lres` 作为是否允许 WCE 的安全指标；small-fragment formation 的入口由 `nr_cmds`、`avg_piece`、`growth`、`age` 共同决定。

## 5. 算法设计

### 5.1 Window refresh

触发时机应复用现有 `__update_discard_tree_range()`、`__submit_discard_cmd()`、`__punch_discard_cmd()`、`__remove_discard_cmd()` 等 refresh hook。每次 refresh 只更新受影响 range 附近的 group，避免扫描全局。

```text
On discard range create/update/remove/submit:
    locate affected segment group(s)
    rebuild per-segment D_PREP summaries
    update group-level last snapshot
    evaluate candidate windows around affected range
```

### 5.2 Candidate evaluation

```text
for each group G affected by refresh:
    build prefix sums for pend_blks, nr_cmds, valid_blks
    for each candidate sub-window r in G:
        compute qcov, lres, nr_cmds, avg_piece, age, growth
        if system pressure or urgent policy:
            bypass
        score = formation_score(r)
        action = decide_action(score, qcov, lres, growth, pressure)
        keep best candidate by score and safety
    install candidate if better than current active window
```

候选优先级应从“更长 run 优先”调整为“更高 formation value 且风险可控优先”。如果两个候选 score 接近，可以用更低 lres、更大 qcov、更老 age 作为 tie-breaker。

### 5.3 Action decision

| 条件 | 动作 | 解释 |
| --- | --- | --- |
| fragment density 高、avg_piece 小、growth 正、lres 不低 | SHORT_HOLD | 捕获 follow-up，但不暴露给 WCE |
| qcov 高、lres 低、window 成熟 | COMPLETION_HOLD + wce_eligible | 允许 WCE 优先清理 residual live blocks |
| score 低、growth 停止、command 已经足够大 | BYPASS/RELEASE | 不再延迟，交给 stock F2FS |
| system pressure、foreground/force/urgent discard | BYPASS/pressure release | 避免影响空间回收与前台 I/O |

### 5.4 Short formation hold

SHORT_HOLD 是修改后的主路径。它对应 motivation 中“small command 是局部窗口早期碎片”的观察。目标不是等待整段完成，而是在较短时间内捕获 nearby follow-up，让 stock F2FS 在之后看到更好的 command formation。

- hold 对象：small-fragment dense、仍在增长的 window。
- hold 时间：建议短于旧设计，初始可用 1-10ms 或一个 discard issue cycle；若内核计时精度限制，可使用 jiffies 级但要保持较小上限。
- release：一旦增长停止、超时、压力上升、捕获到足够 follow-up 或 window 已成熟即释放。
- 不进入 WCE：默认 `wce_eligible=false`，避免低 coverage 窗口触发 GC completion。

### 5.5 Completion hold 与 WCE gate

COMPLETION_HOLD 保留旧设计的价值，但不应作为所有 held window 的默认路径。只有成熟窗口才允许 WCE。

```c
if action == COMPLETION_HOLD and qcov >= near_qcov_thr and lres <= wce_lres_thr:
    wce_eligible = true
    set wce_segmap for segments in window
else:
    wce_eligible = false
    clear wce_segmap
```

这样可以避免 varmail/OLTP 中 coverage 较低但 fragment 密集的 window 被 GC 误当作 target，造成额外 live-block copy cost。

### 5.6 Release policy

| Release reason | 触发条件 | 是否算成功 | 用途 |
| --- | --- | --- | --- |
| capture_release | hold 期间捕获 follow-up，window command shape 改善 | 是 | 证明 formation-aware 生效 |
| mature_release | qcov 增长、avg_piece 增大、window 更接近连续 | 是 | 进入更好 issue/materialization 状态 |
| timeout_release | 超过 short/completion hold 上限 | 否或弱成功 | 判断误判率 |
| pressure_release | pending backlog、空间压力或 urgent/force 模式 | 安全释放 | 证明不会拖垮系统 |
| bypass_release | 窗口不再增长或 command 已足够大 | 中性 | 避免无效 hold |

## 6. 代码修改清单

### 6.1 `swod.h`

新增 action/state 语义和 bitmap，扩展 per-segment / per-group 摘要。

```c
enum swod_action {
    SWOD_ACT_BYPASS = 0,
    SWOD_ACT_SHORT_HOLD,
    SWOD_ACT_COMPLETION_HOLD,
};

struct swod_seg_hint {
    unsigned int pend_blks;
    unsigned int nr_cmds;
    unsigned long oldest_jiffies;

    /* new: formation tracking */
    unsigned int last_pend_blks;
    unsigned int last_nr_cmds;
    unsigned long last_sample_jiffies;
};

struct swod_group_hint {
    u8 state;
    u8 action;
    u8 wce_eligible;

    u16 hold_off;
    u16 hold_len;
    unsigned long hold_until;
    unsigned long last_eval;

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
    s16 last_q_growth;
    s16 last_cmd_growth;
};

struct swod_ctrl {
    unsigned long *hold_segmap;
    unsigned long *park_segmap;
    unsigned long *wce_segmap; /* new: completion-eligible targets only */
    atomic_t nr_wce_groups;
};
```

### 6.2 `f2fs.h` / `discard_cmd_control`

新增运行时参数，优先使用 conservative default，避免一上来改变过多行为。

```c
/* formation-aware SWOD parameters */
bool swod_formation_enable;
unsigned int swod_short_hold_min_ms;
unsigned int swod_short_hold_max_ms;
unsigned int swod_frag_min_cmds;
unsigned int swod_frag_max_avg_piece_blks;
unsigned int swod_frag_min_pend_blks;
unsigned int swod_growth_min_bp;
unsigned int swod_wce_lres_thr_bp;
unsigned int swod_wce_qcov_thr_bp;
unsigned int swod_overlap_skip_ratio_bp;
```

### 6.3 `swod.c`

这是主要修改文件。建议分四步改造：feature builder、candidate scorer、action installer、skip/release logic。

- 新增 `swod_build_window_feat()`：从 prefix sums 计算 qcov/lres/nr_cmds/avg_piece/age/growth。
- 新增 `swod_score_formation()`：判断 small-fragment formation value。
- 替换 `swod_eval_group_locked()` 中固定 `qcov/lres` 过滤逻辑。
- 新增 `swod_install_candidate()`：根据 action 设置 hold_segmap/wce_segmap 和 group snapshot。
- 新增 `swod_release_with_reason()`：释放时统计 coverage gain、follow-up capture、timeout、pressure。
- 修改 `f2fs_swod_should_skip_locked()`：加入 action 判断、overlap ratio、policy bypass。

### 6.4 `gc.c`

WCE 必须改为只查询 `wce_segmap`，而不是直接使用所有 held/parked ranges。

```c
bool f2fs_swod_has_wce_target(struct f2fs_sb_info *sbi)
{
    return swod && atomic_read(&swod->nr_wce_groups) > 0;
}

bool f2fs_swod_range_wce_target(struct f2fs_sb_info *sbi,
                                unsigned int segno,
                                unsigned int nr_segs)
{
    /* Only match wce_segmap, not hold_segmap. */
}
```

### 6.5 `segment.c`

保持现有 hook 结构，但补充 formation-aware refresh 和 release。

- `__create_discard_cmd()`：继续初始化 enqueue time；若可行，记录更高精度 timestamp 用于分析。
- `__update_discard_tree_range()`：新 D_PREP range 进入时触发 affected group refresh。
- `__issue_discard_cmd_orderly()` / `__issue_discard_cmd()`：submit 前调用新的 should_skip；urgent/force policy 直接 release。
- `issue_discard_thread()`：在 pressure、UMOUNT、urgent、force 路径上释放 held windows，避免拖到 teardown。
- `__submit_discard_cmd()` / `__remove_discard_cmd()`：submit/remove 后 refresh old range，更新 growth/outcome counters。

### 6.6 `sysfs.c` 与采集脚本

新增参数和 counters。没有这些统计，就无法证明设计真的利用了更多合并机会。

| 类别 | 建议节点 |
| --- | --- |
| 控制参数 | `swod_formation_enable`, `swod_short_hold_min_ms`, `swod_short_hold_max_ms`, `swod_frag_min_cmds`, `swod_frag_max_avg_piece_blks`, `swod_growth_min_bp`, `swod_overlap_skip_ratio_bp` |
| formation counters | `swod_short_hold_cnt`, `swod_capture_followup_cnt`, `swod_capture_blocks`, `swod_coverage_gain_blocks`, `swod_start_nr_cmds_sum`, `swod_release_nr_cmds_sum` |
| completion gate | `swod_completion_hold_cnt`, `swod_wce_eligible_cnt`, `swod_wce_blocked_low_maturity_cnt`, `swod_wce_blocked_high_lres_cnt` |
| safety | `swod_overlap_bypass_cnt`, `swod_pressure_release_cnt`, `swod_timeout_release_cnt`, `swod_policy_bypass_cnt`, `swod_pending_backlog_max` |

## 7. 边界情况与安全策略

| 边界情况 | 风险 | 解决方案 |
| --- | --- | --- |
| 大 command 擦边 held window | 整体 hold 大命令，反而降低效率 | 引入 overlap ratio；大命令只有主要部分落在 held window 才 skip |
| foreground / force / urgent discard | 延迟可能影响空间回收或前台 I/O | policy bypass，立即 release held windows |
| UMOUNT teardown | 不能把工作拖到卸载阶段 | UMOUNT 前 release 所有 held windows，并统计 residual |
| low free space / high pending backlog | hold 过多导致空间压力 | max held groups、cmd/blk pressure、优先释放 short hold |
| window 不再增长 | 无效等待 | stagnation 检测，提前 release |
| gap 很近但中间是 live block | 不能强行合并 D-L-D | 只优化 visibility 和 issue timing，不伪造跨 live block discard |
| qcov/lres stale | 错误 WCE target | refresh affected groups；WCE 只用最新 wce_segmap |
| fragment window 被误交给 WCE | GC copy cost 增加 | `wce_eligible` gate，SHORT_HOLD 默认不进入 WCE |
| 统计把 timeout 当失败 | fragment batching 不一定以 ready 为成功 | 区分 capture/mature/timeout/pressure release |
| 计时精度不足 | 100us 级机会无法精确 hold | 设计上使用 existing issue cadence + short ms budget，不依赖高精度 timer |

## 8. 实验验证方案

修改目标是“利用更多合并机会进行合并”，所以实验必须证明：hold 期间确实捕获了 follow-up，最终 BG command 更少/更长，且没有把工作拖到 UMOUNT。

### 8.1 主指标

| 指标 | 解释 | 期望变化 |
| --- | --- | --- |
| BG done commands / GiB discarded | 相同 discard 工作量下设备命令数 | 下降 |
| BG done length CDF / median / P90 | 最终发射粒度 | 右移，median/P90 上升 |
| BG done `<=16 blocks` ratio | small command 是否减少 | 下降 |
| UMOUNT done count | 是否只是推迟到 teardown | 不增加，最好下降 |
| pending backlog max/avg | 是否积累过多 pending work | 不显著增加 |
| foreground throughput / P99 latency | 系统可用性 | 不恶化，最好改善 |

### 8.2 Formation-aware 专属指标

| 指标 | 含义 | 为什么重要 |
| --- | --- | --- |
| captured follow-up count | held 期间同 window 新出现的 create 数 | 证明不是盲目等待 |
| captured blocks | held 期间新增 covered blocks | 证明捕获到实际合并机会 |
| coverage gain | release 时覆盖范围相对 hold start 增长 | 对应 motivation Fig.3 |
| release avg_piece gain | 平均 fragment size 是否变大 | 证明 command formation 改善 |
| release breakdown | capture/mature/timeout/pressure 比例 | 证明策略准确性和安全性 |

### 8.3 对照组

| 配置 | 目的 |
| --- | --- |
| Native F2FS | 基础 baseline |
| Static delay all small | 证明简单等待不够 |
| Old SWOD qcov/lres | 证明 near-complete-only 不够 |
| Formation-aware SWOD | 证明新模型利用更多小碎片形成机会 |
| Formation-aware SWOD + WCE gate | 验证成熟窗口 completion 的边际收益 |

## 9. 分阶段实施计划

| 阶段 | 目标 | 改动 | 通过标准 |
| --- | --- | --- | --- |
| Phase 0: instrumentation only | 不改变行为，收集 formation features | 新增 counters/trace、输出 qcov/nr_cmds/avg_piece/growth | 与 native 结果一致，无性能影响 |
| Phase 1: SHORT_HOLD | 实现 formation-aware short hold | 新增 action、score、short hold、release reason | captured follow-up/coverage gain 明显，UMOUNT 不增加 |
| Phase 2: WCE gate | 将 WCE 与 held 解耦 | 新增 wce_segmap/wce_eligible，gc.c 只用 wce targets | GC fallback 不升高，low-maturity window 不进 WCE |
| Phase 3: adaptive tuning | 根据 timeout/pressure 调整阈值 | 可选 EWMA/feedback | timeout 降低，压力下自动收缩 |
| Phase 4: full evaluation | 完整对比 native/static/old/new | 跑 fileserver/varmail/OLTP | BG command 改善且前台不恶化 |

## 10. 推荐默认参数

以下参数是保守起点，应通过 sensitivity analysis 微调。

| 参数 | 建议初值 | 说明 |
| --- | --- | --- |
| `swod_short_hold_min_ms` | 1 | short formation hold 最小等待 |
| `swod_short_hold_max_ms` | 10 | 避免过长 hold；可按 issue cadence 调整 |
| `swod_frag_min_cmds` | 8 | 至少有一组 small fragments |
| `swod_frag_max_avg_piece_blks` | 16 | 平均 fragment 仍是 small |
| `swod_frag_min_pend_blks` | 32 | 避免只为极小窗口 hold |
| `swod_growth_min_bp` | 可从 trace percentile 取值 | 判断是否仍在增长 |
| `swod_wce_qcov_thr_bp` | 7000 | 成熟窗口进入 WCE 的 qcov 门槛 |
| `swod_wce_lres_thr_bp` | 2000 | 成熟窗口进入 WCE 的 live residual 上限 |
| `swod_overlap_skip_ratio_bp` | 5000 | 大 command 至少 50% overlap 才 skip |
| `swod_max_held_groups` | 64 | 防止全局 hold 过多 |

## 11. 成功标准

- 相同 workload 下，BG done command count / GiB discarded 下降。
- BG done length distribution 右移，small command ratio 下降。
- UMOUNT residual command count 不增加，最好下降。
- held window 的 captured follow-up 和 coverage gain 能解释最终 command 改善。
- timeout/pressure release 不应占主导；否则说明机会模型误判。
- pending backlog、foreground throughput、P99 latency 不恶化。
- WCE 只在成熟窗口上生效，不对 low-coverage fragment window 造成 GC fallback 或 copy cost 激增。

## 12. 建议的论文设计表述

设计章节可以用以下语义组织，避免看起来像简单调参或 workload-specific 分类。

```text
SWOD is a formation-aware discard shaping layer built on top of F2FS pending discard management. Instead of treating a newly created small discard command as an independent issue unit, SWOD tracks the short-term evolution of the local segment window in which the command appears. If nearby fragments are still arriving and the window is likely to expose additional safe coalescing opportunities, SWOD briefly holds the corresponding commands and releases them once the window matures, stops growing, or the system enters pressure. Only windows that become sufficiently mature are exposed to completion-assisted paths; all other windows fall back to stock F2FS issue behavior.
```

## 13. 参考依据

- redisc README：当前版本 SWOD 目标是在 pending discard 队列之上识别并 hold 高价值窗口，WCE 围绕 held windows 做 completion，SFI 为辅路径。
- redisc README：当前机会度量使用 `qcov` 与 `lres`，并明确 SWOD v1 不显式建模 latent invalid。
- redisc README：当前 hold 条件为 `qcov >= Tq` 且 `lres <= Tl`，bounded waiting 由窗口长度、qcov、lres 启发式决定。
- redisc README：WCE 当前采用 held-target-first filter，在 held target 中优先选择 victim，失败后回退 stock picker。
- redisc README：当前版本边界包括每个 group 最多一个 held 子窗口、不显式建模 latent invalid、WCE target-first、最终仍由 stock F2FS materialize 与 issue。
