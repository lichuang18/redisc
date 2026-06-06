


## 1. Introduction

NAND flash-based storage 已经广泛应用于服务器、移动设备和嵌入式系统中。相比传统磁盘，闪存设备具有更高的随机访问性能、更低的功耗和更好的体积优势。然而，闪存介质具有 erase-before-write 特性：数据可以按页写入，但擦除通常以更大的块为单位进行。为了向上层提供普通块设备接口，闪存设备内部通常通过 Flash Translation Layer（FTL）维护逻辑地址到物理地址的映射，并在后台执行 garbage collection（GC）。当设备执行 GC 时，victim block 中仍然有效的 pages 需要被迁移到新的位置，随后原 block 才能被擦除。如果设备无法获知哪些逻辑地址已经在文件系统层失效，就可能把这些实际上已经无用的数据仍然当作有效数据迁移，从而增加 write amplification，并影响设备寿命和 I/O 性能。

Discard/TRIM 用于缓解这一 host-device semantic gap。文件系统通过 discard command 通知块设备某些逻辑地址范围已经不再保存有效数据，设备随后可以在 GC 中避免迁移这些数据。因此，discard 对降低 write amplification 和提高长期空间回收效率具有重要作用。然而，discard 本身并不是无成本操作。一个 discard command 仍然需要经过主机 I/O 栈、块层队列和设备端元数据处理路径，并可能与前台读写请求竞争资源。已有研究已经表明，TRIM/discard 的收益和开销之间存在明显 trade-off：过晚发送 discard 会增加设备 GC 中的无效数据迁移，而过于激进地发送 discard 又可能引入 I/O contention 和命令处理开销。

F2FS 是面向闪存设备设计的 log-structured file system。由于采用 out-of-place update，F2FS 在更新数据时会把新数据写入新的位置，并将旧 blocks 标记为 invalid。这种设计有利于顺序写入，但也会持续产生待回收的 invalid blocks。F2FS 已经提供了较为灵活的 discard 控制能力，例如 discard granularity、I/O-aware discard、每轮最大 discard 请求数以及后台 discard issue interval 等。这些机制使 F2FS 能够根据当前 I/O 状态和 discard 队列状态调节 discard 的发射节奏，避免无控制的 discard 干扰前台请求。

然而，F2FS 中 discard command 的形成并不是一个瞬时动作。invalid blocks 会先经过 segment metadata、dirty segment、prefree segment、checkpoint 以及 pending discard queue 等多个阶段，最终才由 issue thread 提交给设备。换言之，issue thread 看到的是已经形成的 pending command，而这些 command 的长度、数量和空间形态在进入 issue thread 之前已经受到上游更新、GC 和 checkpoint 路径的影响。现有 discard 控制机制主要围绕“已经形成的 command 如何发射”做决策，而较少关注这些 command 在形成过程中的短期演化和空间结构。

本文的核心观察是：F2FS 中大量 small discard 并不都应该被视为孤立的小请求。一部分 small discard 是局部 segment-window 正在快速成熟时的早期片段；如果过早发射，可能错过随后到来的相邻或近邻 follow-up，从而损失合并机会。另一方面，仅在 issue-side 等待也不足以解决所有 small discard，因为部分 discard fragmentation 在 pending command 形成之前就已经出现：当一个 segment 中的 discardable blocks 被少量 live blocks 分隔时，即使累计可 discard 的空间已经很大，也只能表现为多个不连续 discard 区间。issue thread 不能跨越 live blocks 合并这些区间，否则会破坏数据正确性。

基于这一观察，本文提出 ReDisc，一种面向 F2FS 的 segment-window-aware discard 处理机制。ReDisc 不替代 F2FS 现有的 checkpoint、prefree、discard materialization 或 issue 路径，而是在其上增加窗口级的机会识别和需求塑形。ReDisc 由三个协同模块组成。第一，SWOD 在 pending discard queue 中识别具有短期合并机会的 segment-window，并对其中的 command 进行有界 hold，以捕获后续 follow-up。第二，WCE 围绕 held windows 优先进行 cleaning，减少窗口中残留的 live blocks，使其更容易自然形成连续 discard 区间。第三，SFI 在 non-target 区域保守地抑制新的 tiny fragments，减少 future small discard 继续形成。

ReDisc 遵循保守的正确性边界：它不跨越 live blocks 发出 discard，不伪造连续 discard range，也不改变 F2FS 原有 discard command 的语义范围。最终哪些 blocks 可以 discard、discard range 如何形成、何时提交到设备，仍由 F2FS 原有路径决定。ReDisc 的作用在于把 discard 处理的观察粒度从单条 command 扩展到 segment-window，使系统既能利用 issue-side 的短期局部合并机会，又能减少上游空间状态导致的 future discard fragmentation。

本文的主要贡献如下：

* 本文指出，F2FS discard 处理不应只从单条 pending command 的当前长度出发理解；局部 segment-window 的短期演化和 live-block 分布同样决定了 discard command 的最终形态。
* 本文揭示了 small discard 的两个来源：一部分来自可以通过 issue-side 有界等待捕获的短期 follow-up，另一部分来自 pending command 形成前已经被 live blocks 切开的空间状态。
* 本文设计并实现 ReDisc，将 SWOD、WCE 和 SFI 结合起来，在不改变 F2FS discard 正确性语义的前提下，同时保留短期合并机会并减少 future tiny fragments。
* 本文在真实 workload 下评估 ReDisc，展示 segment-window-aware discard handling 在减少过早发射和改善 discard range 规整性方面的效果。

## 2. Background

### 2.1 Flash Storage, Garbage Collection, and Discard

NAND flash memory 不能像磁盘一样直接原地覆盖写入。写入通常以 page 为粒度，而擦除以 block 为粒度。为了向主机提供普通块设备接口，闪存设备内部通过 FTL 维护 logical block address 到 physical page 的映射。当主机更新某个逻辑地址时，设备通常将新数据写入新的物理位置，并将旧物理页标记为 invalid。随着写入持续进行，设备内部会积累越来越多 invalid pages，需要通过 device-level garbage collection 回收空间。

在 device-level GC 中，设备选择 victim block，将其中仍然有效的 pages 迁移到其他位置，然后擦除 victim block。如果某些数据在文件系统层已经失效，但设备并不知道这些失效信息，那么设备仍会把它们当作有效页迁移。这些不必要迁移会增加 write amplification，并可能在前台写入路径上引入额外延迟。

Discard，也称 TRIM，是文件系统向设备传递失效信息的接口。文件系统通过 discard command 告诉设备某些 logical block ranges 已经不再包含有效数据。设备接收到这些信息后，可以更新内部映射或有效性状态，使后续 GC 不再迁移这些 pages。这样，discard 能够降低 write amplification，并改善设备空间回收效率。

但是，discard 本身也会产生开销。在主机侧，discard command 需要经过文件系统、块层和 I/O 队列，可能与普通读写请求竞争资源。在设备侧，discard 可能涉及 mapping cache 查找、映射表更新、buffer invalidation 以及元数据写回。不同设备上的 discard 开销会受到命令大小、地址连续性、mapping cache 状态和固件实现的影响。因此，discard handling 需要在两个方向之间平衡：太晚发送 discard 会增加后续 GC 中的无效迁移，太早或太多发送 discard 则可能带来即时 I/O contention 和命令处理开销。

### 2.2 F2FS Space Management and Update Behavior

F2FS 是面向闪存设备设计的 log-structured file system。它将主区域划分为多个 segments，segment 是 F2FS 空间管理的重要基本单位，通常包含 512 个 blocks。F2FS 采用 out-of-place update：当文件数据被更新时，新数据被写入新的空闲位置，旧数据所在的 blocks 被标记为 invalid。这样可以把随机更新转换为更顺序的写入模式，但也会持续产生 invalid blocks。

F2FS 使用多种元数据结构维护空间状态。Segment Information Table（SIT）记录每个 segment 的有效 block 数量和有效性 bitmap；dirty segment list 用于跟踪包含 invalid blocks 的 segments；checkpoint 负责持久化关键元数据并提供一致性恢复点。GC 则用于回收文件系统层面的空闲空间。当空闲空间不足时，foreground GC 会迁移 victim segment 中的有效数据并回收空间；background GC 则在后台准备可回收空间，降低后续前台写入压力。

由于 out-of-place update 和 GC 都会改变 block 的有效性，F2FS 中的 invalid blocks 是 discard demand 的来源。但 invalid blocks 并不会立即变成设备可见的 discard command。它们需要先反映到 segment metadata、dirty/prefree 状态和 checkpoint 相关路径中，然后才可能被 materialize 为 pending discard command。因此，理解 F2FS discard 行为，需要同时关注 invalid blocks 的形成、pending command 的生成，以及最终 issue thread 的发射过程。

