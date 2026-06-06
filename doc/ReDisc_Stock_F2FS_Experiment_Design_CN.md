# ReDisc Motivation 实验设计：只跑 stock F2FS 的 trace-driven study

> 目标：只用 **stock F2FS + 轻量级 tracepoint** 验证 Motivation 中的问题是否存在。  
> 边界：本实验不启用 ReDisc、SWOD 或 DSU；不声称测量真实 NAND write amplification；不依赖专用 SSD、FEMU、Open-Channel SSD、ZNS 或 FTL 修改。  
> 核心原则：tracepoint 可以观察，但不能改变 F2FS 的 allocation、checkpoint、cleaning、discard merge、discard issue 决策。

---

## 1. Motivation 中是否需要自己做 GC/WA 实验？

不需要重复已有工作的 GC/WA 实验。Motivation 中应当这样分工：

1. **引用已有工作**：说明 discard/TRIM 与设备 GC、WA、性能、I/O contention 相关。
2. **本文自己的实验**：只证明 stock F2FS 中存在 ReDisc 关注的 host-visible 问题，包括：
   - 大量小 discard command；
   - 小 discard 在同一 segment window 中存在短期 follow-up；
   - stock F2FS 可能过早发射局部小 discard；
   - pending discard backlog 来自 update path、checkpoint 和 cleaning 的阶段性显化；
   - 现有 knobs 主要改变 backlog 消费节奏，不能显式保留局部机会或塑形 future demand。

这样写更稳：已有工作证明 discard 重要；本文证明 stock F2FS 的 discard lifecycle 中存在可优化机会。

---

## 2. 实验总览

### 2.1 实验问题

| 实验 | 问题 | 对应 Motivation |
|---|---|---|
| Exp-1 discard 请求形态 | stock F2FS 是否产生大量小 discard？ | Observation 1 |
| Exp-2 same-window follow-up | 已发小 discard 后，同 window 是否继续出现 discard？ | Observation 2 |
| Exp-3 demand pipeline | pending discard backlog 是否由 update path/checkpoint/cleaning 显化？ | Observation 3 |
| Exp-4 stock knob 边界 | 现有参数是否只能调消费节奏，而不能保留机会/塑形 demand？ | 设计必要性 |
| Exp-5 前台 I/O 干扰 | discard fragmentation 是否与前台 p99/p999 latency 相关？ | 性能动机，可选 |

### 2.2 推荐实验变量

| 变量 | 推荐值 |
|---|---|
| 文件系统 | stock F2FS + tracepoints |
| 设备 | 普通 NVMe/SATA SSD，记录型号和固件版本 |
| 利用率 | 50%, 70%, 85%, 90% |
| workload | fio random overwrite, Filebench fileserver, Filebench varmail, RocksDB/db_bench 或 MySQL/TPCC |
| 每组时长 | warmup 60s + run 600s |
| 重复次数 | 至少 3 次 |
| segment window size `W` | 1, 2, 4, 8 segments，主实验建议 4 或 8 |
| 小 discard 阈值 `T` | 根据 Exp-1 分布选，例如 P50 或 32/64/128 blocks |
| follow-up 时间窗口 `H` | 10 ms, 50 ms, 100 ms, 1 checkpoint |
| mergeable gap `G` | 0, 4, 16, 32 blocks |

---

## 3. 环境准备

### 3.1 设备选择

假设测试盘为 `/dev/nvme1n1`，挂载点为 `/mnt/f2fs`。强烈建议使用一块独立空盘，避免误删系统盘。

```bash
export DEV=/dev/nvme1n1
export MNT=/mnt/f2fs
lsblk -o NAME,SIZE,MODEL,SERIAL,MOUNTPOINT
sudo nvme id-ctrl $DEV | tee nvme_id_ctrl.txt || true
sudo smartctl -a $DEV | tee smart_before.txt || true
```

记录：设备型号、固件版本、容量、sector size、kernel version、f2fs-tools version、mount options。

```bash
uname -a | tee env_kernel.txt
mkfs.f2fs -V | tee env_f2fs_tools.txt
```

### 3.2 内核配置

需要打开 F2FS、ftrace、tracepoint 和 block trace。

推荐配置项：

