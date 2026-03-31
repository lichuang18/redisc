# SWOD 与动机二联合设计文档

## 文档信息

* 文档名称：SWOD 与动机二联合设计文档
* 面向版本：Linux 5.15 / F2FS
* 当前状态：本轮 discard 研究设计已定稿并已落地为“SWOD + target-first WCE + Selective Fragment IPU”版本；WCE 为主路径，SFI 为辅路径；真正的 materialization 仍由 stock F2FS 完成
* 文档用途：用于后续继续实现、论文撰写、实验规划与代码回溯

---

## 1. 背景与问题定义

### 1.1 问题背景

F2FS 是基于 LFS 的 flash-friendly 文件系统，其更新路径天然采用 out-of-place update。该机制虽然有利于顺序化写入，但会持续制造 obsolete/invalid blocks，并进一步引出 cleaning、metadata update 与 discard 等后续开销。

现有 F2FS 的 discard 线程工作在空间回收链路的末端，其主要职责是消费已经形成的 pending discard 请求，而不是决定这些请求如何在更新过程中产生、如何在空间上组织、以及是否具有进一步合并的潜力。因此，仅围绕 discard issue thread 做参数调优，难以从根本上减少 backlog 的形成。

与此同时，在真实更新负载中，大量 discard 请求往往呈现出小长度、高密度、局部相关的分布特征。若这些请求被过早发出，就会提前消耗掉未来可能成长为 segment-run discard 的局部机会。因此，系统需要一种机制：

1. 识别哪些局部窗口值得先不发；
2. 将这些窗口作为明确目标，优先减少其剩余 live blocks；
3. 同时控制其他区域继续生成新的 partial-invalid segments。

---

## 2. 设计目标

本文联合设计由两个层次组成：

### 2.1 SWOD 的目标

SWOD（Segment-Window Opportunity-aware Discard）用于解决动机一对应的问题：

* 在现有 pending discard 队列之上识别“值得先不发”的局部 segment window；
* 对这些窗口内部的小 discard 进行短暂 hold，而不是立即 issue；
* 为后续补全机制留出时间，使这些窗口在后续 checkpoint / prefree / BG GC / 更新路径作用下，自然演化为更连续的 discard 候选。

### 2.2 动机二对接机制的目标

动机二的后续设计不再重新寻找机会窗口，而是直接消费 SWOD 已经识别并 hold 的窗口，目标包括：

* 更快减少 held windows 中的 residual live blocks；
* 尽量避免系统在其他区域继续制造新的 partial-invalid segments；
* 最终使 held windows 更快 materialize 为连续 discard run，由 stock F2FS 自然发出。

---

## 3. 总体架构

联合架构采用“先保留机会，再定向补全”的两段式闭环：

```text
pending discard
     |
     v
SWOD selector
(识别并 hold 住机会窗口)
     |
     v
Completion Engine
(优先补全 held windows 的残余 live blocks)
     |
     v
Source-side Shaping
(减少非目标区域继续产生新的 backlog)
     |
     v
stock F2FS
(checkpoint / prefree / issue_discard 自然发出更连续 discard)
```

在该架构中：

* SWOD 是 selector，不直接负责“完成窗口”；
* 动机二对应的 Completion Engine 和 Source-side Shaping 是 actuator，负责让窗口更快成熟；
* 真正的 discard materialization 和发射仍然交给 stock F2FS 路径完成。

---

## 4. SWOD 设计（已完成）

### 4.1 设计定位

SWOD 不重写 F2FS 的 discard 框架，也不直接修改写入路径；它只在现有 pending discard 队列之上增加一个轻量的机会模型，用于回答：

> 哪些局部 segment window 已经积累了足够高的 queue-visible discard，且仅剩少量 live blocks 尚未清除，因而值得先不发、保留未来的 segment-run 合并机会？

SWOD 的动作是：

* hold
* skip
* release

SWOD **不是**一个“更激进地优先发长 discard”的调度器，也不会主动提交 materialized 大 discard。

### 4.2 基本单位：segment group / window

SWOD 的基本对象不是单条 discard command，而是由连续若干 segment 组成的 segment group。

窗口大小记为：

[
W = \max\left(1,\ \min\left(W_{cap},\left\lfloor \frac{max_discard_bytes}{seg_bytes}\right\rfloor\right)\right)
]

其中：