### 2.3 F2FS Discard Lifecycle and Control

F2FS 的 discard 处理可以看作一个多阶段 lifecycle。首先，文件更新、删除和 GC 使旧 blocks 失效。其次，这些失效信息通过 SIT、dirty segment、prefree segment 和 checkpoint 等路径被组织和持久化。随后，F2FS 将可 discard 的逻辑地址范围转换为 pending discard command，并插入内部 discard 管理结构。最后，discard issue thread 根据当前策略从 pending queue 中选择 command，并提交到 block layer 和设备。

F2FS 已经提供了多种 discard 控制接口。`discard_granularity` 用于控制过小的 discard 是否被发射；`discard_io_aware` 使 discard thread 能够在前台 I/O 活跃时让步；`max_discard_request` 限制每轮可以提交的 discard 数量；issue interval 则控制后台 discard 的周期和节奏。这些机制构成了 F2FS 对 discard 消费过程的下游控制能力，能够避免 discard command 无限制地干扰前台 I/O。

这些机制的共同特点是：它们主要作用在 pending command 已经形成之后。也就是说，它们能观察当前 command 的长度、当前 pending queue 的压力、以及当前是否存在前台 I/O，但不直接刻画某个局部 segment-window 是否仍在快速成熟，也不改变 future discard demand 进入 pending queue 之前的空间形态。对于本文关注的问题，这一点非常关键：一个 small pending discard command 可能是孤立碎片，也可能是局部窗口的早期片段；一个高度 discardable 的 segment-window 可能因为少量 live blocks 被切成许多不连续 discard 区间。前者需要 issue-side 识别短期合并机会，后者则需要从 pending command 形成之前理解其空间来源。



## 3. Motivation and Observations

Discard 是文件系统向块设备传递失效信息的重要接口。通过 discard，文件系统可以告知设备某些逻辑地址已经不再保存有效数据，从而帮助设备在后续空间回收中避免处理无效数据。然而，discard 并不是无成本操作。一个 discard command 仍然需要经过主机 I/O 栈和设备命令处理路径，并可能与前台读写请求竞争队列资源。因此，discard 的请求数量、粒度和发射时机都会影响后台开销和前台 I/O 干扰。

在 F2FS 中，discard 并不是一个孤立的后台命令提交问题。由于 F2FS 采用 out-of-place update，前台更新、删除和重写会持续使旧 blocks 失效。这些 invalid blocks 不会立即变成设备可见的 discard command，而是先在 dirty segment、prefree segment 和 checkpoint 相关路径中积累，随后被转化为 pending discard command，最后由 issue thread 异步提交给设备。因此，discard command 的形成横跨更新、失效、checkpoint、pending queue 和后台发射多个阶段。issue thread 处理的是已经进入 pending queue 的 command，而这些 command 的数量、长度和空间形态，在进入 issue thread 之前已经受到上游路径的影响。

F2FS 已经提供了较为灵活的 discard 控制接口，例如 `discard_granularity`、`discard_io_aware`、`max_discard_request` 以及 discard issue interval 等。这些机制能够控制小范围 discard 是否发射、前台 I/O 存在时是否让步、每轮最多发多少请求，以及后台 discard 的节奏。换言之，F2FS 已经具备对 discard 消费过程进行调节的能力。然而，这些机制主要基于当前已经形成的 pending command 做出决策，例如当前请求有多大、当前队列中有多少请求、以及当前是否存在前台 I/O。它们能够决定“已经出现的 discard command 如何被发射”，但较少关注这些 command 在形成之前是如何逐步产生的。

因此，我们进一步关注两个问题。首先，一个刚刚进入 pending queue 的 small discard 是否已经具备发射条件，还是会在短时间内继续出现相邻或近邻的 discard，从而使其具备进一步等待和合并的价值。其次，即使能够充分利用这些短期合并机会，small discard 是否仍然会大量存在。由于 F2FS 采用 out-of-place update，invalid blocks 的产生天然受到 live blocks 分布的约束。当一个 segment 中的 discardable blocks 被仍然有效的 live blocks 分隔时，即使累计可 discard 的空间已经很大，也只能表现为多个不连续 discard 区间。这意味着部分 discard fragmentation 并非由发射策略造成，而是在 discard demand 形成过程中就已经产生，并天然限制了后续可合并的空间。为回答这两个问题，我们对 F2FS 的 discard 生命周期进行 trace 分析，并得到以下观察。

### 3.1 Observation 1: F2FS 产生大量小的待发 discard command，且 discard 具有不可忽略的完成代价

我们首先分析 F2FS 中新生成的待发 discard command 的长度分布。在 filebench fileserver workload 下，共观察到 253,108 条新生成的待发 discard command，其中 248,539 条长度不超过 16 blocks，占比约 98.19%。命令长度的中位数仅为 2 blocks，P90 为 8 blocks，P99 也只有 24 blocks。这说明，在 discard command 进入 issue thread 之前，F2FS 内部已经形成了大量短小的 discard ranges。

这一结果说明，pending discard tree/list 虽然能够合并相邻 ranges，但它只能处理当前已经存在且物理地址连续的 discard demand。如果 discard demand 是分批、局部、逐渐显化的，那么某一时刻看到的小 command 可能只是一个尚未成熟的局部窗口的早期片段。此时，如果 issue thread 仅根据当前 command 长度和当前队列状态发射该 command，就可能错过后续出现的相邻或近邻 discard。

除了 command 数量和长度分布，我们还统计了多种 workload 下 completed discard command 的设备完成延迟，包括 filebench、TPCC 和 fio 等测试。结果显示，discard command 具有真实的完成代价，而不是可以忽略的后台操作。即使较短的 discard command，也需要经过设备命令处理路径；当小 command 数量大量增加时，总命令数、队列调度次数以及潜在 I/O 干扰都会增加。因此，F2FS 中大量小 discard command 的存在不是一个纯粹的统计现象，而是一个值得进一步分析的问题。

需要注意的是，这并不意味着 discard command 越大越好。过大的 discard command 也可能引入更长的设备处理时间和更明显的前台干扰。这里的关键问题是：F2FS 会产生大量短小、局部、分批显化的 discard demand，而 discard command 本身又具有不可忽略的完成代价。因此，有必要区分哪些 small command 是缺乏合并机会的独立请求，哪些 small command 只是局部 discard window 的早期片段，后者可能通过短暂保留与后续局部 discard 合并成更规整、受约束的连续 discard 区间。

### 3.2 Observation 2: 小 discard 具有短期时间和空间相关性

进一步的问题是，这些 small discard command 是否完全随机、孤立地产生。如果它们彼此无关，那么等待只会推迟空间回收；如果它们在短时间和近距离内持续出现，那么立即发射就可能过早消耗本可利用的合并机会。

为此，我们对所有长度不超过 16 blocks 的 small pending discard command 进行 follow-up 分析。对于每条 small command，我们在同一 segment-window 内向后查找下一条 discard create 事件，并记录两者之间的时间间隔和空间距离。该分析使用 trace 中所有 small command，而不是人工挑选的典型样本。

Fig. 3(a) 展示 follow-up time CDF。横轴表示当前 small command 生成后，同一 segment-window 中下一条 discard command 出现所经历的时间，纵轴表示在给定时间内观察到 follow-up 的比例。实验中，small command 到下一条同 segment follow-up 的中位时间约为 1.986 μs，P90 约为 2.936 μs。这说明许多 small discard 在生成后极短时间内就能看到同一局部窗口内的后续 discard。换言之，这些小请求并不总是孤立事件，而往往是一个局部 discard window 正在形成过程中的早期片段。

仅有时间相关性还不足以说明这些 follow-up 具有合并价值。因此，Fig. 3(b) 进一步展示 spatial gap CDF。这里的 gap 表示当前 small command 与其 follow-up command 之间的 block 距离；如果两个 range 正好相邻，则 gap 为 0。结果显示，68.56% 的 follow-up 与当前 command 的 gap 不超过 1 block，87.63% 的 gap 不超过 2 blocks，95.49% 的 gap 不超过 4 blocks。这说明 follow-up 不仅来得快，而且通常出现在当前 small command 附近，具有真实的局部合并潜力。

Fig. 3(c) 进一步刻画 segment-window 的 coverage growth。我们将同一 segment 内时间上连续出现的一组 discard create 事件定义为一个 segment-window，并统计从该 window 第一条 command 出现后，window 内累计被 discard command 覆盖的 unique blocks 数量。结果显示，在第一个 discard command 出现时，典型 window 的 coverage 中位数只有 4 blocks；10 μs 后增长到 18 blocks；100 μs 后增长到 155 blocks；1 ms 后增长到 312 blocks。这说明一个 small discard command 往往不是该局部窗口的最终形态，而是一个快速成熟窗口的早期片段。