```text
CONFIG_F2FS_FS=y or m
CONFIG_TRACING=y
CONFIG_FTRACE=y
CONFIG_FUNCTION_TRACER=y
CONFIG_FUNCTION_GRAPH_TRACER=y
CONFIG_EVENT_TRACING=y
CONFIG_BLK_DEV_IO_TRACE=y
CONFIG_KPROBES=y
CONFIG_BPF_EVENTS=y   # optional
```

编译安装内核：

```bash
git clone https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git linux-redisc-trace
cd linux-redisc-trace
cp /boot/config-$(uname -r) .config
make olddefconfig
scripts/config --enable F2FS_FS
scripts/config --enable TRACING
scripts/config --enable FTRACE
scripts/config --enable EVENT_TRACING
scripts/config --enable BLK_DEV_IO_TRACE
make -j$(nproc)
sudo make modules_install
sudo make install
sudo reboot
```

---

## 4. Tracepoint 设计

### 4.1 最小事件集合

必须记录以下事件：

| 事件名 | 位置 | 记录字段 | 用途 |
|---|---|---|---|
| `redisc_invalidate` | F2FS block invalidation 路径 | time, blkaddr, segno, reason | 统计 update path 产生 invalid blocks |
| `redisc_discard_queue` | pending discard entry 创建/扩展/合并处 | time, start_blk, len, segno, window_id, action | 统计 demand 何时进入 queue |
| `redisc_discard_issue` | F2FS submit discard 前 | time, start_blk, len, segno, window_id | 统计 stock F2FS 发射行为 |
| `block:block_rq_issue` | block layer | sector, nr_sector, rwbs 包含 D | 设备可见 discard issue |
| `block:block_rq_complete` | block layer | sector, nr_sector, error, rwbs 包含 D | discard completion latency |
| `redisc_cp_start/end` | checkpoint start/end | time, reason | 关联 demand 显化 |
| `redisc_gc_segment` | cleaning/prefree/free transition | time, segno, valid_blocks, dirty_blocks | 关联 cleaning 与 discard backlog |

### 4.2 如何定位内核函数

不同 Linux 版本中 F2FS 函数名可能略有差异，先用 grep 定位。

```bash
cd linux-redisc-trace
grep -R "f2fs_invalidate_blocks" -n fs/f2fs
grep -R "queue_discard" -n fs/f2fs
grep -R "issue_discard" -n fs/f2fs
grep -R "submit_discard" -n fs/f2fs
grep -R "write_checkpoint" -n fs/f2fs
grep -R "prefree" -n fs/f2fs/segment.c
```

通常可优先考虑这些位置：

| 观测点 | 常见函数/文件 | 说明 |
|---|---|---|
| invalid block 产生 | `f2fs_invalidate_blocks()` in `fs/f2fs/segment.c` | 记录旧 block 被 invalidated |
| pending discard 形成 | `__queue_discard_cmd()` / `add_discard_addrs()` / discard entry merge 相关函数 | 记录进入 pending queue 的范围 |
| discard 发射 | `__submit_discard_cmd()` / `__issue_discard_cmd()` / `issue_discard_thread()` | 记录 F2FS 决定提交的 discard |
| checkpoint | `f2fs_write_checkpoint()` in `fs/f2fs/checkpoint.c` | 记录 CP start/end |
| cleaning/prefree | `f2fs_clear_prefree_segments()` / segment cleaning 相关函数 | 记录 prefree/free 转换 |

### 4.3 快速实现方式 A：trace_printk

适合快速验证。缺点是格式不如正式 tracepoint 稳定。

示例：在 `f2fs_invalidate_blocks()` 中加入：

```c
trace_printk("REDISC_INV blk=%u seg=%u\n", blkaddr, GET_SEGNO(sbi, blkaddr));
```

在 discard issue 前加入：

```c
trace_printk("REDISC_ISSUE start=%u len=%u seg=%u win=%u\n",
             start_blk, len, GET_SEGNO(sbi, start_blk),
             GET_SEGNO(sbi, start_blk) / window_segments);
```

### 4.4 推荐实现方式 B：正式 tracepoint

在 `include/trace/events/f2fs.h` 中新增事件，例如：

