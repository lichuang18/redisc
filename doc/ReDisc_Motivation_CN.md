# ReDisc 论文 Motivation 中文草稿（仿 CoDiscard 写法）

> 用途：可作为论文 **Section 3 Motivation and Observation** 的中文基础稿。  
> 写作边界：Introduction 中介绍 discard/TRIM 的重要性；Motivation 中只用少量句子承接已有结论，然后重点用 **stock F2FS 真机 trace** 证明 ReDisc 所关注的问题真实存在。  
> 关键定位：本文面向 **通用块设备 / 普通 SSD / stock F2FS**，不依赖专用 SSD、设备内部监控接口、Open-Channel SSD、ZNS、FEMU 或 FTL 修改。

---

## 3. Motivation and Observation
**已有研究已经表明，discard/TRIM 是连接文件系统失效语义与闪存设备 GC 的重要接口。TRIM 可以帮助设备识别无效逻辑地址，避免在内部 GC 中搬迁无效页，但 TRIM 处理本身也会引入 I/O contention 和额外延迟。因此，discard 策略会同时影响设备端空间回收效率和主机侧 I/O 性能。**

**然而，现有工作多从 device-aware 或 cross-layer 角度讨论 TRIM 是否值得发送，例如基于模拟器或设备反馈估计 TRIM 的收益与开销。与这些工作不同，本文面向通用块设备上的 stock F2FS，不依赖设备内部 GC 状态、FTL 迁移信息或专用接口，而是研究 F2FS 内部 host-visible discard lifecycle 本身：discard demand 如何产生、何时显化、如何合并，以及是否被过早发射。**
### 3.0 为什么 Motivation 里仍然需要提 discard 的重要性

Discard/TRIM 是连接文件系统语义与闪存设备内部管理的重要接口。已有研究已经说明，discard 可以将 host 侧已经失效的逻辑地址通知给设备，使设备在内部 garbage collection 时避免处理部分无效数据；同时，discard 本身也不是免费的，它会引入命令处理开销、I/O contention 和延迟波动。因此，discard 既可能改善设备侧空间管理，也可能干扰前台 I/O。

这一背景说明 discard 值得被研究，但它并不直接决定本文的研究切入点。与依赖设备内部统计、模拟器或专用设备接口来估计 GC/WA 收益的工作不同，本文面向更通用的部署场景：普通块设备和 stock F2FS。在这种场景下，文件系统不应假设设备会暴露内部 GC 状态、真实 NAND 写入量或 FTL relocation 信息。因此，本文不将研究问题建立在设备内部收益模型上，而是从 F2FS 自身可观测、可控制的 discard lifecycle 出发，研究 discard demand 在进入设备之前如何产生、显化、合并和发射。

> 写作建议：这里需要引用已有工作，例如 CoDiscard、iTRIM、LazyTRIM、SeerSSD 或其他 TRIM latency/overhead 论文。引用的目的只是说明 discard 与性能、GC、WA 相关，不需要在 Motivation 中重复做 WA 或 GC relocation 实验。

---

### 3.1 Motivation：F2FS discard 不是孤立的后台命令发射问题

F2FS discard 的关键矛盾并不是系统是否支持 TRIM，也不只是后台 discard thread 是否足够激进，而是 discard demand 在文件系统内部如何形成、何时显化、以及最终以什么形态提交给设备。由于 F2FS 采用 out-of-place update，前台 overwrite 或 delete 会持续产生 invalid blocks。这些 invalid blocks 并不会立即等价于设备可见的 discard command，而是先在 dirty segment、prefree segment 与 checkpoint 相关路径中积累，随后进入 pending discard queue，最后由后台 issue thread 异步提交给设备。

因此，F2FS discard 具有明显的流水线特征：更新路径决定 future discard demand 的数量与空间形态，checkpoint 和 cleaning 决定这些 demand 何时显化，issue thread 只负责消费已经进入队列的 pending discard。现有 F2FS 已经维护 pending discard queue，并提供 `max_discard_request`、`discard_granularity`、`discard_io_aware`、`discard_io_aware_gran`、`discard_urgent_util` 等控制参数。这些机制能够控制一次发射多少 discard、前台 I/O 存在时是否让步、对多小的范围不发、以及高空间利用率下是否进入更激进的 background discard。

