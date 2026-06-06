# Motivation Experiment Harness

> 用途：为论文 Motivation 部分收集数据，证明 stock F2FS 中存在 discard 机会与压力问题。
> 原则：只跑 **stock F2FS**，不启用 ef2fs/SWOD/ReDisc。

## 目录结构

```
doc/motivation/
├── README.md                ← 本文件
├── setup/
│   └── prepare_device.sh    ← 格式化 + 挂载 stock F2FS
├── scripts/
│   ├── collect_state.sh     ← sysfs 状态采样（后台运行）
│   ├── run_motivation.sh    ← 端到端实验执行器
│   ├── parse_trace.py       ← trace-cmd 输出 → queue.csv / issue.csv
│   └── analyze_motivation.py ← 离线分析与绘图
└── results/                  ← 实验产物（自动创建）
```

## 快速开始

### 方式一：一步执行（推荐）

```bash
cd /home/lch/work/gf_discard/redisc
sudo ./doc/motivation/scripts/run_motivation.sh
```

默认参数：RUNTIME=300s, RAMP=60s，测试文件写入 `/mnt/mot_test/`（避免与旧残留文件冲突）

缩短验证：

```bash
sudo RUNTIME=60 RAMP=10 ./doc/motivation/scripts/run_motivation.sh
```

### 方式二：分步执行

```bash
# 1. 准备 stock F2FS
sudo ./doc/motivation/setup/prepare_device.sh

# 2. 解析 trace（实验结束后）
python3 ./doc/motivation/scripts/parse_trace.py ./results/f2fs-discard.trace ./results/

# 3. 分析 + 绘图
python3 ./doc/motivation/scripts/analyze_motivation.py ./results/ 60
```

## 实验配置

通过环境变量覆盖：

| 变量 | 默认值 | 说明 |
|---|---|---|
| `RUNTIME` | 300 | fio 运行时间（秒） |
| `RAMP` | 60 | ramp time（秒） |
| `WORKLOAD` | randwrite | 预填方式：randwrite（直接跑 fio）/ fill50/fill70/fill85 |
| `DEV` | /dev/nvme2n1p1 | 测试分区（70G） |
| `MNT` | /mnt/f2fs_stock | 挂载点 |
| `OUT` | ./results | 结果目录 |

## 产物说明

```
results/
├── fill.out             # 预填充 fio 输出
├── life_*_kb.txt        # F2FS lifetime write 计数器
├── state.csv            # sysfs 采样（100ms 间隔）
├── f2fs-discard.dat     # trace-cmd 原始二进制
├── f2fs-discard.trace   # trace-cmd 文本输出
├── queue.csv            # QUEUE 事件解析结果
├── issue.csv            # ISSUE 事件解析结果
├── fio.json             # fio 结果
└── fig_*.pdf            # 分析图表
```

## 分析图表

| 图表 | 对应 Figure | 内容 |
|---|---|---|
| `fig_length_dist.pdf` | M1 | discard 长度分布 + latency knee |
| `fig_case_study.pdf` | M2 | 单个 segment-window 的 case study 时间线 |
| `fig_followup_rate.pdf` | M3 | same-window follow-up rate vs H |
| `fig_premature_by_len.pdf` | M4 | premature issue rate 按长度分组 |
| `fig_demand_pipeline.pdf` | M5 | undiscard/queued/issued 时间序列 |

## 关键参数

- **窗口大小**：4 segments = 8 MB = 2048 blocks（见 `BLKS_PER_SEG=512`, `WIN_SEGS=4`）
- **small discard 阈值**：32 blocks
- **H 值**：follow-up 分析使用 H ∈ {10, 25, 50, 100, 200} ms

## 注意事项

- `run_motivation.sh` 会对 nvme2n1p1 执行 `blkdiscard` + `mkfs.f2fs`，是破坏性操作。
- `collect_state.sh` 默认 100ms 采样；可传入第三个参数调整为其他间隔。
- `analyze_motivation.py` 依赖 `pandas`, `numpy`, `matplotlib`。
- Figure M1 的 latency 数据是启发式估计；真实 latency 需从 `block_rq_complete` trace 中解析 `nr_sector` 和完成时间差。