这些结果表明，F2FS 已有的 discard 控制参数可以调节 pending command 的发射节奏，但局部 discard window 的短期成熟过程仍然值得被显式刻画。当一个 small command 刚刚出现时，它可能看起来只是一个很短的请求；但 trace 显示，同一窗口中的后续 discard 往往很快到来，并且在空间上与当前请求非常接近。因此，issue-side discard 调度如果能够识别这种短期局部相关性，就有机会通过有界等待捕获 nearby follow-up，减少 premature issue 和 command fragmentation。

### 3.3 Observation 3: 仅靠 issue-side waiting 无法消除 future small discard 的根源

Observation 2 表明，一部分 small discard 值得等待，因为它们后续会在同一局部窗口中快速出现 nearby follow-up。然而，issue-side waiting 只能处理已经进入 pending queue 的 command。对于那些在进入 pending path 之前已经被 live blocks 切成多个不连续 discard 区间的 segment-window，issue thread 即使能够等待或调整发射顺序，也无法安全地跨越 live blocks 进行合并。

为了进一步理解 small discard 的来源，我们以 segment-window 为单位分析 discard command 的形成过程。我们解析 trace 中所有 `redisc_dc_create` 记录，并将同一 segment 内时间上连续出现的一组 create 事件定义为一个 segment-window。当该 window 覆盖完整 segment，或同一 segment 超过 5 ms 没有新的 create 事件时，认为当前 window 结束，后续事件进入新的 window。对于每个 segment-window，我们统计其最终 coverage ratio、small command 数量以及不连续 discard 区间数量。该方法使用完整 trace 中所有产生 discard command 的 segment-window，而不是人工挑选若干典型 segment。

Fig. 4(a) 展示 small discard burden 在不同 coverage segment-window 中的分布。coverage ratio 表示一个 segment-window 内被 discard command 覆盖的 unique blocks 数量占整个 segment 的比例。结果显示，small discard 并不是主要来自 coverage 很低的随机窗口。相反，最终 coverage 位于 50%–80% 的 segment-window 贡献了绝大多数 small discard command。也就是说，很多 small discard 并不是因为这些 window 只有少量 blocks 可以 discard，而是因为这些 window 已经积累了相当多的 discardable blocks，却尚未形成一个连续、规整的大范围。

Fig. 4(b) 进一步说明这类 window 的形成过程很快。从第一条 discard command 出现开始，segment-window 的 coverage 会在很短时间内增长到数百个 blocks。这说明 small discard 不是长期孤立的小请求，而是局部 discard window 快速成熟过程中的中间状态。

然而，coverage 增长并不意味着 discard range 已经连续。Fig. 4(c) 统计每个 segment-window 第一次达到某个 coverage threshold 时的不连续 discard 区间数量。这里的不连续 discard 区间数量表示把当前 window 中所有 discard ranges 合并后，仍然剩下多少段不连续区间。结果显示，当一个 segment-window 第一次达到 50% coverage 时，其中位数已经达到几十个。这意味着虽然该 window 中已有大量 blocks 可以 discard，但这些 blocks 被仍然有效的 live blocks 分隔开。

这个现象可以用一个简单例子说明。假设一个 segment 中的 block 状态为：

`D D D L D D L D D D`

其中 `D` 表示已经可以 discard 的 block，`L` 表示仍然有效的 live block。虽然大多数 blocks 都已经可以 discard，但 F2FS 不能把整个范围作为一个 discard command 发出，因为这样会跨过仍然有效的 `L`，导致有效数据被设备丢弃。因此，F2FS 只能生成多个不连续 discard 区间。对于 issue thread 来说，这些 live holes 已经存在；它可以等待、排序或合并已经相邻的 ranges，但不能把 `D L D` 强行合并成一个连续 discard command。

因此，issue-side waiting 可以减少部分过早发射，但不能消除所有 small discard 的来源。F2FS 已经提供了灵活的下游 discard 控制能力，可以调节 pending command 的发射粒度和发射时机；而上述现象进一步说明，部分 discard fragmentation 与 pending command 形成之前的空间状态有关。对于已经被 live holes 切开的 segment-window，issue thread 无法安全地跨越 live blocks 合并 separated ranges。换言之，small discard 的研究空间不仅在于“已有 command 何时发射”，也在于“future discard demand 以何种空间形态进入 pending queue”。


## 4. Design

### 4.1 Overview

Section 3 的观察表明，F2FS 已经具备较灵活的 discard 控制能力，可以通过 `discard_granularity`、`discard_io_aware`、`max_discard_request` 以及 issue interval 等参数调节 pending discard 的消费节奏。然而，这些机制主要基于当前已经形成的 pending command 做出决策。对于一个正在快速成熟的局部 segment-window，单条 command 的当前长度并不能完整表达其后续合并价值；对于已经被 live blocks 切开的 segment-window，仅靠调整发射时机也无法跨越 live blocks 形成连续 discard。因此，ReDisc 将 discard 处理的观察粒度从单条 command 扩展到 segment-window，并围绕“机会保留”和“需求塑形”设计三部分机制。

ReDisc 包含三个模块。第一，SWOD 在 pending discard queue 之上识别具有短期合并机会的 segment-window，并对其中的 command 进行有界 hold，避免过早发射。第二，WCE 围绕被 hold 的窗口优先进行 cleaning，使这些窗口中残留的 live blocks 更快减少，从而帮助窗口自然演化为更连续的 discard range。第三，SFI 在 non-target 区域保守地抑制新的 tiny fragments，减少 future small discard 继续形成。

这三个模块对应 discard lifecycle 的不同阶段。SWOD 位于 pending queue 和 issue thread 之间，处理“已经形成的 command 是否应该立即发射”这一问题；WCE 位于 cleaning 路径中，处理“held window 如何更快成熟”这一问题；SFI 位于更新路径附近，处理“non-target 区域是否继续产生新的细碎 discard demand”这一问题。三者并不替代 F2FS 的 checkpoint、prefree、discard materialization 或 issue 路径，而是在这些路径之上提供窗口级的机会判断和轻量引导。

ReDisc 的基本安全边界是：不跨越 live blocks 发出 discard，不伪造连续 discard range，不改变 pending command 的语义范围。SWOD 只暂缓发射，不主动提交；WCE 只改变 cleaning 的优先候选范围，不重写 F2FS 的 victim 合法性判断；SFI 只在非常窄的条件下影响 non-target fragments，不全局改变更新模式。最终哪些 blocks 可以 discard、discard range 如何 materialize，以及何时提交到设备，仍由 F2FS 原有路径决定。

### 4.2 SWOD: Segment-Window Opportunity-aware Discard

SWOD 解决的问题是：对于已经进入 pending discard queue 的 small discard，哪些不应该立即发射，而应该先保留一小段时间等待局部窗口继续成熟。它不是寻找“当前最值得发出去”的 discard，而是寻找“当前更值得先不发”的 segment-window。

SWOD 的基本单位是 segment-window。一个 segment-window 由若干连续 F2FS segments 组成。使用 segment-window 而不是单条 command 作为判断单位，是因为 Section 3 的 trace 显示，小 discard 往往在同一局部 segment 区域中快速聚集；单条 command 的长度只能反映当前状态，不能反映该局部区域在短时间内的演化趋势。窗口大小由设备允许的最大 discard 范围和 F2FS segment 大小共同决定，并施加实现上限，避免窗口过大带来过高维护成本。

对于每个候选窗口 (r)，SWOD 维护两个核心量。第一个是窗口中已经进入 pending queue 的 discard blocks 数量 (Q(r))，第二个是窗口中仍然有效的 live blocks 数量 (L(r))。设窗口容量为 (C(r))，则定义：

[
qcov(r)=\frac{Q(r)}{C(r)}
]

[
lres(r)=\frac{L(r)}{C(r)}
]

其中，`qcov` 表示该窗口中已经显化为 pending discard 的覆盖比例；`lres` 表示该窗口中仍然阻碍连续 discard 的 live-block residual。高 `qcov` 说明窗口中已经有较多待发 discard demand，低 `lres` 说明该窗口距离形成更连续的 discard range 更近。SWOD 使用这两个量作为轻量的 opportunity proxy，而不是尝试预测复杂的 future invalidation。

当一个窗口满足：

[
qcov(r)\ge T_q
]

[
lres(r)\le T_l
]

SWOD 将其视为具有 hold 价值的窗口。若多个窗口同时满足条件，则优先选择更长的连续窗口；长度相同则选择 `lres` 更低者；仍相同则选择更老的窗口。这个选择规则的含义是：优先保留潜在合并范围更大、距离连续化更近、且已经等待更久的局部窗口。

