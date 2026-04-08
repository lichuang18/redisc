# SWOD for F2FS (Linux 5.15)

## 1. 项目概述

本次改动在现有 F2FS discard 框架之上实现了 **SWOD（Segment-Window Opportunity-aware Discard）**。  
SWOD 的核心目标不是重写 F2FS 的 discard 机制，也不是直接修改写入路径，而是在 **pending discard 队列之上** 增加一个轻量的 **segment-window opportunity model**，用于识别那些**当前不宜立即发出、而更值得暂时保留**的局部 discard 候选窗口。

SWOD 的设计初衷如下：

- 现有 F2FS discard 调度主要依据**当前已经形成的 pending discard 请求**做决策；
- 对于大量**短小、密集、局部相关**的小 discard，请求当前长度不足以表达其未来潜在的合并价值；
- 如果这些小 discard 被过早发出，就会提前消费掉其后续演化为 **segment-run discard** 的机会；
- 因此，需要一种轻量机制，在 issue 侧先识别并保留这类机会，再为后续的补全机制（如 BG GC 或源头侧塑形机制）留出时间，使其在之后的 checkpoint / prefree / discard materialization 阶段自然形成更连续的 discard。

本实现面向 **Linux 5.15**，并尽量保持与 stock F2FS 的既有结构兼容。

---

## 2. 设计目标

当前版本的设计已经定稿，整体采用：

1. **SWOD**：在 pending discard 队列之上识别并 hold 高价值窗口；
2. **WCE（主路径）**：围绕这些 held windows 优先做 completion，让窗口更快接近 `READY-FOR-MATERIALIZATION`；
3. **Selective Fragment IPU / SFI（辅路径）**：只在 LFS 场景下，对 non-target zone 中少量“小、碎、老、非热”的 fragment 给一次很窄的 IPU 例外，以减少新的 tiny discard 继续形成。

SWOD 只解决以下问题：

> 对于已经进入 pending discard 队列的小 discard 请求，哪些局部 segment window 更值得**先不发**，从而为后续补全整段/连续段创造条件？

本版 WCE 解决的问题是：

> 当 SWOD 已经保住一些 held windows 之后，如何让 GC 优先围绕这些目标段工作，使窗口更快成熟？

本版 SFI 解决的问题是：

> 在不干扰 held windows completion 的前提下，如何尽量少让 non-target zone 继续长出更多 tiny discard / partial-invalid fragments？

SWOD / WCE / SFI **都不负责**：

- 直接重写 `ipu_policy`
- 直接修改 `alloc_mode`
- 直接重排前台写入落点
- 直接将 held 窗口强行转化为大 discard 并立即发出
- 主动伪造新的大 discard run
- 把 SSR 作为当前版本的主策略

因此，当前版本的职责是：

1. **识别机会窗口**
2. **暂缓发射（hold）**
3. **在系统高压时及时让路**
4. **向 WCE 暴露 held target set**
5. **在 GC 侧优先清理 held windows 中仍残留 live blocks 的段**
6. **在 non-target zone 上仅对少量高性价比旧碎片提供一次窄 IPU 例外**
7. **仍由 stock F2FS 负责最终 materialization 与 issue**

---

## 3. 设计原理

### 3.1 基本思想

SWOD 不是“找出更值得现在发出去的 discard”，而是“找出更值得先不发的 discard 窗口”。

对于某个局部连续 segment window，如果其已经积累了较高比例的 queue-visible pending discard，且仅剩少量 live blocks 尚未清除，那么：

- 立即将窗口内部的小 discard 逐条发出，可能会过早消耗未来合并机会；
- 暂时保留这些请求，则有机会等待后续 BG GC 或更新路径补全剩余 live blocks；
- 当窗口后续自然演化为更规整的 segment-run discard 后，再由 stock F2FS 路径正常发出。

换言之，SWOD 的动作是：

- **hold**
- 不是 **submit**

---

### 3.2 窗口建模

SWOD 的基本单位不是单条 discard command，而是一个由连续若干 segment 组成的 **segment group/window**。

窗口大小由以下原则决定：