```c
TRACE_EVENT(f2fs_redisc_discard_issue,
    TP_PROTO(dev_t dev, block_t start, unsigned int len,
             unsigned int segno, unsigned int win),
    TP_ARGS(dev, start, len, segno, win),
    TP_STRUCT__entry(
        __field(dev_t, dev)
        __field(block_t, start)
        __field(unsigned int, len)
        __field(unsigned int, segno)
        __field(unsigned int, win)
    ),
    TP_fast_assign(
        __entry->dev = dev;
        __entry->start = start;
        __entry->len = len;
        __entry->segno = segno;
        __entry->win = win;
    ),
    TP_printk("dev=%d,%d start=%u len=%u seg=%u win=%u",
        MAJOR(__entry->dev), MINOR(__entry->dev),
        __entry->start, __entry->len, __entry->segno, __entry->win)
);
```

然后在 F2FS discard issue 路径调用：

```c
trace_f2fs_redisc_discard_issue(sb->s_dev, start_blk, len,
    GET_SEGNO(sbi, start_blk), GET_SEGNO(sbi, start_blk) / window_segments);
```

> 建议：先用 trace_printk 跑通，再替换成正式 tracepoint。

---

## 5. 文件系统准备步骤

每组实验都从干净状态开始。

```bash
export DEV=/dev/nvme1n1
export MNT=/mnt/f2fs
sudo umount $MNT 2>/dev/null || true
sudo wipefs -a $DEV
sudo blkdiscard -f $DEV || true
sudo mkfs.f2fs -f $DEV
sudo mkdir -p $MNT
sudo mount -t f2fs -o discard,background_gc=on $DEV $MNT
mount | grep $MNT | tee mount_options.txt
```

不要在 Motivation 实验中手动运行 `fstrim`，否则会混入用户主动 trim 行为。统一使用 F2FS online/background discard 行为。

### 5.1 预填充到指定利用率

以目标利用率 70% 为例：

```bash
export TARGET_UTIL=70
TOTAL_KB=$(df -k --output=size $MNT | tail -1)
TARGET_KB=$((TOTAL_KB * TARGET_UTIL / 100))

sudo fio --name=prefill \
  --filename=$MNT/prefill.dat \
  --rw=write --bs=1M --size=${TARGET_KB}k \
  --ioengine=libaio --iodepth=16 --direct=1 \
  --numjobs=1 --group_reporting

sync
sleep 10
df -h $MNT | tee df_after_prefill.txt
```

为了产生 overwrite/delete，可以额外准备多个 workload 文件：

```bash
sudo mkdir -p $MNT/work
sudo fio --name=init_files \
  --directory=$MNT/work \
  --rw=write --bs=1M --size=20G \
  --nrfiles=16 --filesize=1280M \
  --ioengine=libaio --iodepth=16 --direct=1 \
  --group_reporting
sync
```

---

## 6. Trace 采集流程

### 6.1 使用 trace-cmd 采集

安装：

```bash
sudo apt-get install -y trace-cmd fio filebench smartmontools nvme-cli
```

开启事件：

```bash
sudo trace-cmd reset
sudo trace-cmd record \
  -e block:block_rq_issue \
  -e block:block_rq_complete \
  -e f2fs:* \
  -o trace_stock_f2fs.dat \
  -- sleep 660
```

更推荐把 workload 放到 trace-cmd 后面：

```bash
sudo trace-cmd record \
  -e block:block_rq_issue \
  -e block:block_rq_complete \
  -e f2fs:* \
  -o trace_fio_randwrite.dat \
  -- bash run_fio_randwrite.sh
```

导出文本：

```bash
sudo trace-cmd report trace_fio_randwrite.dat > trace_fio_randwrite.txt
```

### 6.2 使用 trace_pipe 采集 trace_printk

如果使用 `trace_printk`：

```bash
sudo sh -c 'echo nop > /sys/kernel/debug/tracing/current_tracer'
sudo sh -c 'echo 1 > /sys/kernel/debug/tracing/tracing_on'
sudo cat /sys/kernel/debug/tracing/trace_pipe | tee redisc_trace.log &
TRACE_PID=$!
# run workload
sudo kill $TRACE_PID
```

---

## 7. Workload 执行步骤

### 7.1 fio random overwrite

创建脚本 `run_fio_randwrite.sh`：

```bash
#!/usr/bin/env bash
set -e
MNT=/mnt/f2fs
fio --name=rand_overwrite \
  --filename=$MNT/work/randfile.dat \
  --rw=randwrite --bs=4k --size=20G \
  --ioengine=libaio --iodepth=32 --numjobs=4 \
  --direct=1 --time_based=1 --runtime=600 --ramp_time=60 \
  --group_reporting --output=fio_randwrite.json --output-format=json
sync
```