然而，这些机制主要工作在 discard 生命周期的下游。它们看到的是已经形成的 pending discard，而不是这些 discard 所在局部空间窗口的短期演化趋势。换言之，现有机制擅长回答“当前队列里的请求该不该发、一次发多少、何时停止”，却不擅长回答“某个局部 segment window 是否还会在短时间内继续积累 discard，因此是否值得暂时保留”。

从这个角度看，stock F2FS 当前的 discard 机制可以被概括为 **current-state-driven issue policy**：它依据当前长度、当前相邻性、当前 I/O 状态和当前空间压力做决策，但缺少对短期 **future coalescing opportunity** 的显式建模。

```text
overwrite / delete
    -> old blocks become invalid
    -> dirty / prefree segments
    -> checkpoint exposes reclaimable ranges
    -> pending discard queue
    -> issue_discard_thread
    -> device DISCARD / TRIM
```

这条链路说明，discard 并不是一个孤立的后台命令提交问题，而是 F2FS 更新、失效、checkpoint、cleaning 和设备通知之间的尾部动作。只优化尾部发射线程，无法覆盖整条链路中的全部优化空间。

---

### 3.2 Motivation Experiment：使用 stock F2FS trace 观察 discard lifecycle

为了验证上述问题是否存在，本文首先在真实设备上对 stock F2FS 进行 trace-driven 分析。实验只加入轻量级 tracepoint，用于记录 block invalidation、pending discard 形成、discard 发射、discard 完成、checkpoint 和 cleaning 等事件；这些 tracepoint 不改变 F2FS 的分配、checkpoint、cleaning、合并或 discard 发射策略。因此，本节所有 observation 都反映 stock F2FS 自身行为，而不是 ReDisc/SWOD/DSU 带来的收益。

实验采用更新密集型 workload，例如 fio random overwrite、Filebench fileserver、Filebench varmail、RocksDB 或 MySQL/TPCC。每组实验先重新格式化并挂载 F2FS，将文件系统预填充到指定利用率，然后运行 workload，同时记录 host-visible discard lifecycle。我们不在该实验中统计设备内部 write amplification，也不要求设备暴露内部 GC 状态；实验目标是回答三个问题：

1. stock F2FS 是否会生成大量短小、碎片化的 discard command？
2. 已经发出的小 discard 是否在短时间内仍会在同一 segment window 中继续出现 follow-up discard？
3. pending discard backlog 是否主要由上游 update path 和 checkpoint/cleaning 显化，而不仅仅由 issue thread 决定？

---

### 3.3 Observation 1：stock F2FS 产生大量小 discard command

我们的 stock F2FS trace 显示，在更新密集型 workload 下，实际发往设备的 discard command 高度集中在小长度区间。例如，在 `[workload]` 下，`[X%]` 的 discard command 小于 `[Y]` blocks，`[P50/P90/P99]` command size 分别为 `[a/b/c]` blocks。这说明真实系统中的 discard 压力并不总是由少量大范围 discard 构成，而经常表现为大量短小、零散的 discard 请求。

这一结果首先排除了一个简单假设：只要让 F2FS 尽快发送 pending discard，就能自然形成足够大的 discard range。事实上，stock F2FS 的 pending queue 虽然支持相邻合并和长度分桶，但它只能合并当前已经形成、已经相邻的请求。如果 discard demand 在时间上分批显化，那么当前看到的小请求可能只是一个尚未成熟的局部空间窗口的早期片段。

同时，discard command 的设备完成延迟并不只与总 discard block 数有关，还与 command 数量、请求粒度、对齐情况、队列竞争和设备实现有关。大量小 discard 会增加命令数量和调度开销，而无界地合并成超大 discard 又可能越过设备 latency knee。因此，ReDisc 的目标不是简单地“越大越好”或“越早越好”，而是需要一种 **bounded、device-aware、segment-window-aware** 的机会判断：在不越过系统压力和设备延迟约束的前提下，识别哪些短小 discard 值得短暂保留。