- segment 是 F2FS 最自然的空间管理粒度；
- 对于 Linux 5.15，本实现通过 `request_queue->limits.max_discard_sectors` 估计软件侧允许的最大 discard 大小；
- 再结合 `SEGMENT_SIZE(sbi)` 计算一个合理的默认 `swod_win_segs`；
- 同时施加一个实现上界 `SWOD_MAX_WIN_SEGS`，防止窗口过大。

默认窗口段数的估计逻辑为：

\[
W = \max\left(1,\ \min\left(W_{cap},\left\lfloor \frac{max\_discard\_bytes}{seg\_bytes}\right\rfloor\right)\right)
\]

其中：

- \(W\)：窗口包含的 segment 数
- \(W_{cap}\)：实现上界
- \(max\_discard\_bytes\)：块层允许的单次 discard 上限
- \(seg\_bytes\)：单个 F2FS segment 的字节数

---

### 3.3 轻量状态

为了避免在每次 issue 时扫描整棵 discard RB-tree，SWOD 仅维护以下轻量状态：

#### 1）per-segment 摘要：`swod_seg_hint`

每个 segment 维护：

- `pend_blks`：当前该 segment 内已进入 pending discard、且仍处于 `D_PREP` 的块数
- `nr_cmds`：覆盖该 segment 的 pending discard 命令数量
- `oldest_jiffies`：该 segment 内最早进入队列的命令时间

#### 2）per-group 状态：`swod_group_hint`

每个 group 最多维护一个 held 子窗口：

- `state`：当前 group 是否处于 `SWOD_G_HELD`
- `hold_off`：held 子窗口在 group 内的起始偏移
- `hold_len`：held 子窗口包含的 segment 数
- `hold_until`：最晚等待截止时间
- `last_eval`：最近一次评估时间

#### 3）全局 held 位图：`hold_segmap`

用于快速判断某个 segment 当前是否属于 held window，同时也方便后续补全机制查询目标段。

---

### 3.4 机会度量

对于 group 内任意连续子窗口 \(r\)，定义：

\[
C(r)=|r| \cdot B_{seg}
\]

其中：

- \(|r|\)：窗口包含的连续 segment 数
- \(B_{seg}\)：单个 segment 的块数（`BLKS_PER_SEG(sbi)`）

进一步定义：

\[
Q(r)=\sum pend\_blks(i)
\]

\[
L(r)=\sum valid\_blks(i)
\]

其中：

- \(Q(r)\)：窗口内已经显化为 queue-visible pending discard 的块数
- \(L(r)\)：窗口内当前仍为 valid 的 live blocks 数

基于此，定义两个核心比率：

\[
qcov(r)=\frac{Q(r)}{C(r)}
\]

\[
lres(r)=\frac{L(r)}{C(r)}
\]

含义如下：

- `qcov(r)`：该窗口中已经显化到 pending discard 队列的机会比例
- `lres(r)`：该窗口距离整段/连续段完成还剩多少 live-block 障碍

SWOD v1 不显式建模 latent invalid，仅使用 `qcov + lres` 作为在线、轻量的机会代理量。

---

### 3.5 hold 判定条件

当某个窗口满足：

\[
qcov(r) \ge T_q
\]

\[
lres(r) \le T_l
\]

并且当前系统处于“允许等待”的正常背景模式时，SWOD 将该窗口判定为值得 hold 的机会窗口。

其中：

- \(T_q\)：queue-visible coverage 阈值，对应 `swod_qcov_thr_bp`
- \(T_l\)：live residual 阈值，对应 `swod_lres_thr_bp`

多个候选子窗口之间采用以下优先级：

1. 更长的连续 run 优先；
2. 若长度相同，则 `lres` 更小者优先；
3. 若仍相同，则 `age` 更老者优先。

---

### 3.6 bounded waiting

一旦某个窗口进入 `HELD` 状态，SWOD 不会无限期等待，而是使用有界等待时间：

\[
H(r)\in [H_{min}, H_{max}]
\]

本实现中等待时间由以下因素启发式决定：

- 窗口越长，适当增加等待时间；
- `qcov` 越高，适当增加等待时间；
- `lres` 越低，适当增加等待时间；