初始化文件：

```bash
sudo fio --name=init_randfile \
  --filename=$MNT/work/randfile.dat \
  --rw=write --bs=1M --size=20G \
  --ioengine=libaio --iodepth=16 --direct=1 --group_reporting
sync
```

运行：

```bash
chmod +x run_fio_randwrite.sh
sudo trace-cmd record -e block:block_rq_issue -e block:block_rq_complete -e f2fs:* \
  -o trace_fio_randwrite.dat -- ./run_fio_randwrite.sh
sudo trace-cmd report trace_fio_randwrite.dat > trace_fio_randwrite.txt
```

### 7.2 Filebench fileserver

创建 `fileserver.f`：

```text
load fileserver
set $dir=/mnt/f2fs/filebench
set $nfiles=100000
set $meandirwidth=20
set $filesize=128k
set $nthreads=16
set $iosize=4k
run 600
```

运行：

```bash
sudo mkdir -p /mnt/f2fs/filebench
sudo trace-cmd record -e block:block_rq_issue -e block:block_rq_complete -e f2fs:* \
  -o trace_fileserver.dat -- filebench -f fileserver.f
sudo trace-cmd report trace_fileserver.dat > trace_fileserver.txt
```

### 7.3 Filebench varmail

创建 `varmail.f`：

```text
load varmail
set $dir=/mnt/f2fs/varmail
set $nfiles=100000
set $meandirwidth=20
set $filesize=16k
set $nthreads=16
set $iosize=4k
run 600
```

运行：

```bash
sudo mkdir -p /mnt/f2fs/varmail
sudo trace-cmd record -e block:block_rq_issue -e block:block_rq_complete -e f2fs:* \
  -o trace_varmail.dat -- filebench -f varmail.f
sudo trace-cmd report trace_varmail.dat > trace_varmail.txt
```

### 7.4 RocksDB/db_bench 可选

```bash
./db_bench --benchmarks=fillrandom,overwrite,readwhilewriting \
  --db=/mnt/f2fs/rocksdb \
  --num=50000000 --value_size=1024 --key_size=16 \
  --threads=16 --duration=600 \
  --statistics=1 2>&1 | tee db_bench.log
```

---

## 8. Exp-1：discard 请求形态实验

### 8.1 步骤

1. 重新格式化 F2FS。
2. 挂载 `-o discard,background_gc=on`。
3. 预填充到目标利用率，例如 70%。
4. 初始化 workload 文件。
5. 启动 trace。
6. 运行 fio/Filebench workload 600s。
7. 导出 trace。
8. 重复 3 次。

### 8.2 从 trace 中提取 discard request

从 block layer 事件中筛选 discard：

```bash
grep -E "block_rq_issue|block_rq_complete" trace_fio_randwrite.txt | grep -E " D |D" > block_discard.txt
```

如果使用自定义事件：

```bash
grep "f2fs_redisc_discard_issue" trace_fio_randwrite.txt > f2fs_discard_issue.txt
```

### 8.3 指标

| 指标 | 计算方式 |
|---|---|
| command count | discard issue 事件数量 |
| total discarded blocks | 所有 discard len 求和 |
| command size distribution | 对每个 discard len 画 CDF/histogram |
| small discard ratio | `len <= T` 的 discard command 占比 |
| discard completion latency | block issue 与 complete 按 sector/len 近似匹配 |

### 8.4 预期写入 Motivation 的结论模板

```text
在 stock F2FS 上，更新密集型 workload 会产生大量小 discard command。
例如，在 fileserver 下，[X%] 的 discard command 小于 [Y] blocks。
这说明 F2FS discard 优化不能只关注少量大范围 discard，必须处理小请求的碎片化和发射时机。
```

---

## 9. Exp-2：same-window follow-up / premature issue 实验

### 9.1 定义

```text
segment window: 连续 W 个 segment
window_id = segno / W
small discard: len <= T
follow-up window: H ms 或下一个 checkpoint 前
mergeable: follow-up range 与已发 range 相邻，或 gap <= G blocks
```

### 9.2 步骤

