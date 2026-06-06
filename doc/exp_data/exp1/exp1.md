#### Experiment 1: Issued Discard Length Distribution

**实验目的。**
本实验用于回答：ReDisc 是否能够改善实际发射到 block layer 的 discard command 形态。Motivation 中的观察表明，F2FS 会产生大量 small discard command，其中一部分 small command 是局部 segment-window 正在快速成熟时的早期片段。如果这些 command 被过早发射，就会错过后续相邻或近邻 follow-up，最终表现为大量短小、碎片化的 issued discard command。

因此，本实验不首先关注前台 workload 性能，而是直接观察 ReDisc 对 issued discard command length distribution 的影响。相比 created discard command，issued discard command 更能反映设备实际看到的 discard 请求形态；如果 ReDisc 有效，实际发射出去的 discard command 应该整体更长、更规整，小长度 command 的比例应下降。

**对比配置。**
本实验使用两组 discard 粒度下的成对对比：

| 配置         | discard_granularity | SWOD/WCE/SFI | 目的                                       |
| ---------- | ------------------: | ------------ | ---------------------------------------- |
| F2FS-16    |           16 blocks | off          | 默认 F2FS baseline                         |
| ReDisc-16  |           16 blocks | on           | 默认粒度下观察 ReDisc 对 issued length 的影响       |
| F2FS-512   |          512 blocks | off          | segment 粒度 F2FS baseline                 |
| ReDisc-512 |          512 blocks | on           | segment 粒度下观察 ReDisc 是否仍能改善 issued shape |

其中，`F2FS-16` 与 `ReDisc-16` 是最关键的对比，用于验证 ReDisc 在默认 F2FS discard 粒度下是否减少过早 small discard 发射。`F2FS-512` 是粗粒度 discard baseline，用于回答“直接把 discard 粒度调大到 segment 是否足够”。但是，`F2FS-512` 的结果不能只通过 CDF 判断，因为它可能通过过滤或推迟小 discard 改变 issued length 分布。因此，`F2FS-512` 需要与后续 total discarded blocks、undiscard blocks 和 pending backlog 指标结合解释。

**Workload。**
本实验使用以下 workload：

| Workload             | 目的                                     |
| -------------------- | -------------------------------------- |
| filebench fileserver | 文件创建、删除、更新混合，容易触发大量 small discard      |
| filebench varmail    | 小文件更新密集，用于观察小粒度更新下的 issued discard 形态  |
| TPCC                 | 事务型更新负载，用于观察应用型 workload 下的 discard 行为 |

如果 TPCC 环境暂时未完成，可以先使用 `fileserver` 和 `varmail` 完成该实验，TPCC 后续补充。

**实验步骤。**
每个 workload、每个配置都从相同初始状态开始，执行如下流程：

```text
1. mkfs.f2fs 重新格式化测试分区；
2. mount 到固定挂载点；
3. 设置当前配置：
   F2FS-16 / ReDisc-16 / F2FS-512 / ReDisc-512；
4. 清空 dmesg、trace buffer 和自定义 counters；
5. 写入实验开始 marker，记录 workload start timestamp；
6. 运行 workload；
7. 写入实验结束 marker，记录 workload end timestamp；
8. 保存 issued discard 日志、F2FS/ReDisc counters 和 workload 输出；
9. sync + umount；
10. 每组实验至少重复 3 次。
```

需要注意，本实验只统计 workload 时间窗口内的 issued discard command，即只统计 start marker 和 end marker 之间的 issue events。实验结束后的 `sync`、`umount` 或清理阶段可能触发额外 discard，这些事件不应混入本实验，否则会影响 issued length distribution 的解释。

**需要收集的数据。**
本实验需要在 discard issue 路径记录实际提交到 block layer 的 command。每条 issued discard command 至少记录：

| 字段          | 说明                                         |
| ----------- | ------------------------------------------ |
| `timestamp` | command issue 时间                           |
| `workload`  | workload 名称                                |
| `config`    | 当前配置                                       |
| `run_id`    | 第几次重复                                      |
| `policy`    | discard policy，例如 BG/FORCE/FSTRIM/UMOUNT 等 |
| `start`     | discard 起始 block                           |
| `len`       | discard command 长度，单位为 F2FS blocks         |
| `segno`     | command 起始位置对应的 segment number             |
| `offset`    | command 在 segment 内的偏移                     |

本实验的核心字段是 `len`。`segno` 和 `offset` 不是画 CDF 必需字段，但建议保留，便于后续分析 command 是否集中在局部 segment-window 中。

**统计方法。**
对每个 workload 和配置，收集所有 workload 时间窗口内的 issued discard command length，并计算 CDF：

```text
CDF(x) = issued command length <= x 的 command 数量 / issued command 总数量
```

横轴为 issued discard command length，单位为 blocks。纵轴为 CDF。为了更清楚展示短 command 和长 command 的差异，横轴可以使用 log scale，或者在图中标注 16、64、128、512 blocks 等关键位置。

除了 CDF 曲线，还需要同时计算以下 summary statistics：

| 指标                   | 说明                                     |
| -------------------- | -------------------------------------- |
| `issued_len_p50`     | issued command length 中位数              |
| `issued_len_p90`     | issued command length P90              |
| `issued_len_p99`     | issued command length P99              |
| `small_issued_ratio` | `len <= 16 blocks` 的 issued command 占比 |
| `avg_blocks_per_cmd` | 每条 issued command 平均覆盖 blocks          |

这些 summary statistics 可以写在正文中，或者作为 Fig. 6 的补充说明。

**生成图片。**
本实验生成：

```text
Fig. 6: Issued discard length CDF
```

推荐画法有两种。

第一种是每个 workload 一个子图，每个子图包含四条曲线：

```text
F2FS-16
ReDisc-16
F2FS-512
ReDisc-512
```

这种方式最直观，可以观察不同 workload 下 issued length distribution 是否一致右移。

第二种是只突出默认粒度对比，把 `F2FS-16` 和 `ReDisc-16` 作为主图，`F2FS-512` 和 `ReDisc-512` 放在补充图或同图较淡曲线中。这样可以避免粗粒度 baseline 干扰主线解释。

**如何解读。**
如果 ReDisc 有效，预期现象是：

```text
1. ReDisc-16 相比 F2FS-16 的 CDF 曲线整体右移；
2. 在 16 blocks 处，ReDisc-16 的 CDF 值低于 F2FS-16；
3. ReDisc-16 的 P50/P90 issued length 增大；
4. avg_blocks_per_cmd 增大。
```

这说明在默认 F2FS discard 粒度下，ReDisc 减少了小而碎的 issued discard command，使更多 discard 以更长、更规整的形式提交给 block layer。

对于 `F2FS-512`，如果它的 CDF 也明显右移，不能直接说明它等价于 ReDisc。因为 `F2FS-512` 可能只是通过粗粒度过滤减少小 command 的发射。因此，`F2FS-512` 必须结合后续 Fig. 7 中的 total discarded blocks、undiscard blocks 和 pending backlog 共同解释。

**本实验能支持的结论。**
本实验能够证明 ReDisc 是否改善 issued discard command 的长度分布。它不直接证明前台性能收益，也不单独证明 SWOD/WCE/SFI 的内部机制。若 ReDisc 的 issued length CDF 右移，说明 ReDisc-full 能够改变设备实际看到的 discard command 形态，减少过早发射的小 command。后续实验将进一步分析 command 数量、small ratio、total discarded blocks，以及 SWOD/WCE/SFI 各自如何产生这一效果。