**Motivation 中建议展示的图：**

- Fig. 2(a): discard command size CDF 或 histogram；
- Fig. 2(b): discard completion latency vs. discard size；
- Fig. 2(c): small discard ratio under different workloads or utilization。
（Fig. 2(c) 的作用不是再证明“有小 discard”，而是证明：大量小 discard 不是某一个 workload、某一个空间利用率下偶然出现的现象，而是 stock F2FS 在多种更新场景下都会遇到的稳定问题）
---

“Fig. 2 整体应该服务于 Observation 1：stock F2FS 产生大量小 discard command，导致 discard stream 碎片化。”
（Fig. 2(a): discard command size distribution
结论：stock F2FS 发出的 discard command 中，小范围请求占比很高。

Fig. 2(b): discard completion latency vs. discard size
结论：discard command 具有不可忽略的设备完成代价，因此 command fragmentation 会影响后台 discard 开销和前台 I/O 干扰。

Fig. 2(c): small discard ratio across workloads/utilizations
结论：小 discard 不是单一 workload 的偶然现象，而是在不同更新负载和空间状态下普遍存在。）

### 3.4 Observation 2：小 discard 具有局部时间/空间相关性，stock F2FS 可能过早发射

更关键的问题是，小 discard 是否完全随机、孤立地产生。如果它们彼此无关，那么保留小 discard 只会增加延迟；如果它们在局部 segment window 中具有短期相关性，那么 stock F2FS 的即时发射就可能过早消费合并机会。

为此，我们对每个已经发出的短小 discard 进行离线回看。设一个 segment window 包含 `W` 个连续 segment；当一个长度不超过 `T` blocks 的 discard 被发出后，我们继续观察其所在 window 在之后 `H ms` 或下一个 checkpoint 前是否出现新的 pending discard。trace 显示，在 `[workload]` 下，`[Z%]` 的已发小 discard 在 `H=[...] ms` 内出现 same-window follow-up，其中 `[M%]` 与已发请求相邻或 gap 小于 `[G]` blocks；同时，issue 时的 window coverage 为 `[C1%]`，`H` 后增长到 `[C2%]`。

这一 observation 表明，stock F2FS 在当前时刻看到的“小请求”，可能只是一个正在成熟的局部 discard window 的早期片段。由于 current-state-driven issue policy 只能基于当前请求长度、当前相邻性和当前 I/O 状态做决策，它无法感知即将形成的相邻关系，因而可能把一个本可形成更规整 discard run 的局部窗口拆成多个小命令发给设备。

因此，本文第一个核心动机不是继续围绕当前长度设计更复杂的排序函数，而是从 segment-window 视角重新审视小 discard 的调度价值。当系统中的主流请求是短小、密集、局部相关的小 discard 时，调度真正需要回答的不是“谁现在最长”，而是“谁最可能在短时间内成长为更规整、受上限约束、与 segment-window 对齐的 discard run”。

**Motivation 中建议报告的指标：**

| 指标 | 定义 | 支撑的结论 |
|---|---|---|
| `same-window follow-up rate` | 小 discard 发出后 `H` 内，同一 segment window 又出现新 discard 的比例 | 小请求不是完全孤立随机出现 |
| `mergeable follow-up rate` | follow-up 与已发请求相邻或 gap 小于 `G` 的比例 | 存在真实合并机会 |
| `window growth ratio` | `coverage(t_issue + H) / coverage(t_issue)` | current-state 策略看不到窗口成熟过程 |
| `premature issue rate` | issue 后 `H` 内同 window coverage 明显增长的比例 | stock issue path 可能过早消费局部机会 |

---