同时始终限制在：

- `swod_hold_min_ms`
- `swod_hold_max_ms`

之间。

---

### 3.7 状态机

SWOD 仅维护两个主状态：

- `SWOD_G_NORMAL`
- `SWOD_G_HELD`

其工作流程为：

1. 当 group 被重新评估且发现存在满足条件的最佳子窗口时，进入 `HELD`；
2. issue 路径扫描到完全落在 held 子窗口内部的 `D_PREP` cmd 时，先跳过，不发；
3. 当满足以下任一条件时，held 状态被释放：
   - held 窗口已经自然演化为更规整的 segment-run discard
   - 等待时间超时
   - 系统进入高压 / urgent / force 模式

---

## 4. 多窗口支持方式

本实现**不是只有一个全局窗口**。

当前版本的并发策略为：

- **每个 group 最多保留一个最佳 held 子窗口**
- **全局允许多个 group 同时处于 held 状态**
- 全局 held group 数量由 `swod_max_held_groups` 限制

因此，并发 held 窗口上限为：

\[
N_{held}^{max} = \min(nr\_groups,\ swod\_max\_held\_groups)
\]

其中：

- `nr_groups = ceil(nr_main_segs / swod_win_segs)`

这种设计的优点是：

- 避免 group 内多个窗口重叠冲突；
- 维持状态机简单；
- 保持 `should_skip()` 判定逻辑轻量；
- 便于与后续补全机制协同。

---

## 5. 最终方案：WCE 为主，Selective Fragment IPU 为辅

### 5.1 总体定位

这次定稿后的 discard 研究设计采用：

- **WCE 为主路径**：继续让 SWOD 先识别并 hold 高价值窗口，再由 WCE 围绕这些 held windows 优先做 completion；
- **Selective Fragment IPU（SFI）为辅路径**：只在 LFS 场景下，对 non-target zone 中极少量“小、碎、老、非热”的 fragment 给一次很窄的 IPU 例外；
- **stock F2FS 负责 materialization**：SWOD / WCE / SFI 都不直接伪造大 discard，真正的连续 discard run 仍通过 stock F2FS 的 checkpoint / prefree / issue path 自然形成。

也就是说，这版不是“全局转向 IPU”，而是：

> **target zone 继续围绕 held windows 做 completion；non-target zone 只做极窄、极保守的 fragment-IPU 辅助塑形。**

### 5.2 WCE 主路径

当前 WCE 采用的是 **target-first** 而不是原始文档里的 `gain(s)` / tie-break 软偏置：

- 只要当前 SWOD 维护的 held windows 还存在；
- 且 GC 工作在允许 WCE 的模式下；
- victim 选择就先只在这些 held target 覆盖到的 segment / section 中进行；
- 如果这一轮完全找不到合法 victim，再一次性回退到 stock F2FS 的全局选择。

换句话说，本版 WCE 的核心不是重写 cleaner 成本模型，而是给 stock victim picker 外面套一层 **held-target-first filter**。

它服务的目标是：

- 优先减少 held windows 中的 residual live blocks；
- 让窗口更快沿着 `NORMAL -> HELD -> NEAR-COMPLETE -> READY-FOR-MATERIALIZATION` 演化；
- 最终由 stock F2FS 自然 materialize 成更连续的 discard run。

### 5.3 Selective Fragment IPU（SFI）辅路径

SFI 不是全局 IPU 策略，而是一个非常窄的辅助路径。

它只在以下前提下考虑放行：

- 当前系统处于 `LFS` 场景；
- 当前 SWOD 仍然有 held windows；
- 旧块所在 segment **不属于 held target window**；
- 该 segment 表现为“小、碎、老、非热”的 pending-discard fragment。

其筛选依据直接复用 SWOD 已维护的 segment 侧摘要：

- `pend_blks`：该 segment 内当前可见 pending discard 块数
- `nr_cmds`：该 segment 的碎片度
- `oldest_jiffies`：这些 pending fragment 的年龄
- `hold_segmap / nr_held_groups`：该 segment 是否属于 held target，以及系统当前是否还有 held windows

再叠加：

