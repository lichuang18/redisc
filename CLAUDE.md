# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 仓库定位

这是一个基于 Linux 5.15 F2FS 的仓外内核模块仓库，模块名为 `ef2fs`。当前主线不是重写整个 F2FS，而是在 **stock F2FS discard 路径** 上叠加 SWOD 和 WCE 两层机制：

- **SWOD**：在 pending discard 队列之上识别高价值窗口，并暂时 hold。
- **WCE**：主路径；让 GC 优先围绕 held / parked 目标窗口做 completion。
- **HBU**（可选）：在 hbu_targets 上做 OPU 分配，填满无效块空间。

整体闭环是：

`pending discard -> SWOD hold -> WCE completion -> HBU OPU allocation -> stock F2FS materialization/issue`

一个关键约束：**SWOD / WCE / HBU 都不直接伪造大 discard**。最终的连续 discard materialization 与 issue 仍然由 stock F2FS 完成。

## 常用命令

## 构建

构建模块：

```bash
make -j16
```

清理构建产物：

```bash
make clean
```

`Makefile` 通过当前内核头构建模块，依赖：

```bash
/lib/modules/$(uname -r)/build
```

## 常用装载/格式化/挂载流程

仓库里已有一键脚本：

```bash
./auto_insertko.sh
```

该脚本包含的常用流程是：重新编译、重载 `ef2fs`、重新 `mkfs.f2fs`、并以 `mode=lfs` 挂载到 `/mnt`。

## 实验与负载

顺序填盘：

```bash
./fill_data.sh
```

随机覆盖实验（仓库当前最常用的本地 workload 入口）：

```bash
./overwrite.sh
```

使用仓库自带 fio job：

```bash
fio test.fio
```

## 观测脚本

采集 SWOD 指标：

```bash
./collect_swod.sh 60
```

采集 WCE 指标：

```bash
./collect_wce.sh 60
```

这些脚本主要读取：

```bash
/sys/fs/ef2fs/<sb_id>/
/sys/kernel/debug/ef2fs/status
```

## 常用运行时开关

开启 SWOD：

```bash
echo 1 > /sys/fs/ef2fs/nvme2n1/swod_enable
```

开启 WCE：

```bash
echo 1 > /sys/fs/ef2fs/nvme2n1/swod_completion_enable
echo 1 > /sys/fs/ef2fs/nvme2n1/swod_gc_bg_enable
echo 1 > /sys/fs/ef2fs/nvme2n1/swod_gc_fg_enable
```

开启 HBU：

```bash
echo 1 > /sys/fs/ef2fs/nvme2n1/swod_hbu_enable
```

调整 SWOD hold 时间：

```bash
echo 200  > /sys/fs/ef2fs/nvme2n1/swod_hold_min_ms
echo 1000 > /sys/fs/ef2fs/nvme2n1/swod_hold_max_ms
```

实验时手动打开 urgent GC：

```bash
echo 1 > /sys/fs/ef2fs/nvme2n1/gc_urgent
```

## 测试现状

这个仓库里**没有发现独立的 lint 命令、单元测试框架或"单测单个 case"入口**。当前验证方式主要是：

1. 编译并重载模块；
2. 运行 fio workload（`overwrite.sh`、`fill_data.sh`、`test.fio`）；
3. 通过 sysfs/debugfs 与 `collect_swod.sh` / `collect_wce.sh` 观察行为；
4. 必要时查看 `debug.md` 中记录过的调试链路。

## 架构总览

## 大图景

这个仓库的核心思路是：**保留 stock F2FS 的真正执行路径，只在 discard 机会识别和 completion 决策层加控制逻辑。**

主要代码职责分布：

- `segment.c`：把 SWOD 接到 discard command 生命周期里，包括创建、merge/update、punch/remove、issue、pressure release、timeout sweep。
- `swod.c` / `swod.h`：SWOD 核心状态机；维护 segment/group 摘要、held_segmap、hbu_segmap、skip 判定、WCE 查询接口。
- `gc.c`：WCE 主实现位置；在 `get_victim_by_default()` 里加 target-first victim selection。
- `sysfs.c`：导出实验开关和统计项。
- `f2fs.h`：给 `discard_cmd_control` 和相关结构增加 SWOD/WCE/HBU 所需字段。

## SWOD 机制

SWOD 的基本单位不是单条 discard command，而是 **segment window / group**。

它的工作方式是：

- 用 `swod_seg_hint` 汇总每个 segment 的 pending-discard 可见度；
- 在 group 内评估连续子窗口，核心指标是 `qcov`（queue-visible coverage）和 `lres`（live residual）；
- 找到"pending discard 足够多、但 live residual 已经足够低"的窗口后，把它标成 `HELD`；
- issue 路径遇到完整落在 held window 内的 `D_PREP` cmd 时，先 skip，不立即发。

这部分的实现中心在 `swod.c`，而触发点在 `segment.c`。

## WCE 重点：当前主路径