Fig. 3 用来说明 stock F2FS 发出的短小 discard 并不总是孤立请求，而可能是一个正在成熟的局部 segment window 的早期片段。Fig. 3(a) 先展示一个典型 trace：stock F2FS 在某一时刻发出了一个很短的 discard，但在随后的几十毫秒内，同一 segment window 中继续出现多个相邻或近邻的 pending discard range；如果该请求没有立即发出，而是被短暂保留，这些范围本可以合并成一个更规整的 discard run。Fig. 3(b) 进一步从统计上量化这种现象：对所有已发出的小 discard，统计它们在 H ms 内是否出现 same-window follow-up，以及这些 follow-up 是否与已发请求相邻或 gap 很小。这个图说明小 discard 具有局部时间/空间相关性，而不是完全随机出现。Fig. 3(c) 则展示 segment window 的成熟过程：issue 时该 window 的 discard coverage 较低，但经过 H ms 后 coverage 明显增长。这说明 stock F2FS 的 current-state-driven issue policy 只能看到当前的小请求，却看不到短期内即将形成的局部合并机会，因此可能过早发射小 discard，导致本可合并的局部 discard window 被拆成多个设备命令。
（Fig. 3 不是为了证明“后面还会有 discard”，而是为了证明：后续 discard 出现在同一个局部 window 中，并且很多与已发请求可合并，所以 stock F2FS 的即时发射确实可能错失局部合并机会。）
```
为了验证 Observation 2，我会在 stock F2FS 上加入只读 tracepoint，而不改变任何 discard 策略、合并策略、发射策略和 checkpoint/cleaning 行为。trace 的目的不是实现 ReDisc，而是记录 stock F2FS 自己产生和发射 discard 的过程。最少需要记录两类事件：第一类是新的 discard range 进入 F2FS pending discard 结构时，记为 discard_insert；第二类是 F2FS 真正把 discard command 提交到 block layer 或设备时，记为 discard_issue。如果还要分析 discard latency，可以额外记录 block layer 的 block_rq_issue 和 block_rq_complete，筛选其中的 discard request。

具体加 trace 的位置可以放在 fs/f2fs/segment.c 的 discard 路径中。discard_insert 应该加在 F2FS 把失效范围加入 pending discard tree/list 的地方，也就是创建或插入 discard command 的函数附近，例如 add_discard_addrs()、__queue_discard_cmd()、__insert_discard_tree() 或当前内核版本中类似负责生成 pending discard entry 的位置。discard_issue 则加在真正提交 discard bio/request 的地方，例如 __issue_discard_cmd()、__submit_discard_cmd() 或调用 submit_bio()/blkdev_issue_discard() 前后的位置。不同内核版本函数名可能不同，但原则是：insert trace 记录“discard demand 什么时候进入队列”，issue trace 记录“stock F2FS 什么时候把它发出去”。

每个 tracepoint 至少记录这些字段：time_ns、start_blkaddr、len、end_blkaddr、segno、window_id。其中 end_blkaddr = start_blkaddr + len - 1，segno = start_blkaddr / blocks_per_seg，window_id = segno / W。这里的 W 是你定义的 segment window 大小，比如 4 或 8 个连续 segment。这样后处理时就能判断一个已发出的小 discard 后面是否在同一个局部 window 里继续出现新的 discard demand。

实验运行时，每组都使用原生 F2FS。先 mkfs.f2fs，再用原生 mount option 挂载，例如开启 discard 或使用你想分析的 stock discard 配置，然后预填充到目标空间利用率，比如 50%、70%、85% 或 90%。之后运行更新密集型 workload，例如 fio random overwrite、Filebench fileserver/varmail、RocksDB 或 MySQL-TPCC。运行 workload 的同时用 trace-cmd 采集自定义 F2FS tracepoint，例如 f2fs:redisc_discard_insert 和 f2fs:redisc_discard_issue；如果要计算设备侧 discard 完成延迟，再同时采集 block:block_rq_issue 和 block:block_rq_complete。

后处理时先把 trace 按时间排序，然后筛选出所有已经发出的 short discard。short discard 的阈值 T 可以根据 Observation 1 的 size distribution 来定，例如 len <= 8 blocks、len <= 32 blocks 或 len <= 128 blocks。对每个已发出的 short discard D，取它的 t_issue 和 window_id，然后向后看一个有界时间窗口 H，比如 10 ms、50 ms 或 100 ms。如果在 (t_issue, t_issue + H] 内，同一个 window_id 又出现了新的 discard_insert，就说明这个已发小 discard 有 same-window follow-up；如果后续 range 与已发 range 相邻，或者中间 gap 小于设定阈值 G，例如 0、4 或 8 blocks，就说明它有 mergeable follow-up。

最后统计三个核心指标。第一个是 same-window follow-up rate，也就是有同 window 后续 discard 的 short discard 数量占所有 short discard 的比例，用来证明小 discard 不是完全孤立随机出现。第二个是 mergeable follow-up rate，也就是后续 discard 与已发请求相邻或近邻的比例，用来证明确实存在真实合并机会，而不只是同一个 window 里又出现了无关 discard。第三个是 window coverage growth，即比较 issue 时该 window 的 discard 覆盖率和 H ms 后的覆盖率，例如 coverage(t_issue + H) / coverage(t_issue)，用来说明 stock F2FS 发出小 discard 时看到的只是局部窗口的早期状态，而短时间后这个 window 可能会变得更密集、更适合合并。

这组实验最终想证明的是：stock F2FS 发出的小 discard 中，有一部分并不是孤立请求，而是在短时间内仍会出现同 window、相邻或近邻的后续 discard。也就是说，stock F2FS 的 current-state-driven issue policy 只根据当前请求长度、当前相邻性和当前 I/O 状态做决定，可能在局部窗口尚未成熟时就把小 discard 发给设备，从而错失了把多个局部范围合并成更规整 discard run 的机会。
```