- non-target only
- skip hot data
- age threshold
- tiny fragment threshold

SFI 的目标不是帮助 held window completion，而是：

- 不去动 held windows；
- 尽量少让其他地方继续长出新的 tiny discard；
- 作为 WCE 主路径之外的一个 source-side shaping 辅助动作。

### 5.4 为什么这次不把 SSR 当主策略

这次设计里没有把 SSR 作为主路径，原因是当前 F2FS 代码条件下，`f2fs_need_SSR()` 在 `f2fs_lfs_mode(sbi)` 下直接返回 `false`。

因此，在当前代码基线下：

- SSR 并不是一个可以自然承接这版 shaping 设计的主通路；
- 更现实的落点是：保持 WCE 作为 completion 主路径，再通过一个非常窄的 fragment-IPU 例外去抑制 non-target zone 的 tiny fragment 继续增长。

### 5.5 运行语义

本版最终语义可以概括为：

1. SWOD 继续负责 `identify / hold / skip / release`；
2. WCE 在 held windows 上优先做 completion；
3. SFI 只在 non-target zone 上做极窄的 fragment-IPU 辅助；
4. 三者都不直接 issue discard，也不直接伪造大 discard；
5. 真正的 discard materialization 仍交给 stock F2FS。

因此，这一版形成的是：

> **SWOD 保住机会窗口，WCE 优先清掉 held windows 里的 residual live blocks，SFI 只在 non-target zone 上抑制新的 tiny fragment 继续形成，最终仍由 stock F2FS 自然 materialize 成更连续的 discard。**

## 6. 代码改动概述

### 5.1 新增文件

#### `fs/f2fs/swod.h`
定义 SWOD 相关数据结构与接口，包括：

- `struct swod_seg_hint`
- `struct swod_group_hint`
- `struct swod_ctrl`
- `f2fs_swod_init()`
- `f2fs_swod_destroy()`
- `f2fs_swod_refresh_around_locked()`
- `f2fs_swod_should_skip_locked()`
- `f2fs_swod_release_all()`
- `f2fs_swod_has_held()`
- `f2fs_swod_range_held()`
- `f2fs_swod_seg_held()`

同时在 `struct swod_ctrl` 中增加了 WCE / SFI 统计项：

- `gc_pick_bg_cnt`
- `gc_pick_fg_cnt`
- `gc_fallback_cnt`
- `frag_ipu_pick_cnt`
- `frag_ipu_skip_target_cnt`
- `frag_ipu_skip_hot_cnt`
- `frag_ipu_skip_age_cnt`
- `frag_ipu_skip_shape_cnt`

#### `fs/f2fs/swod.c`
实现 SWOD 核心逻辑，包括：

- 默认窗口大小计算
- group 局部重建
- held 候选评估
- hold/release 状态维护
- issue 路径 skip 判定
- 面向 WCE 的 held target 查询接口
- 面向 SFI 的 fragment-IPU advisory selector

---

### 5.2 修改文件

#### `fs/f2fs/f2fs.h`

主要改动：

1. 在 `struct discard_cmd` 中增加：
   - `enq_jiffies`  
   用于记录命令进入 pending 队列的时间，支持 age 估计。

2. 在 `struct discard_cmd_control` 中增加：
   - `swod_enable`
   - `swod_win_segs`
   - `swod_qcov_thr_bp`
   - `swod_lres_thr_bp`
   - `swod_hold_min_ms`
   - `swod_hold_max_ms`
   - `swod_cmd_pressure`
   - `swod_blk_pressure`
   - `swod_max_held_groups`
   - `struct swod_ctrl *swod`

3. 在 `struct discard_cmd_control` 中增加 WCE 开关：
   - `swod_completion_enable`
   - `swod_gc_bg_enable`
   - `swod_gc_fg_enable`

4. 在 `struct discard_cmd_control` 中增加 SFI 开关与阈值：
   - `swod_frag_ipu_enable`
   - `swod_frag_ipu_max_pend_blks`
   - `swod_frag_ipu_min_cmds`
   - `swod_frag_ipu_age_ms`
   - `swod_frag_ipu_skip_hot`

作用：