* (W)：窗口包含的 segment 数
* (W_{cap})：实现上界
* (max_discard_bytes)：块层允许的单次 discard 上限
* (seg_bytes)：单个 F2FS segment 的字节数

在 Linux 5.15 上，默认窗口大小通过 `request_queue->limits.max_discard_sectors` 与 `SEGMENT_SIZE(sbi)` 推导。

### 4.3 轻量状态

SWOD 维护以下状态：

#### 4.3.1 per-segment 摘要

每个 segment 对应一个 `swod_seg_hint`：

* `pend_blks`：当前该 segment 内处于 `D_PREP` 且对 issue 可见的 pending discard blocks
* `nr_cmds`：覆盖该 segment 的 pending discard 命令数量
* `oldest_jiffies`：最早进入队列的 pending discard 时间

#### 4.3.2 per-group 状态

每个 group 对应一个 `swod_group_hint`：

* `state`：`SWOD_G_NORMAL` / `SWOD_G_HELD`
* `hold_off`：held 子窗口在 group 内的起始位置
* `hold_len`：held 子窗口包含的连续段数
* `hold_until`：等待截止时间
* `last_eval`：最近一次评估时间

#### 4.3.3 全局控制结构

`swod_ctrl` 维护：

* `nr_main_segs`
* `win_segs`
* `nr_groups`
* `nr_held_groups`
* `seg_hint[]`
* `grp_hint[]`
* `hold_segmap`
* 统计项：`hold_cnt / skip_cnt / success_release_cnt / timeout_release_cnt / pressure_release_cnt`

### 4.4 机会度量

对于 group 内任意连续子窗口 (r)，定义：

[
C(r)=|r|\cdot B_{seg}
]

其中：

* (|r|)：子窗口包含的 segment 数
* (B_{seg})：单个 segment 的块容量

进一步定义：

[
Q(r)=\sum pend_blks(i)
]

[
L(r)=\sum valid_blks(i)
]

其中：

* (Q(r))：窗口内已经显化为 queue-visible pending discard 的块数
* (L(r))：窗口内当前仍然 live 的有效块数

于是定义：

[
qcov(r)=\frac{Q(r)}{C(r)}
]

[
lres(r)=\frac{L(r)}{C(r)}
]

其中：

* `qcov(r)`：queue-visible coverage
* `lres(r)`：live residual

SWOD v1 不显式估计 latent invalid，而是通过 bounded waiting 在线吸收未来演化不确定性。

### 4.5 hold 判定

若窗口满足：

[
qcov(r)\ge T_q
]

[
lres(r)\le T_l
]

则认为该窗口具有保留价值。

其中：

* (T_q)：queue-visible coverage 阈值，对应 `swod_qcov_thr_bp`
* (T_l)：live residual 阈值，对应 `swod_lres_thr_bp`

在多个候选中，优先级采用字典序：

1. 连续 run 更长优先
2. `lres` 更小优先
3. `age` 更老优先

### 4.6 bounded waiting

窗口进入 HELD 后，不会无限等待，而采用有界等待：

[
H(r)\in [H_{min}, H_{max}]
]

其中：

* `swod_hold_min_ms`
* `swod_hold_max_ms`

当前实现采用启发式规则：

* run 越长，可适当多等
* `qcov` 越高，可适当多等
* `lres` 越低，可适当多等

### 4.7 多窗口支持

当前实现不是全局只有一个窗口，而是：

* 每个 group 最多 hold 一个最佳子窗口
* 多个 group 可以同时进入 HELD
* 全局 held group 数量由 `swod_max_held_groups` 控制

并发 held 窗口上限为：

[
N_{held}^{max}=\min(nr_{groups},\ swod_max_held_groups)
]

### 4.8 SWOD 当前职责

SWOD 只做：

* 识别机会窗口
* hold 小 discard
* skip issue
* 在高压模式下 release/bypass

SWOD 不做：

* 直接发出大 discard
* 直接指定 GC victim
* 直接修改全局更新路径

---

## 5. 动机二对接设计（本轮设计定稿）

动机二的设计分为两个子层：

### 5.1 子层 A：Window Completion Engine（WCE）

目标：

* 优先减少 held windows 中的 residual live blocks；
* 让 held windows 更快从 near-ready 变成真正可 materialize 的连续 discard run。

#### 5.1.1 原始设想

最初设计中，WCE 的原则是：