### 3.5 Observation 3：仅优化 issue thread 无法抑制 future discard demand 的生成

即使一个更聪明的 issue-side scheduler 能够减少部分过早发射，它仍然只能处理已经进入 pending queue 的请求。对 F2FS 来说，pending discard 的根源在更上游：out-of-place update 每次写入新块时都会使旧块失效，并将这些失效逐步积累到 dirty 或 prefree segment 中，随后在 checkpoint 或 cleaning 阶段批量显化为 queue-visible discard demand。

为了刻画这一过程，我们记录三类 host-visible 量：

1. **更新路径产生的 invalidated blocks**：old-to-new 替换导致的源头失效量；
2. **新进入 pending discard backlog 的 blocks**：queue-visible demand 的增长；
3. **当前尚未消化的 undiscard blocks / pending discard backlog**：后台存量压力。

trace 显示，在 `[workload]` 下，invalidated blocks 随前台更新持续增长，而 newly queued discard blocks 并不是平滑出现，而是在 checkpoint 或 cleaning 附近阶段性抬升。同时，pending discard backlog 在高更新压力下持续堆积。这说明 backlog 不是 issue thread 即时制造的，而是由更新路径持续塑形后在下游集中暴露的结果。

这一观察限定了线程侧优化的作用边界。`max_discard_request`、`discard_granularity`、`discard_io_aware`、`min/mid/max_discard_issue_time` 等参数能够改变的是每轮发多少、多久发一次、前台 I/O 存在时是否让步、以及对多小的范围不发；它们本质上调节的是 backlog 的消费节奏，而不是 backlog 的形成机制。只要更新路径仍持续制造大量分散 invalid blocks，后台 discard 压力就仍会继续积累。

因此，本文第二个核心动机不是继续在 discard 发射线程上做更细的参数调优，而是在普通块设备、stock F2FS 和 host-only 约束下，寻找一种以减少 future discard demand 和 command fragmentation 为直接目标的更新路径策略。换言之，动机二真正要回答的不是“既有 pending discard 该如何排序与等待”，而是“更新落点、失效块聚集方式以及 segment 完整化时机，能否在 backlog 进入队列之前就被主动塑形”。

**Motivation 中建议展示的图：**

- Fig. 4(a): discard demand 在 checkpoint/cleaning 附近批量显化
- Fig. 4(b): pending backlog 是 demand 生成速率和 issue 消费速率之间的差
- Fig. 4(c): invalidation 越分散，未来 discard command 越碎