- 将 SWOD 作为 discard 子系统的一层附加状态，绑定到 `discard_cmd_control` 生命周期；
- 允许在运行时单独控制 target-first WCE 是否生效，以及 BG/FG GC 是否参与；
- 允许在 non-target zone 上启用一个很窄的 fragment-IPU 辅助路径。

---

#### `fs/f2fs/segment.c`

主要改动：

1. `__create_discard_cmd()`
   - 为新建的 discard_cmd 初始化 `enq_jiffies`

2. `create_discard_cmd_control()` / `destroy_discard_cmd_control()`
   - 初始化与销毁 `swod_ctrl`

3. `__update_discard_tree_range()`
   - 支持在纯 queueing 场景下触发 SWOD 局部 refresh

4. `__submit_discard_cmd()`
   - 在 submit 结束后，对旧 D_PREP 覆盖范围统一 refresh
   - 保证 SWOD 看到“旧机会最终变成了什么”

5. `__punch_discard_cmd()`
   - 在局部 punch 结束后，对旧范围统一 refresh
   - 适配删掉中间一个块、保留左右两段的情况

6. `__remove_discard_cmd()`
   - 仅在被删除命令原状态为 `D_PREP` 时触发 refresh

7. `__issue_discard_cmd_orderly()`
   - 在 submit 前调用 `f2fs_swod_should_skip_locked()`
   - 对 ordered 路径的 held cmd 实现 skip

8. `__issue_discard_cmd()`
   - 在 list 路径的 submit 前调用 `f2fs_swod_should_skip_locked()`
   - 对 held cmd 实现 skip

9. `issue_discard_thread()`
   - 在系统进入 aggressive / urgent discard 模式时，释放所有 held window，退回 stock F2FS 行为

---

#### `fs/f2fs/gc.c`

本版新增 target-first WCE 逻辑，主要体现在 `get_victim_by_default()`：

1. 判断 WCE 是否允许进入 target pass；
2. 若当前存在 held windows，则优先只在 held target 集中选 victim；
3. `next_victim_seg[]` / `check_bg_victims()` / 普通扫描路径都要经过 held-range 匹配；
4. 若本轮 target pass 完全找不到合法 victim，则回退到 stock picker；
5. 对 BG / FG pick 次数和 fallback 次数做统计。

需要强调的是：

- 目标集内部仍然沿用 stock F2FS 的合法性检查和 cost 选择；
- WCE 没有引入新的 GC cost 模型；
- WCE 只是约束“先在哪些候选里找”。

#### `fs/f2fs/sysfs.c`

主要改动：

新增 SWOD / WCE / SFI 的 sysfs 参数与统计：

##### 可写参数
- `swod_enable`
- `swod_win_segs`
- `swod_qcov_thr_bp`
- `swod_lres_thr_bp`
- `swod_hold_min_ms`
- `swod_hold_max_ms`
- `swod_cmd_pressure`
- `swod_blk_pressure`
- `swod_max_held_groups`
- `swod_completion_enable`
- `swod_gc_bg_enable`
- `swod_gc_fg_enable`
- `swod_frag_ipu_enable`
- `swod_frag_ipu_max_pend_blks`
- `swod_frag_ipu_min_cmds`
- `swod_frag_ipu_age_ms`
- `swod_frag_ipu_skip_hot`

##### 只读统计
- `swod_held_groups`
- `swod_hold_cnt`
- `swod_skip_cnt`
- `swod_success_release_cnt`
- `swod_timeout_release_cnt`
- `swod_pressure_release_cnt`
- `swod_gc_pick_bg_cnt`
- `swod_gc_pick_fg_cnt`
- `swod_gc_fallback_cnt`
- `swod_frag_ipu_pick_cnt`
- `swod_frag_ipu_skip_target_cnt`
- `swod_frag_ipu_skip_hot_cnt`
- `swod_frag_ipu_skip_age_cnt`
- `swod_frag_ipu_skip_shape_cnt`

说明：

- 这些节点位于 `/sys/fs/ef2fs/<sb_id>/`
- 由于本树使用 `ef2fs` 作为 kset 名称，实际路径不是标准 mainline 的 `/sys/fs/f2fs/...`