SWOD 的等待是有界的。每个 held window 都有一个 hold deadline，等待时间被限制在 `H_min` 和 `H_max` 之间。窗口越长、`qcov` 越高、`lres` 越低，说明该窗口的潜在合并价值越高，可以给予稍长的等待时间；但无论窗口状态如何，等待时间都不会超过上限。当窗口自然成熟、等待超时、系统 discard pressure 升高，或进入 urgent/force 等不适合继续等待的状态时，SWOD 释放 held 状态，让相关 command 回到原有 issue 路径。

在 issue path 中，SWOD 的动作非常简单：当 issue thread 准备提交某个 pending command 时，如果该 command 完全落在 held window 内部，则暂缓发射；否则继续走 F2FS 原有 issue 流程。这个判断只影响“是否现在发”，不修改 command 的起始地址、长度或有效性。换言之，SWOD 只 hold，不 submit；只保留机会，不制造新的 discard range。这样可以避免跨越 live blocks 发出不安全的 discard。

SWOD 还向后续模块暴露 held target set。被 hold 的窗口不仅表示“当前不宜立即发射”，也表示“该窗口值得后续 completion 优先关注”。因此，SWOD 是 ReDisc 的机会识别层：它从 pending queue 中识别正在成熟的局部窗口，并为 WCE 继续推进窗口成熟提供目标。

### 4.3 WCE: Window Completion Engine

SWOD 能够保留具有机会的 segment-window，但仅仅等待并不总能让窗口自动变成连续 discard range。许多 held windows 中仍然残留少量 live blocks，这些 live blocks 像隔板一样把 discardable blocks 切成多个不连续区间。如果这些 residual live blocks 长时间不被清理，held window 即使等待也无法自然形成更规整的 discard。因此，WCE 的目标是围绕 held windows 优先进行 completion，使这些窗口更快减少 live residual。

WCE 不重写 F2FS 的 cleaner 成本模型，也不替代原有 victim selection 逻辑。它采用 target-first 的方式工作：当系统中存在 held 或 parked windows，并且当前 GC 模式允许 WCE 生效时，victim selection 首先在这些 target windows 覆盖的 segments 或 sections 中寻找合法 victim。如果 target set 中存在合法 victim，则优先选择它，使 held window 中的 residual live blocks 更快减少；如果 target set 中没有合法 victim，则立即回退到 F2FS 原有 victim selection 路径。

WCE 的逻辑可以概括为：

```text
if WCE is enabled and target windows exist:
    search legal victims inside target windows
    if a legal target victim is found:
        select it
    else:
        fallback to the original victim selection
else:
    use the original victim selection
```

这种设计保持了两个边界。首先，WCE 不改变 victim 的合法性判断。只有 F2FS 原本认为可以被 cleaning 的 segment，才可能被 WCE 选中。其次，WCE 不全局改变 cleaning 策略，而只是优先在 held/parked target 中寻找 victim。若目标窗口不适合清理，系统立即回到原有路径，不会因为 ReDisc 而阻塞 GC 进程。

WCE 与 SWOD 的关系是连续的。SWOD 在 issue-side 发现某个窗口具有短期合并机会后，暂缓发射该窗口内的 pending command；WCE 则在 cleaning 侧尝试减少该窗口内阻碍连续化的 live blocks。这样，held window 不只是被动等待，而是被纳入一个轻量的 completion 流程。当 residual live blocks 减少后，后续 checkpoint、prefree 或 discard queue 更新可以让 F2FS 原有路径自然 materialize 更连续的待发 discard range。

WCE 不直接生成 discard command，也不主动把 held window 转化为大 discard。它只提高 held/parked windows 在 cleaning 中被完成的优先级。最终是否形成更连续的 discard，仍取决于 F2FS 对 block 有效性的判断和原有 discard materialization 路径。这一点保证了 WCE 不会破坏 discard 的安全语义，也避免把设计变成一个新的 GC cost model。

### 4.4 SFI: Selective Fragment IPU

SWOD 和 WCE 主要围绕 held windows 工作：前者保留已有机会，后者推动这些机会成熟。然而，系统中还存在大量 non-target 区域。如果这些区域继续形成新的 tiny fragments，即使 held windows 能够被较好处理，future small discard 仍可能持续进入 pending queue。因此，SFI 作为 ReDisc 的第三个模块，用于在 non-target 区域保守地抑制新的 tiny fragments。

SFI 不是全局 IPU 策略，也不是对 F2FS 更新模式的整体替换。它只在非常窄的条件下生效：当前处于 LFS 场景，系统中存在 held windows，旧块所在 segment 不属于 held target，该 segment 表现为小、碎、老、非热的 fragment，并且当前数据不属于应跳过的 hot data。换言之，SFI 只处理那些不参与当前 held-window completion、但可能继续制造 future small discard 的旧碎片。

SFI 判断 fragment 是否值得处理时，复用 SWOD 已经维护的轻量摘要，包括 segment 内 pending discard blocks 数、pending command 数、最早进入 pending queue 的时间，以及该 segment 是否属于 held target。pending blocks 很少说明该 fragment 规模小；pending command 数较多说明它较碎；age 较老说明它不是刚刚出现的短期噪声；跳过 hot data 则避免对频繁更新的数据产生额外干扰。

SFI 明确采用 non-target only 的策略。held windows 已经由 SWOD 和 WCE 负责保留与 completion；如果 SFI 同时作用于 held target，可能干扰这些窗口的自然演化。因此，SFI 跳过 held target，只在 non-target zone 中对少量 aged tiny fragments 提供窄范围 IPU 例外。这样，SFI 与 WCE 形成互补：WCE 加速目标窗口成熟，SFI 减少其他区域继续产生新的碎片。

SFI 的作用边界同样是保守的。它不直接改变 discard command，不直接生成大 discard，也不全局改变写入落点。它只是对少量满足条件的旧碎片调整更新处理方式，使这些位置不再继续以小而碎的形式进入 future pending discard。通过这种方式，ReDisc 不仅关注“已有 command 何时发射”，也关注“future discard demand 以何种空间形态进入 pending queue”。

整体来看，SWOD、WCE 和 SFI 分别覆盖了 discard 生命周期中的三个关键环节：SWOD 识别并保留 issue-side 的短期合并机会，WCE 推动 held windows 完成，SFI 抑制 non-target 区域新的 tiny fragments。三者共同将 F2FS discard 处理从单条 command 的当前状态扩展到 segment-window 的形成和演化过程，同时保持 discard materialization 与提交路径的原有语义。


## 6. Evaluation

### 6.1 Experimental Setup

**本节目的。**
本节介绍实验环境、对比配置、workload、统计方法和数据来源。这里不放实验结论，只说明实验如何保证公平、可重复，以及每类指标用于回答什么问题。

**正文内容可以这样组织。**

本节首先介绍实验平台，包括 CPU、内存、SSD 型号、设备容量、内核版本、F2FS 版本、挂载参数和 discard 相关默认配置。由于 ReDisc 关注 F2FS 内部 pending discard 的形成、保留、发射和完成过程，实验需要区分三类 discard 事件：新生成的待发 discard command、实际提交到 block layer 的 discard command，以及设备完成的 discard command。三者分别用于分析 ReDisc 对 command 形成路径、issue 路径和完成路径的影响。

实验使用两类对比配置。第一类用于整体 discard 行为和前台性能对比，即在不同 `discard_granularity` 下比较 F2FS 和 ReDisc-full。第二类用于模块分析，即固定基础 F2FS 参数，只逐步开启 SWOD、WCE 和 SFI，从而观察各模块的贡献。

**整体对比配置。**

| 配置         | discard_granularity | SWOD/WCE/SFI | 目的                           |
| ---------- | ------------------: | ------------ | ---------------------------- |
| F2FS-16    |           16 blocks | off          | 默认 F2FS baseline             |
| ReDisc-16  |           16 blocks | on           | 默认粒度下验证 ReDisc-full 效果       |
| F2FS-512   |          512 blocks | off          | segment 粒度 F2FS baseline     |
| ReDisc-512 |          512 blocks | on           | segment 粒度下验证 ReDisc-full 效果 |

`F2FS-512` 用于回答一个自然质疑：如果目标是减少 small discard，是否直接把 `discard_granularity` 调到 segment 粒度即可。该配置是 F2FS 原生粗粒度 discard baseline。`ReDisc-16` 和 `ReDisc-512` 分别用于观察 ReDisc 在默认粒度和 segment 粒度下的增量效果。

**模块分解配置。**

