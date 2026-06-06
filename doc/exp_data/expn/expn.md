## Experiment 1: End-to-End Performance

### 实验目的

本实验用于回答：ReDisc 在改善 discard command 形态的同时，是否会影响前台 workload 性能。由于 ReDisc 会对部分 pending discard command 进行有界 hold，并通过 WCE/SFI 改变 future discard demand 的形成过程，因此首先需要证明它不会以牺牲前台吞吐或尾延迟为代价。

本实验只评估完整系统的端到端效果，不分析 SWOD、WCE 和 SFI 的单独贡献。模块分解和机制验证放在后续实验中完成。也就是说，本实验关注的是：当 ReDisc-full 作为一个整体开启后，前台应用是否仍能保持稳定性能，以及相比 F2FS 原有 discard 配置是否具有收益。

同时，本实验还需要回答一个可能的质疑：ReDisc 的效果是否只是来自 discard 粒度设置，而不是来自 segment-window-aware 机制本身。F2FS 默认 `discard_granularity=16` blocks，而它也可以设置为 segment 粒度，即 `discard_granularity=512` blocks。因此，本实验采用两组 discard 粒度，并在每组粒度下分别比较 F2FS 和 ReDisc-full。这样可以同时观察两件事：第一，在默认粒度下 ReDisc 是否有效；第二，在 segment 粒度下 ReDisc 是否仍然不会带来额外性能损害，并且是否比单纯依赖粗粒度 discard 更稳。

### 对比配置

本实验使用 4 个配置，按两组 discard 粒度成对比较。

| 配置         | discard_granularity | SWOD/WCE/SFI | 目的                              |
| ---------- | ------------------: | ------------ | ------------------------------- |
| F2FS-16    |           16 blocks | off          | 默认 F2FS baseline                |
| ReDisc-16  |           16 blocks | on           | 默认 discard 粒度下验证 ReDisc-full 效果 |
| F2FS-512   |          512 blocks | off          | segment 粒度 F2FS baseline        |
| ReDisc-512 |          512 blocks | on           | segment 粒度下验证 ReDisc-full 效果    |

其中：

```text
F2FS-16:
    discard_granularity = 16
    gc_urgent = 0
    SWOD/WCE/SFI = off

ReDisc-16:
    discard_granularity = 16
    gc_urgent = 0
    SWOD/WCE/SFI = on

F2FS-512:
    discard_granularity = 512
    gc_urgent = 0
    SWOD/WCE/SFI = off

ReDisc-512:
    discard_granularity = 512
    gc_urgent = 0
    SWOD/WCE/SFI = on
```

这里不单独设置 `ReDisc-off`。在当前实现中，当 SWOD、WCE 和 SFI 全部关闭时，修改后的代码行为就是 F2FS，因此关闭 ReDisc 机制的配置直接记为 F2FS baseline 即可。

本实验也不使用 `gc_urgent` 作为 baseline。`gc_urgent` 属于更激进的紧急处理状态，更适合放在后续 pressure/robustness 实验中验证 ReDisc 是否能及时 release 或 bypass，而不是作为常规调优配置。

### Workload 选择

本实验只使用 3 个 workload。

| Workload             | 类型           | 目的                         |
| -------------------- | ------------ | -------------------------- |
| filebench fileserver | 文件创建、删除、更新混合 | 触发大量 small discard，是主要测试负载 |
| filebench varmail    | 小文件更新密集      | 观察小文件场景下的吞吐和尾延迟            |
| TPCC                 | 事务型更新负载      | 观察上层应用型 workload 的性能影响     |

本实验不使用 fio。fio 更适合块级压力测试，而本实验关注 F2FS 文件系统路径下 discard command 的形成及其对前台 workload 的影响，因此保留 filebench 和 TPCC 更贴合本文主题。

### 实验流程

每个 workload、每个配置都从相同初始状态开始。