* 优先放在 `BG_GC` 生效；
* 只在非 urgent / 非 force 模式生效；
* 不重写 stock F2FS cleaner，只做 bounded completion bonus / tie-break。

对应原始接口与度量设想包括：

```c
struct swod_target {
    unsigned int first_segno;
    unsigned int nr_segs;
    unsigned int qcov_bp;
    unsigned int lres_bp;
    unsigned long hold_until;
    unsigned long age_jiffies;
};
```

以及：

* `f2fs_swod_collect_targets()`
* `completion gain(s)`
* `cost_stock(s) + gain(s)` 的 bounded tie-break 结合方式

这套方案的优点是偏置更软、更贴近 stock cleaner，但实现复杂度更高。

#### 5.1.2 当前已实现版本：target-first WCE

当前代码落地的不是上述软偏置方案，而是一个更直接的 **target-first WCE**。

其核心语义是：

* 只要当前 SWOD 仍有 held windows；
* 且 GC 处于允许 WCE 生效的模式；
* victim selection 就先只在 held target 覆盖到的段 / section 中寻找合法 victim；
* 若这轮 target pass 完全找不到合法 victim，再一次性回退到 stock F2FS 的全局选择。

换句话说，当前版本不是通过 `gain(s)` 对所有候选做排序微调，而是：

> **先把 SWOD already-held windows 作为 GC 的优先目标集；在目标集内部仍沿用 stock F2FS 的合法性检查与 cost 选择。**

#### 5.1.3 当前版本为什么采用 target-first

原因主要有三点：

1. SWOD 已经维护了 `hold_segmap`，天然提供了“哪些 segment 当前属于 held window”的快速目标接口；
2. 基于 `hold_segmap` 增加一个 target-first 筛选层，改动最小，能最快形成 `SWOD -> GC completion -> stock materialization` 的最小闭环；
3. 这更贴合当前实现阶段的硬约束：**窗口中还有 segment，就优先从这里选 victim。**

因此，当前版本没有先实现：

* `f2fs_swod_collect_targets()`
* `gain(s)`
* `tie_margin`
* `bonus_max`

而是先走一条更直接、更容易验证的路径。

#### 5.1.4 当前代码中的 SWOD -> WCE 接口

当前代码通过以下接口向 WCE 暴露 held target set：

* `f2fs_swod_has_held()`
* `f2fs_swod_range_held()`
* `f2fs_swod_seg_held()`

其中核心接口是 `f2fs_swod_range_held()`：

* 只要某个 victim range 与 held bitmap 有交集，就认为该 range 命中 held target set；
* 当前仍是基于 `hold_segmap` 的轻量 range 命中判断，而不是基于 per-window residual live / qcov / age 的细粒度 completion score。

#### 5.1.5 当前与 stock GC 的结合方式

当前版本与 stock GC 的结合方式为：

* target pass：先只在 held target set 中选；
* target set 内部：仍按 stock F2FS 的合法性检查与原始 cost 选择；
* 若 target pass 没选到合法 victim：fallback 一次，退回 stock picker。

因此它的定位是：

* **不是重写 cleaner cost model**；
* **不是主动发明新的大 discard**；
* **只是把 SWOD 保住的窗口转化为 GC 的优先目标集。**

### 5.2 子层 B：Selective Fragment IPU（SFI，辅路径）

目标：

* 在不干扰 held windows completion 的前提下；
* 尽量减少 non-target zone 继续形成新的 tiny discard / partial-invalid fragments。

#### 5.2.1 设计定位

这一版不是把 source-side shaping 做成“全局偏向 IPU/SSR”。

本轮最终采用的是一个更窄的辅路径：

* 只在 `LFS` 场景下考虑；
* 只在系统当前确实存在 held windows 时考虑；
* 只对 **non-target zone** 上极少量“小、碎、老、非热”的 fragment 给一次 IPU 例外；
* target zone 仍然坚持不碰 IPU，避免和 WCE completion 打架。

换句话说，当前不是“SSS 全量落地”，而是把 non-target zone 的 shaping 收敛成一个更容易控制的 **Selective Fragment IPU**。

#### 5.2.2 为什么 target zone 仍保持 OPU

held windows 的目标是让其中剩余 live blocks 尽快消失。

若某个旧块位于 held window 内：

* 若未来更新采用 OPU，则新版本写到别处，旧位置在目标窗口内变 invalid；
* 若采用 IPU，则旧位置被原地覆盖，反而继续保留“有效性”。