| 配置              | 目的                                         |
| --------------- | ------------------------------------------ |
| F2FS-16         | 默认 baseline                                |
| ReDisc-SWOD     | 只启用 issue-side 有界 hold，验证短期 follow-up 捕获能力 |
| ReDisc-SWOD+WCE | 启用 SWOD 和 WCE，验证 held window completion    |
| ReDisc-full     | 启用 SWOD、WCE、SFI，验证完整设计效果                   |

模块分解实验固定 `discard_granularity=16`，不与 `F2FS-512` 交叉。这样可以避免把“F2FS 原生参数调节”和“ReDisc 模块贡献”混在一起。

**Workload。**

| Workload             | 类型           | 目的                         |
| -------------------- | ------------ | -------------------------- |
| filebench fileserver | 文件创建、删除、更新混合 | 触发大量 small discard，是主要测试负载 |
| filebench varmail    | 小文件更新密集      | 观察小文件场景下 discard 行为和前台延迟   |
| TPCC                 | 事务型更新负载      | 观察应用型 workload 下的吞吐和尾延迟    |

fio 不作为主要 workload。本文关注 F2FS 文件系统路径中 discard command 的形成和发射，因此 filebench 与 TPCC 更贴合本文主题。

**需要收集的基础数据。**

1. workload 层：

   * throughput
   * average latency
   * P95/P99 latency
   * runtime
   * total operations / transactions

2. F2FS discard 路径：

   * created discard commands
   * issued discard commands
   * completed discard commands
   * command 的 `start`, `len`, `segno`, `offset`, `timestamp`
   * issued command length distribution
   * completed discard latency
   * total discarded blocks
   * pending discard command count
   * undiscard blocks

3. F2FS 空间和 GC：

   * free segments
   * dirty segments
   * prefree segments
   * foreground GC count
   * background GC count
   * GC migrated valid blocks
   * checkpoint count
   * checkpoint latency

4. ReDisc 内部统计：

   * held window count
   * skipped command count
   * hold time
   * release reason
   * follow-up captured ratio
   * WCE target pick / fallback
   * SFI trigger / skip reason
   * future small discard count in non-target zones

**本节图表。**

| 表格      | 内容                                           |
| ------- | -------------------------------------------- |
| Table 1 | Experimental platform and F2FS configuration |
| Table 2 | Workloads and parameters                     |
| Table 3 | Compared configurations                      |

---

### 6.2 Discard Behavior Improvement

**本节目的。**
本节首先回答 ReDisc 是否真的改善了 discard command 的形态。相比直接展示前台性能，本节更能体现 ReDisc 的核心贡献：减少小而碎的 issued discard command，提高每条 command 的有效覆盖范围，同时保持 total discarded blocks，避免被误解为只是少发 discard。

**本节核心问题。**

1. ReDisc 是否减少 issued discard command 数量？
2. ReDisc 是否降低 small issued discard command 的比例？
3. ReDisc 是否使 issued discard length 分布右移？
4. ReDisc 是否保持 total discarded blocks，而不是简单抑制 discard？
5. 相比 F2FS-512，ReDisc 是否能以更细粒度的方式改善 discard 形态？

**正文内容可以这样组织。**

本节首先分析 issued discard command 的长度分布。若 ReDisc 能捕获短期局部 follow-up，并避免过早发射小 command，那么 issued discard length CDF 应该整体右移，说明更多 discard 以更长、更规整的形式提交给 block layer。随后统计 issued command 总数、small issued ratio 和 total discarded blocks。该结果用于说明 ReDisc 减少的是 command fragmentation，而不是简单减少 discard 总量。

`F2FS-512` 是重要对照。它可能通过粗粒度过滤减少部分 small discard，但也可能导致 discard coverage 下降或 undiscard blocks 增加。因此，`F2FS-512` 需要和 total discarded blocks、pending backlog、undiscard blocks 等指标一起解释，不能只看 command 数量。

**需要做的实验。**

在每个 workload 下运行以下配置：

```text
F2FS-16
ReDisc-16
F2FS-512
ReDisc-512
```

每组实验重复至少 3 次，取平均值，并给出标准差或 error bar。

**需要收集的数据。**

| 指标                                  | 说明                                     |
| ----------------------------------- | -------------------------------------- |
| `issued_cmds`                       | 实际提交到 block layer 的 discard command 数量 |
| `small_issued_cmds`                 | `len <= 16 blocks` 的 issued command 数  |
| `small_issued_ratio`                | small issued command 占比                |
| `issued_len_p50/p90/p99`            | issued command 长度分布                    |
| `avg_blocks_per_cmd`                | 每条 issued command 平均覆盖 blocks          |
| `total_discarded_blocks`            | 总 discard blocks                       |
| `pending_discard_cmds`              | pending queue 中 command 数量             |
| `undiscard_blks`                    | 尚未 discard 的 blocks                    |
| `completed_discard_latency_p50/p99` | completed discard latency              |

**建议生成的图片。**

**Fig. 6: Issued discard length CDF.**
横轴是 issued discard command length，纵轴是 CDF。重点观察 `ReDisc-16` 相比 `F2FS-16` 是否右移，`ReDisc-512` 相比 `F2FS-512` 是否仍能改善分布。

这张图回答：

```text
ReDisc 是否让实际发射到设备的 discard command 更长、更规整？
```

**Fig. 7: Issued command count, small ratio, and total discarded blocks.**
建议做成三个子图：

* Fig. 7(a): issued command count
* Fig. 7(b): small issued ratio
* Fig. 7(c): total discarded blocks

这张图回答：

```text
ReDisc 是否减少 small issued discard，
同时保持 total discarded blocks，
而不是简单少发 discard？
```

**本节预期结论。**

本节不讨论前台性能收益，而是证明 ReDisc 对 discard 行为本身有效。预期结果是：相比 F2FS，ReDisc 减少 issued command 数量和 small issued ratio，提高平均 issued command 长度，同时 total discarded blocks 基本保持。若 `F2FS-512` 也能减少 small command，则需要进一步检查其 total discarded blocks、undiscard blocks 和 backlog，判断它是否只是通过粗粒度过滤推迟或减少了 discard。

---

### 6.3 Component and Mechanism Analysis

**本节目的。**
本节解释 ReDisc 为什么能改善 discard 行为。模块分解和机制验证放在一起：每个模块不仅要展示对整体指标的贡献，还要用内部统计证明它确实按设计工作。

本节围绕 ReDisc 的设计链条展开：

```text
SWOD 保留 issue-side 短期机会；
WCE 推动 held window completion；
SFI 减少 non-target future tiny fragments。
```

模块分解实验固定 `discard_granularity=16`，使用以下配置：

```text
F2FS-16
ReDisc-SWOD
ReDisc-SWOD+WCE
ReDisc-full
```

#### 6.3.1 SWOD: Capturing short-term follow-up

**要回答的问题。**
SWOD 是否真的捕获了 small discard 后续到来的 follow-up，而不是盲目延迟 discard？

**正文内容。**
SWOD 的目标是避免把正在成熟的 segment-window 过早发射。因此，实验需要证明被 SWOD hold 的 command 在短时间内确实经常出现 follow-up，并且 hold 时间是有界且较短的。进一步，需要观察 held command 最终是否更可能参与形成更长的 issued discard command。

**需要收集的数据。**

| 指标                                     | 说明                                                       |
| -------------------------------------- | -------------------------------------------------------- |
| `held_windows`                         | 被 SWOD hold 的 segment-window 数量                          |
| `held_cmds`                            | 被 hold/skip 的 pending command 数量                         |
| `skip_cnt`                             | issue path 中被 SWOD 暂缓发射的次数                               |
| `hold_time_p50/p90/p99`                | hold 时间分布                                                |
| `followup_captured_ratio`              | hold 期间同 window 出现 follow-up 的比例                         |
| `release_reason`                       | release 原因：matured / timeout / pressure / urgent / force |
| `promotion_ratio`                      | 原本 small 的 held command 最终参与形成更长 issued command 的比例      |
| `held_before_len` / `issued_after_len` | hold 前后 command 长度变化                                     |

**建议生成的图片。**

**Fig. 8(a): SWOD hold time CDF.**
证明 SWOD 大多数等待时间很短，不是长期拖延 discard。

**Fig. 8(b): Release reason breakdown.**
展示 matured、timeout、pressure、urgent、force 等 release 原因。理想结果是 normal/matured release 占主要部分，timeout/pressure 不应过高。

**Fig. 8(c): Held command before/after length.**
比较被 hold 时的 command length 和最终 issued 时的 command length，用于证明 SWOD 能把一部分 small command 推动为更长的 issued discard。

**关键指标定义。**

```text
promotion ratio =
    原本 len <= 16 blocks 的 held command 中，
    最终参与 len > 16 / 64 / 128 blocks issued command 的比例
```