---

#### `fs/f2fs/debug.c`（如果已改）

如果本次修改还同步扩展了 `stat_show()`，则 `/sys/kernel/debug/f2fs/status` 中会额外输出一行 SWOD 总览，例如：

- 是否启用
- 窗口大小
- 当前 held group 数
- hold/skip/release 统计

若未修改 `debug.c`，则 SWOD 状态只会出现在 sysfs 中，不会显示在 debugfs status。

---

## 6. 关键实现原则

### 6.1 SWOD 只关心 `D_PREP` 状态

SWOD 的机会判断只建立在**当前仍可被 issue thread 消费的 pending discard** 之上，因此：

- 只统计 `dc->state == D_PREP` 的命令
- `D_SUBMIT / D_PARTIAL / D_DONE` 不属于当前可 hold 的机会

这也是为什么：

- `__submit_discard_cmd()` 末尾必须 refresh
- `__remove_discard_cmd()` 只有删掉 `D_PREP` 才要 refresh

---

### 6.2 采用局部重建，而不是全量重扫

每当某一段逻辑范围对应的 D_PREP 可见机会发生变化时，SWOD 只围绕该范围映射到的 group 做局部重建，而不会重新扫描整棵 RB-tree 或整个文件系统。

这种设计保证了：

- 改动侵入性小
- 性能开销可控
- 实现复杂度明显低于精细化全量增量维护

---

### 6.3 issue 路径只做 skip，不做 submit

SWOD 的动作是：

- 保留机会
- 暂缓 issue

而不是：

- 直接生成更大 discard 并立刻发出

因此，SWOD 只在：

- `__issue_discard_cmd_orderly()`
- `__issue_discard_cmd()`

中插入一次 `should_skip()` 判断，真正的 discard 提交仍由 stock F2FS 完成。

---

## 7. Linux 5.15 适配说明

本实现运行于 Linux 5.15。  
由于该版本没有某些更新内核中的 helper，默认窗口大小的估计直接基于：

- `bdev_get_queue(bdev)`
- `q->limits.max_discard_sectors`

进行计算，而不是依赖更新版本的块层辅助函数。

这保证了实现与 5.15 的 block layer API 兼容。

---

## 9. 当前版本的边界与特性

当前版本是这轮 discard 研究设计的定稿版，其边界如下：

1. 每个 group 最多只保留一个 held 子窗口；
2. 不显式建模 latent invalid；
3. 仅对完全落在 held 子窗口内部的命令做 skip；
4. WCE 采用 target-first completion，而不是 `gain(s)` / tie-break 软偏置；
5. `f2fs_swod_range_held()` 目前只基于 held bitmap 做范围命中判断；
6. SFI 只在 LFS 场景、且存在 held windows 时生效；
7. SFI 只针对 non-target、tiny、fragmented、aged、non-hot 旧碎片；
8. SSR 不作为当前版本的主路径，因为当前代码基线下 `f2fs_need_SSR()` 在 `f2fs_lfs_mode(sbi)` 下直接返回 `false`；
9. 不主动提交“materialized”大 discard，而是交由 stock F2FS 自然处理。

这些边界是有意保留的，目的在于：

- 让主路径聚焦在 held-window completion；
- 让辅路径只做很窄的 fragment 抑制；
- 保持和 stock F2FS 的边界清晰；
- 便于实验分析每一条路径的真实效果。

---

## 10. 运行时调试建议

建议重点观察以下节点：

### sysfs
- `swod_enable`
- `swod_completion_enable`
- `swod_gc_bg_enable`
- `swod_gc_fg_enable`
- `swod_frag_ipu_enable`
- `swod_frag_ipu_max_pend_blks`
- `swod_frag_ipu_min_cmds`
- `swod_frag_ipu_age_ms`
- `swod_frag_ipu_skip_hot`
- `swod_held_groups`
- `swod_hold_cnt`
- `swod_skip_cnt`
- `swod_timeout_release_cnt`
- `swod_pressure_release_cnt`
- `swod_gc_pick_bg_cnt`
- `swod_gc_pick_fg_cnt`
- `swod_gc_fallback_cnt`
- `swod_frag_ipu_pick_cnt`
- `swod_frag_ipu_skip_target_cnt`
- `swod_frag_ipu_skip_hot_cnt`
- `swod_frag_ipu_skip_age_cnt`
- `swod_frag_ipu_skip_shape_cnt`