**WCE 是当前版本的主路径，且实现方式是 target-first，不是重写 GC cost model。**

关键点：

- WCE 的目标集合来自 SWOD 暴露的 held / parked bitmap；
- `gc.c` 中的 `get_victim_by_default()` 会先判断当前 GC 是否允许进入 WCE target pass；
- 只有命中 held_segmap 的 candidate，才会在 target pass 中参与 victim 挑选；
- target pass 内部仍然沿用 stock F2FS 的合法性检查和 cost 选择；
- 如果目标集里找不到合法 victim，只回退一次到 stock picker，并记录 fallback 统计。

换句话说，WCE 做的事是：**先限制"去哪些候选里找"，而不是改写"在候选里怎么打分"。**

未来若要理解或修改 WCE，优先看：

- `gc.c:get_victim_by_default()`：target-first 主逻辑
- `gc.c:swod_wce_enabled()`：BG/FG 是否允许进入 WCE
- `swod.c:f2fs_swod_pick_held_dirty()`：从 held_segmap 中选 dirty segment

## HELD / PARKED 的关系

当前实现里，WCE 看到的是 held_segmap（只包含高覆盖率窗口，高碎片化窗口不设置 held_segmap）。

held_segmap 和 hbu_segmap 是互斥的：一个 segment 要么在 held_segmap（SWOD hold），要么在 hbu_segmap（HBU OPU），不会同时存在。

## HBU 机制

HBU（Hold Block Utilization）是在 hbu_targets 上做 OPU 分配的机制：

- 条件：高有效块 (>=80%) + 高碎片化 (nr_cmds >= 4)
- 分配新数据到 segment 的无效块空间，而不是新 segment
- 目的是减少碎片化 discard cmd 的数量

## 关键文件地图

- `readme.md`：当前最值得先读的实现总览。
- `desgin.md`：更完整的设计动机、架构推导和实验规划说明。
- `new_design_v4.md`：V4 设计文档，包含 SWOD/WCE/HBU 的详细设计。
- `swod.c` / `swod.h`：SWOD 核心状态机、WCE 查询接口。
- `segment.c`：discard 生命周期接线点，也是 SWOD 真正落地到 issue path 的地方。
- `gc.c`：WCE target-first victim selection 主入口。
- `sysfs.c`：所有关键 knob 和统计项出口。
- `f2fs.h`：`discard_cmd_control` 的字段扩展。
- `debug.md`：近期围绕 held 生命周期、skip 诊断、timeout sweep 的调试记录。

## 关键默认值

`segment.c:create_discard_cmd_control()` 和 `swod.c:f2fs_swod_init()` 里初始化的默认值对实验很重要：

| 参数 | 默认值 | 说明 |
|------|--------|------|
| swod_enable | 0 | SWOD 总开关 |
| swod_qcov_thr_bp | 8500 | hold qcov 阈值 (85%) |
| swod_lres_thr_bp | 1000 | hold lres 阈值 (10%) |
| swod_hold_min_ms | 50 | 最小 hold 时间 |
| swod_hold_max_ms | 300 | 最大 hold 时间 |
| swod_max_held_groups | 128 | 最大并发 hold 窗口数 |
| swod_completion_enable | 0 | WCE 总开关 |
| swod_gc_bg_enable | 1 | BG GC WCE |
| swod_gc_fg_enable | 1 | FG GC WCE |
| swod_frag_thr_bp | 7000 | 碎片化阈值 (70%) |
| swod_frag_max_avg_piece_blks | 16 | 平均 cmd 大小阈值 |
| swod_frag_timeout_k | 3 | 碎片化超时倍数 |
| swod_hbu_enable | 0 | HBU 总开关 |
| swod_hbu_valid_thr_bp | 8000 | HBU 有效块阈值 (80%) |
| swod_hbu_min_cmds | 4 | HBU nr_cmds 阈值 |

也就是说：**SWOD 和 WCE 默认都不是开箱即开，实验时通常通过 sysfs 手动打开。**

## 观测重点

常用只读统计项：

- SWOD 生命周期：`swod_held_groups`, `swod_hold_cnt`, `swod_skip_cnt`, `swod_success_release_cnt`, `swod_timeout_release_cnt`, `swod_pressure_release_cnt`
- SWOD 诊断：`swod_skip_check_cnt`, `swod_skip_miss_*`, `swod_eval_blocked_cnt`, `swod_eval_no_candidate_cnt`
- WCE：`swod_gc_pick_bg_cnt`, `swod_gc_pick_fg_cnt`, `swod_gc_fallback_cnt`
- HBU：`swod_hbu_target_cnt`, `swod_hbu_target_add_cnt`, `swod_hbu_target_rm_cnt`, `swod_hbu_alloc_success_cnt`, `swod_hbu_alloc_skip_held_cnt`, `swod_hbu_alloc_skip_full_cnt`

排查问题时，建议顺序：先读 `readme.md`，再对照 `swod.c`、`segment.c`、`gc.c` 的真实实现。