1. 使用 Exp-1 的同一批 trace。
2. 对每个 `redisc_discard_issue` 事件，提取 `time, start_blk, len, segno, window_id`。
3. 只保留 `len <= T` 的小 discard。
4. 对每个小 discard，在之后 `H ms` 内查找同 `window_id` 的 `redisc_discard_queue` 或 `redisc_discard_issue` 事件。
5. 判断是否存在 follow-up，是否 mergeable。
6. 计算 issue 时 window coverage 与 `H` 后 coverage。

### 9.3 指标公式

```text
same_window_followup_rate
= #issued small discard with same-window follow-up within H / #issued small discard

mergeable_followup_rate
= #issued small discard with mergeable follow-up within H / #issued small discard

window_growth_ratio
= coverage(window, t_issue + H) / coverage(window, t_issue)

premature_issue_rate
= #issued small discard whose window coverage grows by >= delta within H / #issued small discard
```

其中 coverage 可以定义为某个 window 内已经进入 pending discard 或已经 issue 的 discard blocks 占该 window 总 blocks 的比例。

### 9.4 Sensitivity

至少测试：

```text
W = 1, 2, 4, 8 segments
H = 10 ms, 50 ms, 100 ms, next checkpoint
G = 0, 4, 16, 32 blocks
```

### 9.5 Motivation 结论模板

```text
[X%] 的已发小 discard 在 [H] ms 内出现 same-window follow-up，
其中 [Y%] 与原请求相邻或 gap 小于 [G] blocks。
这说明小 discard 并非完全孤立产生，而是具有局部时间/空间相关性。
stock F2FS 的 current-state-driven issue policy 可能在窗口成熟前过早发射这些请求。
```

---

## 10. Exp-3：discard demand pipeline 实验

### 10.1 步骤

1. 使用同一 stock F2FS trace。
2. 提取 `redisc_invalidate`，统计 cumulative invalidated blocks。
3. 提取 `redisc_discard_queue`，统计 cumulative newly queued discard blocks。
4. 提取 `redisc_discard_issue`，统计 cumulative issued discard blocks。
5. 计算 pending backlog：

```text
pending_backlog_blocks(t) = queued_discard_blocks(t) - issued_discard_blocks(t)
```

6. 标注 checkpoint start/end 与 cleaning/prefree/free transition。
7. 画时间序列。

### 10.2 图

| 图 | x 轴 | y 轴 | 说明 |
|---|---|---|---|
| Fig. 4(a) | time | cumulative invalidated blocks | update path 持续制造 invalid blocks |
| Fig. 4(b) | time | newly queued discard blocks | checkpoint/cleaning 后 demand 显化 |
| Fig. 4(c) | time | pending backlog blocks | issue thread 面临的存量压力 |
| Fig. 4(d) | time | backlog + CP/GC vertical lines | 证明 backlog 与 CP/cleaning 相关 |

### 10.3 Motivation 结论模板

```text
invalidated blocks 随前台更新持续增长，而 newly queued discard blocks 在 checkpoint/cleaning 附近阶段性抬升。
这说明 pending discard backlog 是上游 update path 失效块在下游的延迟显化，而不是 issue thread 即时制造的。
因此，只调 issue thread 的发射速率无法从源头减少 future discard demand 及其破碎度。
```

---

## 11. Exp-4：stock F2FS knob 边界实验

### 11.1 目的

防止审稿人认为 ReDisc 只是调 `max_discard_request` 或 `discard_io_aware`。该实验只使用 stock F2FS 现有参数，证明它们主要改变 backlog 消费节奏，但不能显式识别 segment-window 机会或塑形 future demand。

### 11.2 查找 sysfs 参数

```bash
F2FS_SYS=/sys/fs/f2fs/$(basename $DEV)
ls $F2FS_SYS | grep -E "discard|issue|gc|ipu|ssr"
for f in $F2FS_SYS/*discard* $F2FS_SYS/*issue*; do echo $f; cat $f 2>/dev/null; done
```

### 11.3 配置组

| 配置 | 目的 | 示例 |
|---|---|---|
| default | stock baseline | 不改参数 |
| aggressive issue | 更快消费 backlog | 提高 `max_discard_request`，关闭或降低 I/O-aware 限制 |
| conservative issue | 更慢消费 backlog | 降低 `max_discard_request`，启用 I/O-aware |
| larger granularity | 减少小请求发射 | 提高 `discard_granularity` |
| urgent/background | 空间压力触发更积极 discard | 调整 `discard_urgent_util` 等 |