### 原有 F2FS/Discard 指标
- `pending_discard`
- `queued_discard`
- `issued_discard`
- `undiscard_blks`
- `gc_mode`
- `gc_background_calls`
- `moved_blocks_background`

若启用了 debugfs 扩展，还可结合：

- `/sys/kernel/debug/f2fs/status`

一起观察：

- WCE 是否真的在 held target 上 pick 到 victim；
- fallback 是否过多；
- held windows 的 residual live 是否下降得更快；
- SFI 是否只在 non-target old fragments 上被放行；
- SFI 是否在减少 tiny fragment 的继续增长。

---

## 11. 本文档的用途

当前版本已经记录了这轮 discard 研究设计的最终实现口径：

- 主路径是 WCE completion；
- 辅路径是 non-target zone 上的 Selective Fragment IPU；
- materialization 仍交给 stock F2FS。

后续若继续修改，应视为新一轮演进，而不是本轮设计定义的一部分。阅读本文件即可快速恢复：

- 这版设计为什么这样划分；
- 每条路径当前在代码中如何实现；
- 这版机制的目标边界与预期功效。

---

## 12. 总结

当前版本的本质不是“更激进地发出更长 discard”，而是：

> 在 issue 侧先识别并保留那些未来有潜力长成更连续 segment-run discard 的局部机会窗口，再由 WCE 优先围绕 held windows 清理 residual live blocks，同时只在 non-target zone 上用一个很窄的 fragment-IPU 例外去抑制新的 tiny fragment 继续增长，最终仍由 stock F2FS 自然完成 materialization 与 issue。

通过这一设计，系统已经形成了一个清晰的最终版闭环：

- **SWOD** 负责识别并保住机会；
- **WCE** 负责把 held windows 尽快推向可 materialize 状态；
- **SFI** 负责在不碰 held target 的前提下，减少 non-target zone 继续产生更多 tiny discard；
- **stock F2FS** 负责真正的连续 discard materialization 与 issue



4.8

# WCE parked-handoff redesign notes

## Goal
Let WCE keep useful GC targets even when SWOD-held windows time out or are pressure-released before BG GC starts.

## Design
- Keep original SWOD hold-selection logic.
- Add a second state `SWOD_G_PARKED` only for handoff.
- On `SWOD_REL_TIMEOUT` or `SWOD_REL_PRESSURE`, try to convert a `HELD` window into a parked WCE target instead of dropping it immediately.
- Parked windows still block overlapping discard commands, so pending discard ranges can continue to merge.
- WCE target lookup sees `HELD || PARKED`.
- SFI (`f2fs_swod_should_frag_ipu`) still only sees `HELD`, so parked targets do not broaden that policy.
- Explicit bypass / release-all paths drop both HELD and PARKED directly.

## Key filters for parking
A HELD window is parked only if all are true:
- it is being released for timeout or pressure;
- current qcov is still above `swod_qcov_thr_bp`;
- current live ratio is low enough: `lbp <= min(20%, swod_lres_thr_bp + 10%)` and `lbp > 0`.

Parking priority favors:
- colder data,
- longer runs,
- higher qcov,
- fewer valid blocks,
- older pending discard age.

## Lifetime / bounds
- Park budget: `max(4, swod_max_held_groups / 2)`.
- Park hold time: about `4x` original hold, clamped to `[500ms, 2000ms]`.
- When budget is full, replace only a weaker parked target.

## Files changed
- `swod.h`
- `swod.c`
- `gc.c`

## Important extra fix
In `gc.c`, the WCE target pass originally consumed `nsearched` once before target filtering and again after a match.
That makes the target pass run out of budget too early.
The patch changes it so only real WCE targets consume search budget during target pass.

## Apply
From repo root:

```bash
patch -p1 < wce_parked_handoff_apply.patch
```