---

Fig. 4 用来说明，仅优化 discard issue thread 无法从源头抑制 future discard demand。Fig. 4(a) 展示按时间窗口统计的 invalidated blocks、newly queued discard blocks 和 issued discard blocks。结果显示，前台更新持续产生 invalidated blocks，而 queue-visible discard demand 往往在 checkpoint 或 cleaning 附近批量显化。Fig. 4(b) 进一步展示 pending discard backlog 的变化：每当 newly queued demand 超过 issue thread 的消费能力，backlog 就会抬升，说明 issue thread 只是消费已经形成的 pending demand。更关键的是，Fig. 4(c) 从空间维度展示 invalidation 分散程度与 future discard fragmentation 的关系：在 invalidated blocks 数量相近的情况下，触及更多 segment window 的 interval 会产生更多 future discard commands，并且平均 command size 更小。这表明 future discard command 的数量和碎片化程度在进入 issue thread 之前已经被 update path 塑形。因此，仅靠调整 issue thread 的发射速率不能解决 discard demand 的生成问题，需要在更新路径上主动控制 invalid blocks 的聚集方式和 future discard demand 的空间形态。

### 3.6 Research Question and Design Transition

基于上述 observation，本文将 F2FS discard 重新定义为一个跨越消费端和生成端的闭环问题：消费端需要识别并保留短期局部合并机会，生成端需要在 discard pressure 升高时主动减少 future demand 及其破碎度。

**研究问题：**

> 能否在不修改设备接口、不引入新的 on-disk format、不依赖专用设备内部统计的前提下，为 stock F2FS 构建一个面向通用块设备的 discard 优化机制：一方面在 issue 侧识别哪些小 discard 此刻不应过早发出，另一方面在更新路径上主动降低未来要发多少 discard，以及这些 discard 会碎成什么样子？

本文对这个问题的回答是 ReDisc。ReDisc 包含两个互补部分：

- **SWOD：Segment-Window Opportunity-aware Discard**。它在 issue 侧识别具有短期合并潜力的 segment window，并对其中的小 discard 进行有界保留，避免过早发射造成 command fragmentation。
- **DSU：Discard-Demand Shaping Update Path**。它在更新路径侧引入 discard-pressure-aware 的 IPU/SSR 触发条件，从源头减少 future discard demand 及其空间破碎度。

二者共同把 F2FS discard 优化从“下游发射节流”扩展为“机会保留 + 需求塑形”的闭环机制。

---

## 写作注意事项

1. Motivation 中可以引用已有工作说明 discard 会影响 GC、WA 和性能，但不要把自己的贡献写成“降低真实 WA”，除非有可靠设备内部计数。
2. Motivation 不需要重复别人关于 GC/WA 的实验；需要做的是 stock F2FS trace，证明 ReDisc 关注的问题真实存在。
3. 不要写“因为普通 SSD 无法观测 WA，所以转向其他问题”。更好的表述是：本文面向通用块设备，不依赖专用设备内部接口，因此从 host-visible discard lifecycle 出发。
4. 不要写“discard command 越少越好”。应写成：减少 premature、fragmented、low-opportunity 的小 discard，同时保留空间压力下的及时 discard。
5. Motivation 中不要报告 SWOD/DSU 引入后的收益；这属于 Evaluation。
6. “大 discard latency 高”不能直接推出“要合并成更大 discard”。更准确的结论是：需要 bounded、device-aware、segment-aligned 的合并机会判断。



最终动机描述：
动机1: 

为了理解 stock F2FS 中 small discard command 是否只是随机、孤立地产生，我们进一步分析每个已经生成的 small discard command 的后续行为。本文将长度不超过 16 blocks 的 discard command 视为 small command。对于每个 small command，我们在其所在的 segment-window 中向后观察，记录后续 discard command 的出现时间以及与当前 command 的空间距离。该分析使用 trace 中所有 small discard commands，而不是人工挑选的典型样本。