因此，对 target zone 的策略保持不变：

```text
若旧块位于 held window 内 -> 禁止 IPU，保留 OPU
```

#### 5.2.3 为什么 non-target zone 只做窄 IPU 例外

当前代码基线下，`f2fs_need_SSR()` 在 `f2fs_lfs_mode(sbi)` 下直接返回 `false`，因此 SSR 并不是这版设计可自然承接的主路径。

在这个前提下，更现实的 shaping 方式是：

* 不动 held target；
* 不把系统全局推向 IPU；
* 只对 non-target zone 上一小部分“小、碎、老、非热”的 fragment 做一次很克制的 IPU 例外。

这样做的目的不是“完成 held windows”，而是：

* 减少其他区域继续长出更多 tiny discard；
* 控制 future discard demand 的继续膨胀；
* 作为 WCE 主路径之外的辅助抑制动作。

#### 5.2.4 当前代码中的筛选依据

SFI 直接复用 SWOD 已维护的 segment 侧摘要与 held 状态：

* `pend_blks`：反映 tiny fragment 规模
* `nr_cmds`：反映碎片度
* `oldest_jiffies`：反映 fragment 年龄
* `hold_segmap` / `nr_held_groups`：判断当前是否存在 held windows，以及某个 segment 是否属于 target zone

再叠加：

* non-target only
* skip hot data
* age threshold
* tiny fragment threshold

因此，SFI 是一个 advisory-only 的窄筛选器，而不是全局策略切换器。

---

## 6. 联合状态机

联合设计的完整状态链如下：

```text
NORMAL
  |
  |  SWOD 识别高 qcov + 低 lres 窗口
  v
HELD
  |
  |  WCE 通过 target-first completion 减少 residual live
  |  SFI 在 non-target zone 抑制新的 tiny fragment 增长
  v
NEAR-COMPLETE
  |
  |  residual live -> 0
  v
READY-FOR-MATERIALIZATION
  |
  |  checkpoint / prefree / stock issue path
  v
MATERIALIZED DISCARD RUN
  |
  v
ISSUED BY STOCK F2FS
```

其中：

* SWOD 不直接发出大 discard
* WCE/SFI 也不直接伪造新的大 discard
* 真正的 materialization 仍交给 stock F2FS 完成

---

## 7. 代码落点设计

### 7.1 已完成：SWOD 实现落点

* `fs/f2fs/f2fs.h`

  * `discard_cmd` 增加 `enq_jiffies`
  * `discard_cmd_control` 增加 SWOD tunable 与 `swod_ctrl *`
* `fs/f2fs/swod.h`

  * 定义 SWOD 结构与接口
* `fs/f2fs/swod.c`

  * 实现窗口评估、状态机、hold/skip/release
* `fs/f2fs/segment.c`

  * 在 queue / submit / punch / remove / issue 路径上加 hook
* `fs/f2fs/sysfs.c`

  * 暴露 SWOD 参数与统计

### 7.2 已完成：WCE 落点

当前已完成的落点：

* `fs/f2fs/gc.c`

  * 已在 victim selection 中接入 target-first WCE
  * 已支持 held-target-first pass + stock fallback
  * 已支持 BG / FG 的独立开关与统计
* `fs/f2fs/swod.c`

  * 已提供 `has_held()` / `range_held()` / `seg_held()` 查询接口

### 7.3 已完成：SFI 落点

当前已完成的落点：

* `fs/f2fs/f2fs.h`

  * 已增加 `swod_frag_ipu_*` tunables
* `fs/f2fs/swod.c`

  * 已增加 `f2fs_swod_should_frag_ipu()` 作为 advisory-only selector
  * 仅在 LFS + held windows 存在 + non-target + non-hot + tiny/fragmented/aged 条件下放行
* `fs/f2fs/swod.h`

  * 已增加 SFI 对外接口与统计项
* `fs/f2fs/sysfs.c`

  * 已增加 SFI 的开关、阈值与 skip/pick 统计

需要强调的是：

* SFI 是 non-target zone 上的一个很窄的辅助路径；
* 它不是全局 IPU 开关；
* 也不是以 SSR 作为主策略的 shaping 版本。

---

## 8. 参数设计

### 8.1 SWOD 已有参数