示例命令，根据实际 sysfs 文件名调整：

```bash
# default: no change

# aggressive
sudo sh -c "echo 1024 > $F2FS_SYS/max_discard_request" || true
sudo sh -c "echo 0 > $F2FS_SYS/discard_io_aware" || true

# conservative
sudo sh -c "echo 8 > $F2FS_SYS/max_discard_request" || true
sudo sh -c "echo 1 > $F2FS_SYS/discard_io_aware" || true

# larger granularity
sudo sh -c "echo 64 > $F2FS_SYS/discard_granularity" || true
```

### 11.4 指标

同一 workload 下比较：

```text
discard command count
small discard ratio
same-window follow-up rate
mergeable follow-up rate
premature issue rate
pending backlog blocks
foreground write/fsync p99/p999 latency
```

### 11.5 Motivation 结论模板

```text
现有参数可以改变 pending discard 的消费速度，例如 command count 或 backlog 大小会变化；
但是 same-window follow-up 和 premature issue 仍然存在，且 future demand 的生成趋势没有被改变。
这说明 stock knobs 缺少对 segment-window opportunity 和 upstream demand shaping 的显式建模。
```

---

## 12. Exp-5：前台 I/O 干扰实验（可选但建议）

### 12.1 目的

证明 discard fragmentation 不只是 trace 上的现象，还与用户可感知性能相关。

### 12.2 步骤

1. 跑同样 workload。
2. 同时记录 fio 的 clat percentiles 或 Filebench latency。
3. 从 trace 中按 1s 时间桶统计 discard command count、small discard count、pending backlog。
4. 计算这些 discard 指标与 foreground p99/p999 latency 的相关性。

### 12.3 fio 输出指标

```bash
jq '.jobs[0].write.clat_ns.percentile' fio_randwrite.json
jq '.jobs[0].write.iops' fio_randwrite.json
jq '.jobs[0].write.bw' fio_randwrite.json
```

### 12.4 Motivation 结论模板

```text
discard burst 与 foreground p99/p999 latency spike 在时间上存在相关性。
这说明 discard lifecycle 的碎片化和发射时机不仅影响后台维护开销，也可能干扰前台 I/O。
```

---

## 13. 数据处理脚本框架

建议将 trace 转成 CSV：

```text
time,event,start_blk,len,segno,window_id,action
0.001,inv,12345,1,12,3,-
0.010,queue,12340,8,12,3,new
0.020,issue,12340,8,12,3,-
0.030,complete,12340,8,12,3,-
```

Python 分析伪代码：

```python
import pandas as pd

W = 4          # segments per window
T = 64         # small discard threshold in blocks
H = 0.050      # 50 ms
G = 16         # mergeable gap in blocks

df = pd.read_csv('trace_events.csv')
issues = df[df.event == 'issue']
queues = df[df.event == 'queue']
small = issues[issues.len <= T]

follow = 0
mergeable = 0
for _, r in small.iterrows():
    cand = queues[(queues.time > r.time) &
                  (queues.time <= r.time + H) &
                  (queues.window_id == r.window_id)]
    if len(cand) > 0:
        follow += 1
        end = r.start_blk + r.len
        for _, c in cand.iterrows():
            gap1 = abs(c.start_blk - end)
            gap2 = abs(r.start_blk - (c.start_blk + c.len))
            if min(gap1, gap2) <= G:
                mergeable += 1
                break

same_window_followup_rate = follow / len(small)
mergeable_followup_rate = mergeable / len(small)
print(same_window_followup_rate, mergeable_followup_rate)
```

---

## 14. Motivation 中最终需要填的数字

| 占位符 | 来源实验 | 含义 |
|---|---|---|
| `[X%]` | Exp-1 | 小 discard command 占比 |
| `[Y] blocks` | Exp-1 | 小 discard 阈值 |
| `[P50/P90/P99]` | Exp-1 | discard size 分布 |
| `[Z%]` | Exp-2 | same-window follow-up rate |
| `[M%]` | Exp-2 | mergeable follow-up rate |
| `[C1%] -> [C2%]` | Exp-2 | window coverage growth |
| `[B] blocks` | Exp-3 | backlog 峰值或平均值 |
| `[L] ms` | Exp-5 | p99/p999 latency spike |