**本小节预期结论。**
SWOD 的 hold 行为不是盲目延迟。大部分 held command 应在短时间内观察到同一局部窗口中的 follow-up，并且一部分 small command 最终参与形成更长的 issued discard。

---

#### 6.3.2 WCE: Completing held windows

**要回答的问题。**
WCE 是否真的让 held windows 更快减少 live residual，而不是随意增加 GC？

**正文内容。**
SWOD 只能保留机会，但如果 held window 中残留 live blocks 长时间不被清理，窗口仍然无法形成更连续的 discard range。WCE 的目标是让 GC 更优先作用于 held/parked windows。实验需要证明 WCE 确实命中这些 target windows，并且 held windows 的 live residual 更快下降，同时 GC 代价没有明显恶化。

**需要收集的数据。**

| 指标                           | 说明                                 |
| ---------------------------- | ---------------------------------- |
| `wce_target_pick_cnt`        | WCE 在 target window 中选中 victim 的次数 |
| `wce_fallback_cnt`           | target 中无合法 victim 后回退次数           |
| `lres_before` / `lres_after` | held window live residual 变化       |
| `qcov_before` / `qcov_after` | pending coverage 变化                |
| `window_completion_time`     | held window 从 hold 到成熟或释放的时间       |
| `matured_window_ratio`       | held windows 最终成熟的比例               |
| `gc_migrated_valid_blocks`   | GC 搬迁的有效 blocks                    |
| `foreground_gc_count`        | 前台 GC 次数                           |

**建议生成的图片。**

**Fig. 9(a): Held window lres reduction.**
横轴是时间或阶段，例如：

```text
hold start
after 100us
after 1ms
release
```

纵轴是 `lres`。对比 `ReDisc-SWOD` 和 `ReDisc-SWOD+WCE`。

**Fig. 9(b): Matured window ratio.**
比较 SWOD-only 和 SWOD+WCE 下 held windows 最终成熟的比例。

**Fig. 9(c): WCE target pick vs fallback.**
展示 WCE 命中 target victim 的次数和 fallback 次数。证明 WCE 不是空转，也不会在找不到目标时阻塞原有 GC。

**Fig. 9(d): GC migration overhead.**
比较 F2FS、SWOD、SWOD+WCE、Full 下 GC migrated valid blocks 或 foreground GC count。证明 WCE 没有显著增加 GC 代价。

**本小节预期结论。**
WCE 应提高 held window 的成熟比例，降低 held window 的 live residual，并且 target pick 具有实际命中率。同时，GC migrated valid blocks 和 foreground GC count 不应明显恶化。

---

#### 6.3.3 SFI: Reducing future tiny fragments

**要回答的问题。**
SFI 是否减少 non-target 区域后续新生成的 small discard，而不是大范围改变更新路径？

**正文内容。**
SFI 是辅助模块，不应单独用来解释整体收益。它应在 SWOD+WCE 的基础上观察效果。实验要证明：SFI 只命中少量 old、tiny、fragmented、non-hot 的 non-target segments，并减少这些区域后续产生 small discard command 的数量。

**需要收集的数据。**

| 指标                             | 说明                               |
| ------------------------------ | -------------------------------- |
| `sfi_pick_cnt`                 | SFI 实际触发次数                       |
| `sfi_skip_target`              | 因属于 held target 跳过次数             |
| `sfi_skip_hot`                 | 因 hot data 跳过次数                  |
| `sfi_skip_age`                 | 因 fragment 不够老跳过次数               |
| `sfi_skip_size`                | 因 fragment 不够 tiny 跳过次数          |
| `non_target_small_create_rate` | non-target 区域 small discard 生成速率 |
| `future_small_created`         | SFI 触发后一段时间内新生成的 small discard 数 |
| `tiny_fragment_segments`       | tiny fragmented segment 数量       |
| `extra_ipu_writes`             | SFI 引入的额外 IPU 写入量                |

**建议生成的图片。**

**Fig. 10(a): Future small discard creation in non-target zones.**
比较 `ReDisc-SWOD+WCE` 和 `ReDisc-full`。纵轴是 non-target 区域新生成的 small discard 数量或生成速率。

**Fig. 10(b): Tiny fragmented segment count over time.**
横轴是时间，纵轴是 tiny fragmented segment 数量。比较是否开启 SFI。

**Fig. 10(c): SFI trigger and skip reason breakdown.**
展示 SFI 命中和跳过原因，证明它是保守触发，不是大范围改变更新路径。

**本小节预期结论。**
SFI 应在不显著增加额外写入的前提下，降低 non-target 区域 future small discard 的生成量。它的贡献主要体现在 created small discard 的减少，而不是立即影响当前 issued command。

---

### 6.4 Foreground Performance Impact

**本节目的。**
在证明 ReDisc 能改善 discard command 形态之后，本节进一步评估这些改动是否会引入前台性能开销。由于 ReDisc 会暂缓部分 pending discard，并在 discard/GC/update 路径中加入窗口级判断，因此需要确认它不会明显降低应用层 throughput 或增加 P99 latency。

本节不是 ReDisc 有效性的核心证明。性能提升可能受到设备内部调度、FTL、OP 空间和设备 GC 等不可控因素影响，因此不应要求 ReDisc 在所有 workload 下都显著提升 throughput。若 ReDisc 与 F2FS 性能接近，应解释为 ReDisc 没有引入明显前台性能退化；若在部分 discard-intensive workload 下观察到 P99 latency 降低，则可以作为额外收益，但其原因需要结合 6.2 和 6.3 的 discard 行为分析解释。

**需要做的实验。**

在每个 workload 下运行以下配置：

```text
F2FS-16
ReDisc-16
F2FS-512
ReDisc-512
```

每组实验重复至少 3 次，取平均值和标准差。

**需要收集的数据。**

| 指标                              | 说明            |
| ------------------------------- | ------------- |
| throughput                      | workload 吞吐   |
| average latency                 | 平均延迟          |
| P95 latency                     | 中高分位延迟        |
| P99 latency                     | 关键尾延迟         |
| runtime                         | workload 运行时间 |
| total operations / transactions | workload 强度确认 |

**建议生成的图片。**

**Fig. 11(a): Normalized throughput.**
横轴是 workload，纵轴是 normalized throughput。对比：

```text
F2FS-16
ReDisc-16
F2FS-512
ReDisc-512
```

**Fig. 11(b): Normalized P99 latency.**
横轴是 workload，纵轴是 normalized P99 latency。越低越好。

**本节预期结论。**
ReDisc-16 相比 F2FS-16 不应出现明显 throughput 下降或 P99 latency 升高；ReDisc-512 相比 F2FS-512 也不应出现明显性能退化。如果不同配置之间差距较小且互有波动，应解释为 ReDisc 的前台性能影响较小。该结果与 6.2 和 6.3 结合，说明 ReDisc 改善 discard 行为并没有以牺牲前台性能为代价。

---

### 6.5 Overhead and Robustness

**本节目的。**
本节回答 ReDisc 是否安全、可控、稳定，尤其回应三个潜在质疑：

```text
1. hold discard 会不会导致 pending backlog 增长？
2. WCE/SFI 会不会增加 GC 或写放大？
3. 参数是否只有某个特定点有效？
```

**正文内容。**
首先分析 ReDisc 对 pending discard backlog、undiscard blocks、free segments 和 foreground GC 的影响。然后分析 CPU/memory overhead。最后做参数敏感性实验，说明 `Hmax`、`Tq`、`Tl` 和 window size 不需要精确调到某个特殊点才能有效。

**需要收集的数据。**

| 指标                              | 说明                              |
| ------------------------------- | ------------------------------- |
| `pending_discard_cmds`          | pending queue 中 command 数量      |
| `undiscard_blks`                | 尚未 discard 的 blocks             |
| `free_segments`                 | 可用 segment 数                    |
| `prefree_segments`              | prefree segment 数               |
| `dirty_segments`                | dirty segment 数                 |
| `foreground_gc_count`           | 前台 GC 次数                        |
| `gc_migrated_valid_blocks`      | GC 搬迁量                          |
| `write_amp_proxy`               | GC migrated blocks / app writes |
| CPU overhead                    | ReDisc 额外 CPU 开销                |
| memory overhead                 | SWOD metadata 占用                |
| `Hmax`, `Tq`, `Tl`, window size | 参数敏感性                           |

**建议生成的图片。**

**Fig. 12(a): Pending backlog over time.**
横轴是时间，纵轴是 pending discard command 数或 undiscard blocks。证明 ReDisc 不会导致 backlog 失控。

**Fig. 12(b): Free segments and foreground GC count.**
展示空间压力指标。证明 ReDisc 不会明显增加 foreground GC。