```bash
1. 如设备允许，先清理测试分区状态
   blkdiscard /dev/xxx    # 可选

2. 重新格式化测试分区
   mkfs.f2fs -f /dev/xxx

3. 挂载 F2FS
   mount -t f2fs /dev/xxx /mnt/test

4. 设置统一 mount option
   除 discard_granularity 和 ReDisc 开关外，其余参数保持一致。

5. 预填充文件系统到固定使用率
   例如 70% 或 80%。
   所有配置使用相同预填充方式和相同数据规模。

6. sync，并可选 remount
   确保 workload 开始前文件系统状态一致。

7. 设置当前配置
   F2FS-16 / ReDisc-16 / F2FS-512 / ReDisc-512

8. 清空实验统计
   dmesg -C
   清空 ftrace/bpftrace buffer
   清空 F2FS/ReDisc counters
   记录实验开始时间戳

9. 运行 workload
   fileserver / varmail / TPCC 每次运行固定时间。
   建议每次 10–30 min，所有配置保持一致。

10. 保存 workload 输出
    保存 throughput、average latency、P95/P99 latency、runtime。

11. 保存 F2FS/ReDisc 汇总统计
    保存 discard 统计、GC 统计、空间状态统计。

12. sync + umount
    sync
    umount /mnt/test

13. 每组实验至少重复 3 次
    取平均值，并记录标准差或 error bar。
```

### 配置设置示例

假设 sysfs 路径为：

```bash
/sys/fs/f2fs/<dev>/
```

配置示例：

```bash
# F2FS-16
echo 16 > /sys/fs/f2fs/<dev>/discard_granularity
echo 0  > /sys/fs/f2fs/<dev>/gc_urgent
echo 0  > /sys/fs/f2fs/<dev>/swod_enable
echo 0  > /sys/fs/f2fs/<dev>/swod_completion_enable
echo 0  > /sys/fs/f2fs/<dev>/swod_frag_ipu_enable

# ReDisc-16
echo 16 > /sys/fs/f2fs/<dev>/discard_granularity
echo 0  > /sys/fs/f2fs/<dev>/gc_urgent
echo 1  > /sys/fs/f2fs/<dev>/swod_enable
echo 1  > /sys/fs/f2fs/<dev>/swod_completion_enable
echo 1  > /sys/fs/f2fs/<dev>/swod_frag_ipu_enable

# F2FS-512
echo 512 > /sys/fs/f2fs/<dev>/discard_granularity
echo 0   > /sys/fs/f2fs/<dev>/gc_urgent
echo 0   > /sys/fs/f2fs/<dev>/swod_enable
echo 0   > /sys/fs/f2fs/<dev>/swod_completion_enable
echo 0   > /sys/fs/f2fs/<dev>/swod_frag_ipu_enable

# ReDisc-512
echo 512 > /sys/fs/f2fs/<dev>/discard_granularity
echo 0   > /sys/fs/f2fs/<dev>/gc_urgent
echo 1   > /sys/fs/f2fs/<dev>/swod_enable
echo 1   > /sys/fs/f2fs/<dev>/swod_completion_enable
echo 1   > /sys/fs/f2fs/<dev>/swod_frag_ipu_enable
```

如果实际 sysfs 名称不同，应在实验脚本中统一封装配置函数，避免手工设置错误。

### 重要注意事项

本实验是端到端性能实验，不应开启逐条 discard command 的 `pr_info` 打印。大量 dmesg 输出会干扰 workload 性能，导致 throughput 和 tail latency 结果不可信。

本实验只使用轻量 counter、tracepoint 或汇总统计。逐条 discard create/issue/complete 日志应放到后续机制分析实验中单独收集，不要与端到端性能实验混在一起。

### 需要收集的 workload 指标

| 指标                              | 来源                  | 说明               |
| ------------------------------- | ------------------- | ---------------- |
| throughput                      | filebench / TPCC 输出 | 衡量整体前台性能         |
| average latency                 | workload 输出         | 平均延迟，仅辅助         |
| P95 latency                     | workload 输出或 trace  | 中高分位延迟           |
| P99 latency                     | workload 输出或 trace  | 关键尾延迟指标          |
| runtime                         | workload 输出         | 确保运行时长一致         |
| total operations / transactions | workload 输出         | 确认 workload 强度一致 |