---

## 15. 实验运行模板

每个 workload、每个利用率、每个重复次数都执行：

```bash
#!/usr/bin/env bash
set -e

DEV=/dev/nvme1n1
MNT=/mnt/f2fs
UTIL=$1
WL=$2
REP=$3
OUT=results/${WL}_util${UTIL}_rep${REP}
mkdir -p $OUT

sudo umount $MNT 2>/dev/null || true
sudo wipefs -a $DEV
sudo blkdiscard -f $DEV || true
sudo mkfs.f2fs -f $DEV | tee $OUT/mkfs.log
sudo mkdir -p $MNT
sudo mount -t f2fs -o discard,background_gc=on $DEV $MNT
mount | grep $MNT | tee $OUT/mount.log

TOTAL_KB=$(df -k --output=size $MNT | tail -1)
TARGET_KB=$((TOTAL_KB * UTIL / 100))
sudo fio --name=prefill --filename=$MNT/prefill.dat \
  --rw=write --bs=1M --size=${TARGET_KB}k \
  --ioengine=libaio --iodepth=16 --direct=1 \
  --numjobs=1 --group_reporting | tee $OUT/prefill.log
sync
sleep 10

df -h $MNT | tee $OUT/df_before_workload.txt
sudo trace-cmd record -e block:block_rq_issue -e block:block_rq_complete -e f2fs:* \
  -o $OUT/trace.dat -- ./run_${WL}.sh
sudo trace-cmd report $OUT/trace.dat > $OUT/trace.txt
sync
sudo umount $MNT
```

运行矩阵：

```bash
for util in 50 70 85 90; do
  for wl in fio_randwrite fileserver varmail; do
    for rep in 1 2 3; do
      ./run_one.sh $util $wl $rep
    done
  done
done
```

---

## 16. 最终图表清单

| 图号 | 图名 | 对应实验 | Motivation 作用 |
|---|---|---|---|
| Fig. 1 | F2FS discard lifecycle | 设计图 | 建立因果链 |
| Fig. 2 | Discard command size distribution | Exp-1 | 证明大量小 discard |
| Fig. 3 | Discard latency vs size / burst | Exp-1/5 | 说明碎片化与发射时机重要 |
| Fig. 4 | Same-window follow-up and premature issue | Exp-2 | 证明局部机会被过早消费 |
| Fig. 5 | Discard demand pipeline | Exp-3 | 证明 backlog 根源在 update path |
| Fig. 6 | Existing knob limitation | Exp-4 | 证明不是简单调参 |

---

## 17. 论文中实验方法段落模板

```text
To validate the motivation, we conduct a trace-driven study on stock F2FS running on commodity SSDs. We only add lightweight tracepoints to record the lifecycle of discard demand, including block invalidation, pending discard creation, discard issuing, discard completion, checkpoint, and cleaning events. The tracepoints do not alter F2FS allocation, checkpointing, cleaning, discard merging, or discard issuing policies. Therefore, all observations in this section reflect the behavior of stock F2FS.

Unlike cross-layer approaches that rely on device-side monitors or simulator-visible FTL statistics, our study targets general block devices and uses only host-visible information. The goal of this motivation study is not to quantify internal NAND write amplification, but to reveal whether stock F2FS exposes fragmented, premature, and upstream-shaped discard demand that can be optimized before commands reach the device.
```

中文版本：

```text
为了验证动机，本文首先在普通 SSD 上对 stock F2FS 进行 trace-driven 分析。我们只加入轻量级 tracepoint，用于记录 block invalidation、pending discard 创建、discard 发射、discard 完成、checkpoint 和 cleaning 等事件；这些 tracepoint 不改变 F2FS 的分配、checkpoint、cleaning、discard 合并或发射策略。因此，本节所有 observation 都反映 stock F2FS 自身行为。

与依赖设备内部 monitor 或模拟器可见 FTL 统计的 cross-layer 方法不同，本文面向通用块设备，只使用 host-visible 信息。本节动机实验的目标不是量化设备内部 NAND write amplification，而是揭示 stock F2FS 是否存在碎片化、过早发射、以及由上游更新路径塑形的 discard demand，从而说明在命令到达设备之前进行优化具有必要性。
```