**Fig. 12(c): Parameter sensitivity of Hmax.**
横轴是 `Hmax`，纵轴可以有两个：

```text
small issued ratio
P99 foreground latency 或 pending backlog
```

用于说明 `Hmax` 太小捕获不到 follow-up，太大可能增加等待代价，中间范围较稳定。

**Fig. 12(d): Sensitivity of Tq/Tl.**
可以用热力图或分组柱状图，展示不同 `Tq/Tl` 下 small issued ratio 和 foreground latency。

**Table 4: CPU and memory overhead.**
展示 ReDisc 的元数据开销、issue-path 判断开销、refresh 开销和整体 CPU overhead。

**本节预期结论。**
ReDisc 的 hold 是有界的，pressure、timeout 或 urgent 状态下可以释放或回退，不会导致 discard backlog 或空间压力失控。WCE 和 SFI 的额外写入与 GC 代价可控。参数在合理范围内均能带来收益，不依赖单个特殊配置点。

---

## 最终图表清单建议

| 图号      | 内容                                                            | 所属章节 | 目的                         |
| ------- | ------------------------------------------------------------- | ---- | -------------------------- |
| Fig. 6  | issued discard length CDF                                     | 6.2  | 展示 issued discard 长度分布是否右移 |
| Fig. 7  | issued count / small ratio / total blocks                     | 6.2  | 证明减少碎片而不是少发 discard        |
| Fig. 8  | SWOD hold time / release reason / before-after length         | 6.3  | 验证 SWOD                    |
| Fig. 9  | lres reduction / matured ratio / WCE pick-fallback            | 6.3  | 验证 WCE                     |
| Fig. 10 | future small creation / SFI skip reason                       | 6.3  | 验证 SFI                     |
| Fig. 11 | throughput / P99 latency                                      | 6.4  | 验证前台性能无明显退化                |
| Fig. 12 | pending backlog / free segments / foreground GC / sensitivity | 6.5  | 安全性和稳定性                    |
| Table 1 | platform                                                      | 6.1  | 实验环境                       |
| Table 2 | workload                                                      | 6.1  | workload 参数                |
| Table 3 | compared configurations                                       | 6.1  | 对比配置                       |
| Table 4 | CPU/memory overhead                                           | 6.5  | 实现开销                       |

---

## 最关键的实验结论链

Evaluation 最终应该形成以下闭环：

1. **Discard Behavior:** ReDisc 减少 issued small discard command，提高 discard command 平均覆盖范围，并保持 total discarded blocks。
2. **SWOD:** 被 hold 的 small command 大多在短时间内出现 follow-up，说明 hold 不是盲目延迟。
3. **WCE:** held window 的 live residual 更快下降，成熟比例提高，说明 completion 路径有效。
4. **SFI:** non-target future small discard 生成减少，说明 ReDisc 不只处理已有 command，也能影响 future demand。
5. **Performance Impact:** ReDisc 不显著降低 foreground throughput 或增加 P99 latency。
6. **Robustness:** pending backlog、foreground GC、CPU/memory overhead 和参数敏感性均可控，说明设计安全稳定。


## Related Work

### TRIM/discard 的收益、开销与发送策略

TRIM/discard 的基本作用是向底层设备传递文件系统层的失效信息，使设备在 garbage collection 中避免迁移已经无效的数据，从而降低 write amplification。早期工作主要从设备内部空间管理和写放大的角度分析 discard 的收益。Hu 等人对 flash-based SSD 中的 write amplification 进行建模，说明 over-provisioning、GC policy 和 workload pattern 都会影响设备内部写放大。Frankie 等人进一步分析 TRIM command 对 effective over-provisioning 和 write amplification 的影响，说明 TRIM 可以通过释放逻辑无效空间改变设备可见的有效空闲比例。Desnoyers 和 Dayan 等人的工作也从 SSD write performance、log-structured cleaning 和 write amplification management 的角度说明，设备内部 GC 开销与空间余量、数据冷热和 workload 变化密切相关。

这些研究说明 discard 对设备长期性能和寿命具有重要价值，但也引出了另一个问题：discard 并不是越早、越多发送越好。FlashVM 讨论了 flash-aware memory management 中的 discard batching，以避免额外开销。CoDiscard 进一步将 discard 调度建模为收益与代价之间的选择，指出完全禁止 TRIM 会增加 write amplification，而无控制地发送 TRIM 又会引入 TRIM overhead。它通过 revenue model 估计 TRIM command 带来的 write amplification reduction 和对应 overhead，从而选择 high performance-price-ratio 的 command 发送。

另一类工作直接关注文件系统层 discard 的发射时机。Lazy TRIM 针对 Ext4 中 TRIM command 引起的 journaling overhead，通过延迟和合并 TRIM 操作降低日志路径中的开销。iTRIM 面向 F2FS-based mobile devices，指出 TRIM 虽然能减少 device-level GC migration，但也可能与 user I/O 产生 contention，尤其在 F2FS checkpoint 期间 accumulated TRIM commands 会显著增加前台延迟。为此，iTRIM 将 TRIM 处理转移到 system idle time，并根据 TRIM size 和 logical address pattern 选择更合适的 TRIM unit。2024 年的 Delayed TRIM 则从设备侧拆分 TRIM 处理过程：设备收到 TRIM request 后先记录 trim buffer 和 validity bitmap，而将 write buffer、L2P 和 P2L table 中的高开销 invalidation 操作延迟到 device idle time。类似地，2024 年关于 F2FS DISCARD management 的工作提出 EPD 以利用短 idle time 处理 DISCARD，并提出 PSA segment allocation scheme，用 overwrite command 替代部分 DISCARD，以改善 WAF 和 throughput。

上述工作共同说明，discard 需要在长期收益和即时开销之间取得平衡。它们主要回答的是“discard 是否值得发送”“何时发送 discard”“每次发送多大的 discard”以及“设备何时处理 discard”。ReDisc 与这些工作互补。ReDisc 关注的不是单条 command 的收益估计，也不是简单把 discard 推迟到 idle time，而是 F2FS 内部 small pending discard 的形成与演化：一个 small discard 可能是孤立碎片，也可能是正在快速成熟的 segment-window 的早期片段。因此，ReDisc 在 issue-side 识别具有短期 follow-up 的 segment-window，并通过有界 hold 保留其局部合并机会。

### 基于 discard 的空间回收与 log-structured 系统

除了文件系统和设备侧的 TRIM 调度，近年的工作也开始将 discard 纳入更高层的 log-structured storage garbage collection。DisCoGC 是这类工作的代表。它面向 ByteDance 的 ByteStore/ByteDrive 分布式 append-only 存储系统，指出原有 compaction-based GC 会在降低 space amplification 的同时带来额外数据搬移和 SSD wear，导致 write amplification 与 space amplification 之间存在显著 trade-off。DisCoGC 将 discard 与 compaction 结合，通过 discard reclaim stale data，避免移动已经无效的数据；同时通过 batch、parallelism/IOPS control、trim filter 和 trim merger 控制 discard 开销。trim filter 优先处理大范围 trim，trim merger 将 LBA 相邻的小范围合并成更大的 trim command，从而更好利用 SSD 的 limited trim IOPS。

DisCoGC 与 ReDisc 在思想上有一个共同点：二者都认识到 discard 的粒度和连续性非常重要，小而碎的 discard 会降低设备侧处理效率，并且需要通过过滤、合并或调度降低其数量。然而，两者作用的系统层次和问题边界不同。DisCoGC 工作在分布式 append-only storage 的 LogFile/chunk 层，目标是在 compaction 与 discard 之间重新平衡 space amplification 和 write amplification。它面向的是高层存储系统中 stale ranges 的回收，并通过系统级 compaction、discard batching 和 trim IOPS control 处理大范围垃圾数据。

ReDisc 工作在 F2FS 内部，面对的是 pending discard command 在进入设备之前的形成过程。它不控制分布式存储中的 LogFile compaction，也不依赖高层对象或 chunk 的删除语义。ReDisc 的基本单位是 F2FS segment-window，关注的是一个局部窗口中 pending discard coverage、live residual 和 follow-up 到达情况。换言之，DisCoGC 证明了在 log-structured storage 中引入 discard 可以减少 compaction 搬移，而 ReDisc 进一步关注在文件系统内部如何减少 small discard 的过早发射和 future fragmentation。

这一类工作对 ReDisc 的启发是明确的：discard 更适合处理连续、足够大的无效范围，而碎片化范围需要被过滤、合并或通过上游行为减少。不同的是，ReDisc 不在高层执行 compaction，也不直接把小范围强行合并成跨越 live blocks 的 discard。它通过 SWOD 保留正在成熟的局部窗口，通过 WCE 减少 held window 中的 residual live blocks，通过 SFI 抑制 non-target 区域新的 tiny fragments，使 F2FS 原有路径更有机会自然形成连续 discard 区间。