画图时建议统一归一化到 `F2FS-16`：

```text
normalized throughput =
    当前配置 throughput / F2FS-16 throughput

normalized P99 latency =
    当前配置 P99 latency / F2FS-16 P99 latency
```

这样可以同时比较默认 F2FS、segment 粒度 F2FS，以及两种粒度下 ReDisc 的整体表现。如果需要突出 ReDisc 在相同粒度下的增量效果，可以在正文中补充说明：

```text
ReDisc-16 should be compared with F2FS-16,
and ReDisc-512 should be compared with F2FS-512.
```

### 需要同步收集的 discard 指标

虽然 Fig. 6 主要展示前台性能，但仍需保存 discard 汇总指标，用于解释性能变化，并为后续实验复用。

| 指标                                | 说明                                     |
| --------------------------------- | -------------------------------------- |
| created discard commands          | 新生成的待发 discard command 数量              |
| issued discard commands           | 实际提交到 block layer 的 discard command 数量 |
| completed discard commands        | 设备完成的 discard command 数量               |
| small issued commands             | `len <= 16 blocks` 的 issued command 数量 |
| total discarded blocks            | 总 discard blocks                       |
| issued length P50/P90/P99         | issued command 长度分布                    |
| completed discard latency P50/P99 | discard 完成延迟                           |
| pending discard commands          | pending queue 中剩余 command 数量           |
| undiscard blocks                  | 尚未 discard 的 blocks                    |

这些指标不一定放在 Fig. 6 中，但必须保存。后续 Fig. 7/Fig. 8 会用它们证明 ReDisc 改善的是 discard command 形态，而不是简单减少 discard 总量。

### 需要同步收集的空间和 GC 指标

| 指标                       | 说明                |
| ------------------------ | ----------------- |
| free segments            | 可用 segment 数      |
| dirty segments           | dirty segment 数   |
| prefree segments         | prefree segment 数 |
| foreground GC count      | 前台 GC 次数          |
| background GC count      | 后台 GC 次数          |
| GC migrated valid blocks | GC 搬迁的有效 blocks   |
| checkpoint count         | checkpoint 次数     |
| checkpoint latency       | checkpoint 延迟     |

这些指标用于确认 ReDisc 没有造成明显空间压力、foreground GC 增加或 checkpoint 异常。

### 生成图片

本实验生成 Fig. 6，包含两个子图。

#### Fig. 6(a): Normalized throughput

横轴：

```text
fileserver, varmail, TPCC
```

每个 workload 下画 4 根柱子：

```text
F2FS-16
ReDisc-16
F2FS-512
ReDisc-512
```

纵轴：

```text
Normalized throughput
```

`F2FS-16 = 1.0`。

#### Fig. 6(b): Normalized P99 latency

横轴同 Fig. 6(a)。

每个 workload 下画 4 根柱子：

```text
F2FS-16
ReDisc-16
F2FS-512
ReDisc-512
```

纵轴：

```text
Normalized P99 latency
```

`F2FS-16 = 1.0`，越低越好。

### 预期目标

ReDisc 与 F2FS 的 throughput / P99 latency 接近，说明 ReDisc 的额外判断、有界 hold、WCE/SFI 路径不会引入明显前台性能开销。

### 本实验可以支持的论文结论

如果实验结果符合预期，本节可以得出以下结论：


本实验主要验证 ReDisc 的端到端性能安全性。由于 ReDisc 会暂缓部分 pending discard，并在 GC/update 路径中加入额外判断，因此需要先确认这些机制不会对前台 workload 造成明显性能退化。若在 discard/GC 压力较高的场景下观察到吞吐提升或尾延迟降低，则说明 ReDisc 有潜力降低碎片化 discard 对前台 I/O 的干扰；但其内部原因需要结合后续 discard 形态和模块分解实验进一步解释。