Fig. 3(a) 展示了 small discard command 的 follow-up time CDF。横轴表示当前 small command 生成后，同一局部 window 中下一条 discard command 出现所经历的时间；纵轴表示在给定时间内能够观察到 follow-up 的 small command 比例。该图表明，许多 small discard command 在生成后很短时间内就会出现同一 window 内的后续 discard。这说明 small discard 并不是完全孤立的随机事件，而经常是局部 discard window 形成过程中的早期片段。

仅有时间相关性并不足以说明这些 follow-up 具有合并价值，因此 Fig. 3(b) 进一步展示了 follow-up 的 spatial gap CDF。横轴表示当前 small command 与后续 discard command 之间的空间距离，单位为 blocks；如果两个 range 相邻，则 gap 为 0。该图用于刻画后续 discard 是否出现在当前 command 附近。结果显示，许多 follow-up 与当前 small command 在空间上相邻或距离很近，说明这些后续请求具有真实的合并潜力，而不是出现在完全不相关的位置。

如果使用 Fig. 3(c)，它进一步展示 segment-window 在 small discard 出现后的 coverage growth。横轴表示从该 window 第一次出现 small discard 后经过的时间，纵轴表示该 window 内已经被 discard command 覆盖的 unique blocks 数量。该图说明，一个 small discard command 往往不是该 window 的最终形态；在很短时间内，同一局部 window 中会有更多 blocks 进入 discard demand，使 coverage 快速增长。

因此，Motivation 1 的核心结论是：stock F2FS 看到的许多 small discard command 并不是孤立、无合并价值的小请求，而是具有短期时间相关性和局部空间相关性的 discard window 早期片段。如果 issue thread 仅根据当前 command 长度和当前 pending 状态立即发射，就可能过早消费后续合并机会。由此，本文需要一种 issue-side waiting 机制，在不显著阻塞 discard 进度的前提下，为短小且局部相关的 discard command 保留短暂的成熟时间，从而减少过早发射和 command fragmentation。

动机2: 

为了进一步理解 small discard 的来源，我们以 segment 为单位分析 discard command 的形成过程。我们没有人工挑选特定 segment，而是解析 trace 中所有 redisc_dc_create 记录，并将同一 segment 内时间上连续出现的一组 create 事件定义为一个 segment-window。当该窗口覆盖完整 segment，或同一 segment 超过 5ms 没有新的 create 事件时，认为当前 segment-window 结束。随后，我们对每个 segment-window 统计其最终 coverage ratio、small command 数量以及 disjoint discard run 数量。

Fig. 4(a) 显示，small discard command 并不是主要来自 coverage 很低的随机窗口。相反，大部分 small discard command 来自最终 coverage 位于 50%–80% 的 segment-window。也就是说，这些窗口中已经积累了大量可以 discard 的 blocks，但它们尚未形成一个完整、规整的大范围。Fig. 4(b) 进一步说明，这类窗口的形成过程非常快：从第一条 discard command 出现开始，segment-window 的 coverage 会在很短时间内增长到数百个 blocks。这表明 small discard 并不是长期孤立的小请求，而是局部 discard window 快速成熟过程中的中间状态。

然而，coverage 增长并不意味着 discard range 已经连续。Fig. 4(c) 显示，当一个 segment-window 第一次达到 50% coverage 时，它通常已经包含大量 disjoint discard runs。这意味着虽然该窗口中已有大量 blocks 可以 discard，但这些 blocks 被仍然有效的 live blocks 分隔开。由于 discard command 不能跨越 live block，否则会丢弃有效数据，issue thread 无法安全地把这些 separated runs 合成一个大 command。

因此，Motivation 1 中的 issue-side waiting 只能利用后续到来的相邻 discard 来减少部分过早发射；但对于已经被 live holes 切开的 segment-window，issue thread 无法从源头消除碎片化。要进一步减少 future small discard demand，需要在更新和分配路径中减少这种中等 coverage、高 run_count 的 segment-window 形成，使 discard-heavy window 更少被 live blocks 打断。