### 语义增强、碎片感知与 ReDisc 的定位

还有一些工作关注 host-device semantic gap 或存储系统中的碎片化问题。iDiscard 试图增强文件系统和 flash device 之间的 discard 语义，使设备获得更完整的失效信息，减少因逻辑失效但设备未知而造成的不必要迁移。Open-channel SSD、ZNS SSD 和 host-managed storage 相关工作则通过让 host 参与更多数据放置和回收决策，减少设备内部不透明 GC 带来的 write amplification。近年来，TRIM/invalidation 语义也被扩展到新的存储形态，例如 Revisiting Trim for CXL Memory 讨论了在 CXL memory expansion 场景中引入 trim 语义的必要性。

碎片化相关研究也与本文有关。Filesystem Fragmentation on Modern Storage Systems 系统性分析了现代存储系统中的 file fragmentation 及其对 I/O path 的影响。ZoneTrace 则为 F2FS on ZNS SSDs 提供运行时 zone monitoring 和空间状态可视化工具，帮助观察 F2FS 在 zoned storage 上的 segment/zone 使用情况。这些工作说明，文件系统内部的空间布局和碎片状态会影响底层存储行为；但它们主要关注文件/zone 层面的碎片观测或 ZNS 场景，并不直接解决 F2FS discard command 在 pending queue 中的形成与发射问题。

ReDisc 的定位与这些工作不同。它不扩展设备接口，不要求设备暴露 FTL、GC 或 mapping 状态，也不依赖 open-channel/ZNS/CXL 这类特殊硬件语义。ReDisc 仍然工作在普通 block device 上的 F2FS discard 路径之内，利用的是 F2FS 内部已经可见的信息：pending discard coverage、live residual、segment-window 的短期演化，以及 non-target fragments 的形成。通过将 discard 处理粒度从单条 command 提升到 segment-window，ReDisc 同时覆盖 issue-side 的短期机会和 source-side 的 future demand shaping。

总体来看，已有研究已经证明 discard 既有收益也有开销，并从收益建模、idle-time 调度、设备侧延迟处理、compaction-discard 协同和语义增强等角度提出了不同方案。ReDisc 关注的是一个更细粒度的问题：F2FS 中 small discard command 在进入设备之前如何形成、是否仍在短期演化、以及是否已经被 live blocks 切成多个不连续区间。它并不替代已有 discard 调度思想，而是在 F2FS 内部补充 segment-window 级别的机会识别和需求塑形机制。




## 参考文献

[1] Changman Lee, Dongho Sim, Jooyoung Hwang, and Sangyeun Cho. “F2FS: A New File System for Flash Storage.” USENIX FAST, 2015.

[2] Yu Liang, Cheng Ji, Chenchen Fu, Rachata Ausavarungnirun, Qiao Li, Riwei Pan, Siyu Chen, Liang Shi, Tei-Wei Kuo, and Chun Jason Xue. “iTRIM: I/O-Aware TRIM for Improving User Experience on Mobile Devices.” IEEE Transactions on Computer-Aided Design of Integrated Circuits and Systems, 40(9):1782–1795, 2021.

[3] X. Feng, X. Chen, R. Li, J. Li, C. Song, D. Liu, Y. Tan, and L. Qiao. “CoDiscard: A Revenue Model Based Cross-layer Cooperative Discarding Mechanism for Flash Memory Devices.” Journal of Systems Architecture, 128:102564, 2022.

[4] Runhua Bian, Liqiang Zhang, Jinxin Liu, Jiacheng Zhang, Jianong Zhong, Jiahao Gu, Hao Guo, Zhihong Guo, Yunhao Li, Fenghao Zhang, Jiangkun Zhao, Yangming Chen, Guojun Li, Ruwen Fan, Haijia Shen, Chengyu Dong, Yao Wang, Rui Shi, Jiwu Shu, and Youyou Lu. “Discard-Based Garbage Collection for Distributed Log-Structured Storage Systems in ByteDance.” USENIX FAST, 2026.

[5] Jungheon Kim and Dongkun Shin. “Delayed TRIM for Reducing Trim Overhead.” IEEE NVMSA, 2024.

[6] Jinwoong Kim, Donghyun Kang, and Young Ik Eom. “Managing DISCARD Commands in F2FS File System for Improving Lifespan and Performance of SSD Devices.” Journal of KIISE, 51(8):669–677, 2024.

[7] D. H. Kang and Y. I. Eom. “iDiscard: Enhanced Discard() Scheme for Flash Storage Devices.” IEEE BigComp, pp. 360–366, 2018.

[8] K. Lee, D. H. Kang, D. Jeong, and Y. I. Eom. “Lazy TRIM: Optimizing the Journaling Overhead Caused by TRIM Commands on Ext4 File System.” IEEE ICCE, 2018.

[9] C. Hyun, D. Shin, and N. Chang. “To TRIM or Not to TRIM: Judicious TRIMing for Solid State Drives.” SOSP Poster, 2011.

[10] B. Kim, D. Kang, C. Min, and Y. I. Eom. “Understanding Implications of Trim, Discard, and Background Command for eMMC Storage Device.” IEEE GCCE, 2014.

[11] T. Frankie, G. Hughes, and K. Kreutz-Delgado. “Analysis of Trim Commands on Overprovisioning and Write Amplification in Solid State Drives.” ACM Southeast Regional Conference, 2012.

[12] T. Frankie, G. Hughes, and K. Kreutz-Delgado. “A Mathematical Model of the Trim Command in NAND-flash SSDs.” ACMSE, 2012.

B. SSD write amplification、GC、log-structured storage 基础

[13] X.-Y. Hu, E. Eleftheriou, R. Haas, I. Iliadis, and R. Pletka. “Write Amplification Analysis in Flash-based Solid State Drives.” SYSTOR, 2009.

[14] Philippe Desnoyers. “Analytic Modeling of SSD Write Performance.” SYSTOR, 2012.

[15] Philippe Desnoyers. “Analytic Models of SSD Write Performance.” ACM Transactions on Storage, 10(2), 2014.

[16] Niv Dayan, Luc Bouganim, and Philippe Bonnet. “Modelling and Managing SSD Write-amplification.” ACM SIGMOD, 2016.

[17] Mohammadreza Ghodsi, Vasileios Tsironis, Alexander Fedorov, and Haris Volos. “Kreon: An Efficient Memory-Mapped Key-Value Store for Flash Storage.” USENIX ATC, 2022.
注：如果正文不讨论 KV/LSM，可不引用。

[18] Chen Shen, Youyou Lu, Fei Li, Weidong Liu, and Jiwu Shu. “NovKV: Efficient Garbage Collection for Key-Value Separated LSM-Stores.” IEEE MSST, 2020.

[19] Jing Wang, Youyou Lu, Qing Wang, Minhui Xie, Keji Huang, and Jiwu Shu. “Pacman: An Efficient Compaction Approach for Log-Structured Key-Value Store on Persistent Memory.” USENIX ATC, 2022.

[20] Youyou Lu, Jiwu Shu, and Weimin Zheng. “Extending the Lifetime of Flash-based Storage through Reducing Write Amplification from File Systems.” USENIX FAST, 2013.

[21] Jiacheng Zhang, Jiwu Shu, and Youyou Lu. “ParaFS: A Log-Structured File System to Exploit the Internal Parallelism of Flash Devices.” USENIX ATC, 2016.

[22] Youyou Lu, Jiacheng Zhang, Zhe Yang, Liyang Pan, and Jiwu Shu. “OCStore: Accelerating Distributed Object Storage with Open-Channel SSDs.” IEEE ICDCS, 2019.

C. 近年扩展相关：fragmentation、ZNS、CXL trim

[23] Jonggyu Park and Young Ik Eom. “Filesystem Fragmentation on Modern Storage Systems.” ACM Transactions on Computer Systems, 41(1–4), Article 3, 2023.

[24] Ping-Xiang Chen, Donghyun Seo, Chia-Yu Sung, Jonggyu Park, Matias Bjørling, and Nikil Dutt. “ZoneTrace: Zone Monitoring Tool for F2FS on ZNS SSDs.” ACM Transactions on Design Automation of Electronic Systems, 2024.

[25] Hayan Lee, Jungwoo Kim, Wookyung Lee, Juhyung Park, Sanghyuk Jung, Jinki Han, Bryan S. Kim, Sungjin Lee, and Eunji Lee. “Revisiting Trim for CXL Memory.” ACM HotStorage, 2025.

[26] J. Ma, L. Tang, Y. Li, T. He, A. Zhai, and J. Xue. “Filesystem Fragmentation on Modern Storage Systems.”