* `swod_enable`
* `swod_win_segs`
* `swod_qcov_thr_bp`
* `swod_lres_thr_bp`
* `swod_hold_min_ms`
* `swod_hold_max_ms`
* `swod_cmd_pressure`
* `swod_blk_pressure`
* `swod_max_held_groups`

### 8.2 WCE 参数

当前已实现：

* `swod_completion_enable`
* `swod_gc_bg_enable`
* `swod_gc_fg_enable`

### 8.3 SFI 参数

当前已实现：

* `swod_frag_ipu_enable`
* `swod_frag_ipu_max_pend_blks`
* `swod_frag_ipu_min_cmds`
* `swod_frag_ipu_age_ms`
* `swod_frag_ipu_skip_hot`

---

## 9. 本轮设计的最终实现形态

本轮 discard 研究设计已经结束，最终实现形态如下：

### 9.1 SWOD

目标：

* 识别并 hold 高价值窗口
* 为后续 completion 暴露 held target set

### 9.2 WCE（主路径）

目标：

* 围绕 held windows 优先做 completion
* 让窗口更快接近 `READY-FOR-MATERIALIZATION`

实现形态：

* target-first victim selection
* held-target-first pass + stock fallback

### 9.3 SFI（辅路径）

目标：

* 不干扰 held window completion
* 减少 non-target zone 继续形成 tiny discard / fragment backlog

实现形态：

* LFS-only
* held-windows-present only
* non-target only
* tiny / fragmented / aged / non-hot only

### 9.4 stock F2FS

目标：

* 继续负责 checkpoint / prefree / issue path
* 自然完成 discard materialization 与发射

---

## 10. 实验与消融建议

建议按下列组别做消融：

1. **stock F2FS**
2. **SWOD-hold-only**
3. **SWOD + WCE**
4. **SWOD + SSS(target keep OPU)**
5. **SWOD + WCE + SSS**

重点观察：

* `swod_hold_cnt`
* `swod_skip_cnt`
* `swod_held_groups`
* `swod_success_release_cnt`
* `pending_discard`
* `issued_discard`
* `undiscard_blks`
* `gc_background_calls`
* `moved_blocks_background`
* `avg_vblocks`

同时在 micro（fio）与 macro（tpcc/MySQL）两类负载下都做验证。

---

## 11. 当前版本已记录完成的内容

### 已完成

* SWOD 设计与实现完成
* SWOD 轻量状态机完成
* SWOD issue skip 逻辑完成
* SWOD sysfs 接口完成
* SWOD 多窗口（多 group 并发 held）语义明确
* SWOD 到 WCE 的 held-target 查询接口完成
* WCE 主路径完成：target-first victim selection + stock fallback
* WCE 的 sysfs 开关与 pick/fallback 统计完成
* SFI 辅路径完成：基于 `pend_blks / nr_cmds / oldest_jiffies / hold_segmap` 的窄 fragment-IPU selector
* SFI 的 sysfs 开关、阈值与 skip/pick 统计完成

### 本文件要表达的结论

* 本轮设计到这里已经定稿；
* 当前版本的主路径是 WCE，辅路径是 SFI；
* SSR 不作为本轮设计的主 shaping 路径；
* 真正的 discard materialization 仍由 stock F2FS 完成。

---

## 12. 下次重新开启会话时应当知道什么

阅读本文件后，应当能够立即恢复以下上下文：

1. 当前版本不是“只有 SWOD”，而是 **SWOD + WCE + SFI**；
2. 其中 **WCE 是主路径**，负责围绕 held windows 做 completion；
3. **SFI 是辅路径**，只在 non-target zone 上抑制 tiny fragment 继续形成；
4. 当前没有把 SSR 作为主策略，因为现有代码基线下 `f2fs_need_SSR()` 在 `f2fs_lfs_mode(sbi)` 下直接返回 `false`；
5. 当前版本不直接伪造大 discard，真正的 materialization 仍交给 stock F2FS；
6. 这轮设计本身已经结束，后续若有改动，应视为下一轮演进而非本轮定义的一部分。

---

## 13. 一句话总结

SWOD 与动机二的联合设计，不是让系统“更激进地发出 discard”，而是：

> **先识别并保留局部未来可合并机会，再围绕这些 held windows 定向减少其残余 live blocks，并尽量在其他区域少制造新的 backlog，最终由 stock F2FS 自然 materialize 成更连续的 discard run。**

这就是整个设计的核心闭环。

