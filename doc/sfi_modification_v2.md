# SFI 修改方案 v1 (已完成)

## 修改日期

2026-06-12

## 背景

### 问题

当前 SFI 设计过于宽松，导致 varmail 测试中 dc_create 减少 77%，过度干预了正常 discard 流程。

**根本原因**：
1. `swod_frag_ipu_max_pend_blks = 32` 阈值太小，几乎所有小 fragment 都满足
2. 方案3（基于队列压力）设计不合理，用硬编码阈值判断

### 观察

varmail 的 workload 特性：
- 大量小文件、频繁创建删除
- 产生大量 small discard
- discard 粒度为 16 blocks（本身就很小）
- SFI 让 fragment 直接 IPU 后，产生更多 live blocks 碎片化

fileserver 的 workload 特性：
- 顺序读写大文件
- dc_create 减少 11%（合理范围）
- SFI 影响适度

## 设计原则

1. **SFI 是辅助路径，不是主路径**：只做窄的 fragment 抑制
2. **基于 segment 碎片化程度判断**：用已有的 `seg_hint` 信息
3. **不引入新字段**：复用 F2FS 原有状态

## 修改方案

### 修改文件

- `swod.c`：`f2fs_swod_should_frag_ipu()` 函数

### 核心思路

用 **segment 碎片化程度** 来判断是否应该允许 SFI IPU：

| 条件 | 含义 | 判断 |
|------|------|------|
| `pend_blks` 很小 | segment 碎片化不严重 | **不应该 IPU**，fragment 不会造成太多碎片 |
| `pend_blks` 较大 | segment 碎片化严重 | **可以允许 IPU**，减少新 fragment 继续形成 |

### 新增判断逻辑

在现有过滤条件之后，增加：

```c
/*
 * 方案3：基于 segment 碎片化程度的判断
 * 如果该 segment 的碎片化程度不够严重，不应该允许 IPU
 * 只有当 segment 确实很碎时，SFI 才有意义
 */
pend_blks = READ_ONCE(sw->seg_hint[segno].pend_blks);
nr_cmds = READ_ONCE(sw->seg_hint[segno].nr_cmds);

/*
 * 新增判断：
 * 1. pend_blks 必须 >= discard_granularity
 *    否则 segment 碎片化不够严重，SFI 没有必要
 * 2. nr_cmds 必须足够多
 *    否则说明该 segment 的碎片不是来自多个 cmd 的合并
 *
 * 阈值设计：
 * - pend_blks >= dcc->discard_granularity：至少有 1 个 segment 的碎片
 * - nr_cmds >= dcc->swod_frag_ipu_min_cmds：确实有多个 cmd 的碎片
 */
if (pend_blks < dcc->discard_granularity) {
    atomic64_inc(&sw->frag_ipu_skip_shape_cnt);
    return false;
}
```

### 删除/修改的现有代码

原有的 `swod_frag_ipu_max_pend_blks` 检查需要调整：

```c
// 原有逻辑（需要调整）：
if (!pend_blks ||
    pend_blks > dcc->swod_frag_ipu_max_pend_blks ||  // 这个条件过于严格
    nr_cmds < dcc->swod_frag_ipu_min_cmds) {
    atomic64_inc(&sw->frag_ipu_skip_shape_cnt);
    return false;
}

// 修改后：
if (!pend_blks ||
    nr_cmds < dcc->swod_frag_ipu_min_cmds) {
    atomic64_inc(&sw->frag_ipu_skip_shape_cnt);
    return false;
}
// 删除了 pend_blks > max_pend_blks 的限制，改为下面的新判断
```

### 完整的修改后代码

```c
bool f2fs_swod_should_frag_ipu(struct inode *inode,
			       struct f2fs_io_info *fio)
{
	struct f2fs_sb_info *sbi = fio->sbi;
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct swod_ctrl *sw;
	unsigned int segno, pend_blks, nr_cmds, age_thr;
	unsigned long oldest, age_ms;

	if (!dcc || !swod_enabled(dcc) || !dcc->swod_frag_ipu_enable)
		return false;
	if (!f2fs_lfs_mode(sbi))
		return false;
	if (!fio || !__is_valid_data_blkaddr(fio->old_blkaddr))
		return false;

	/*
	 * SFI 是 WCE 的辅助路径，只在系统当前真的有 held windows 时启用。
	 */
	if (!f2fs_swod_has_held(sbi))
		return false;

	sw = dcc->swod;
	segno = GET_SEGNO(sbi, fio->old_blkaddr);
	if (segno >= sw->nr_main_segs)
		return false;

	/*
	 * target window 绝不走这个辅助 IPU，避免和 WCE completion 打架。
	 */
	if (f2fs_swod_seg_held(sbi, segno)) {
		atomic64_inc(&sw->frag_ipu_skip_target_cnt);
		return false;
	}

	/*
	 * 热数据直接跳过；温/冷数据更适合这条路径。
	 */
	if (dcc->swod_frag_ipu_skip_hot &&
	    (file_is_hot(inode) || is_inode_flag_set(inode, FI_HOT_DATA))) {
		atomic64_inc(&sw->frag_ipu_skip_hot_cnt);
		return false;
	}

	/*
	 * 基于 segment 碎片化程度的判断
	 */
	pend_blks = READ_ONCE(sw->seg_hint[segno].pend_blks);
	nr_cmds = READ_ONCE(sw->seg_hint[segno].nr_cmds);

	/*
	 * 条件1：该 segment 必须有足够的碎片化
	 * pend_blks >= discard_granularity 说明该 segment 已经有一定碎片
	 */
	if (!pend_blks || pend_blks < dcc->discard_granularity) {
		atomic64_inc(&sw->frag_ipu_skip_shape_cnt);
		return false;
	}

	/*
	 * 条件2：碎片必须来自多个 cmd 的合并
	 * 否则说明该 segment 的碎片不是真正的 fragment
	 */
	if (nr_cmds < dcc->swod_frag_ipu_min_cmds) {
		atomic64_inc(&sw->frag_ipu_skip_shape_cnt);
		return false;
	}

	/*
	 * 年龄判断
	 */
	oldest = READ_ONCE(sw->seg_hint[segno].oldest_jiffies);
	if (!oldest || time_after(oldest, jiffies)) {
		atomic64_inc(&sw->frag_ipu_skip_age_cnt);
		return false;
	}

	age_ms = jiffies_to_msecs(jiffies - oldest);

	/*
	 * 冷数据更容易放行；非冷数据要更老才值得这次 IPU。
	 */
	age_thr = dcc->swod_frag_ipu_age_ms;
	if (!file_is_cold(inode))
		age_thr <<= 1;

	if (age_ms < age_thr) {
		atomic64_inc(&sw->frag_ipu_skip_age_cnt);
		return false;
	}

	atomic64_inc(&sw->frag_ipu_pick_cnt);
	return true;
}
```

## 默认参数调整

| 参数 | 原值 | 新值 | 说明 |
|------|------|------|------|
| `swod_frag_ipu_max_pend_blks` | 32 | **删除** | 不再使用硬编码阈值 |
| `swod_frag_ipu_min_cmds` | 2 | 保持 | 碎片必须来自多个 cmd |
| `swod_frag_ipu_age_ms` | 5000 | 保持 | - |

## 预期效果

| 测试 | 原设计 | 新设计 |
|------|--------|--------|
| varmail dc_create | -77% | **预期 -20% ~ -30%** |
| fileserver dc_create | -11% | 保持 |

## 实现检查清单

- [x] 修改 `swod.c` 中的 `f2fs_swod_should_frag_ipu()`
- [x] 删除 `swod_frag_ipu_max_pend_blks` 相关逻辑（可选，保留兼容性）
- [x] 编译测试
- [ ] 运行 varmail 测试验证
- [ ] 运行 fileserver 测试验证
- [ ] 对比 dc_create 变化

## 回滚方案

如果新设计效果不理想：

1. 恢复 `pend_blks > max_pend_blks` 的检查
2. 调大 `swod_frag_ipu_max_pend_blks` 的值（如 64、128）
3. 或完全关闭 SFI（`swod_frag_ipu_enable=0`）

---

# SFI 修改方案 v2

## 修改日期

2026-06-11

## 背景

### v1 问题

v1 修改后，varmail 测试中 dc_create 仍然减少 **79%**，远超预期。

### 根因分析

分析 log 文件发现：
- varmail 中 **64% 的 dc_create 是 len <= 16 blocks**
- 这些 small discard 本身就等于 `discard_granularity`
- `pend_blks >= discard_granularity (16)` 条件几乎总能满足
- `nr_cmds >= 2` 门槛也太低，大量 segment 满足

### 数据验证

| dc_create len 分布 | 数量 | 占比 |
|---------------------|------|------|
| len <= 16 | 41317 | **64%** |
| 16 < len <= 32 | 2427 | 4% |
| len > 512 | 17735 | 28% |
| 总计 | 64424 | 100% |

### SFI 设计意图澄清

用户明确指出：
- **SFI 目标是让小写请求 IPU → 合并到已有 live block → 源头产生更少、更长的 discard cmd**
- `nr_cmds` 门槛太低导致 SFI 放行太多 IPU

### 问题总结

| 参数 | 当前值 | 问题 |
|------|--------|------|
| `swod_frag_ipu_min_cmds` | **2** | 门槛太低，大量 segment 满足 |
| `swod_frag_ipu_age_ms` | **5000ms** | 较宽松 |

## 修改方案

### 修改文件

- `segment.c`：`create_discard_cmd_control()` 函数中的默认值初始化

### 参数调整

| 参数 | 原值 | 新值 | 说明 |
|------|------|------|------|
| `swod_frag_ipu_min_cmds` | 2 | **6** | 提高门槛，只对碎片化严重的 segment 启用 SFI |
| `swod_frag_ipu_age_ms` | 5000 | **8000** | 年龄阈值适当提高 |

### 代码修改

```c
// segment.c 修改前
dcc->swod_frag_ipu_min_cmds = 2; // 至少是碎的
dcc->swod_frag_ipu_age_ms = 5000;// 先只碰更老的

// segment.c 修改后
dcc->swod_frag_ipu_min_cmds = 6; // 提高门槛，只对碎片化严重的 segment 启用
dcc->swod_frag_ipu_age_ms = 8000; // 年龄阈值适当提高
```

### 预期效果

| 场景 | 原 dc_create | 原减少幅度 | 目标减少幅度 |
|------|--------------|------------|--------------|
| varmail | 64424 | -79% | **-10% ~ -20%** |
| fileserver | - | -11% | 保持或略有下降 |

## 目标说明

- 理想值：**10-30%** dc_create 减少
- 常见值：**10-20%**
- 超过 30% 视为过于激进

## 实现检查清单

- [x] 修改 `segment.c` 中的默认值
- [x] 编译测试
- [ ] 运行 varmail 测试验证
- [ ] 运行 fileserver 测试验证
- [ ] 对比 dc_create 变化

## 回滚方案

如果效果仍不理想，继续调参：
- `min_cmds: 12→16`, `age_ms: 15000→20000`
- 或将 `pend_blks >= discard_granularity * 4` 改为 `* 8`（128 blocks）

---

# SFI 修改方案 v4

## 修改日期

2026-06-12

## 背景

### v3 问题

修改 `data.c` 后，varmail 测试中 dc_create 仍然减少 **79%**。

### 根因分析

| 参数 | v2 调整后值 | 问题 |
|------|-------------|------|
| `min_cmds` | 12 | 门槛太低 |
| `age_ms` | 15000 | 年龄阈值太低 |
| `pend_blks >= discard_granularity` | 16 blocks | **门槛太低！** |

**关键发现**：
- varmail 中有 17740 个 `len=1` 的 dc（27.5%）
- 这些小 discard 分散在各 segment
- 即使 `nr_cmds` 达到阈值，`pend_blks` 也很小（每个 len=1 只有 1 block）
- **len=1 的 dc 凑够数量就能满足 SFI 条件**

### 约束条件

用户明确指出：**discard_granularity 不能有特权**

不能简单地把门槛设为 `discard_granularity`，必须明显更高才能防止 len=1 的 dc 满足条件。

## 修改方案

### 修改文件

- `swod.c`：`f2fs_swod_should_frag_ipu()` 中的 `pend_blks` 判断

### 代码修改

```c
// 修改前
if (!pend_blks || pend_blks < dcc->discard_granularity) {
    atomic64_inc(&sw->frag_ipu_skip_shape_cnt);
    return false;
}

// 修改后
if (!pend_blks || pend_blks < dcc->discard_granularity * 4) {
    atomic64_inc(&sw->frag_ipu_skip_shape_cnt);
    return false;
}
```

### 修改原因

| 场景 | len=1 dc 数量 | pend_blks | 是否满足新条件 |
|------|--------------|-----------|----------------|
| varmail | 35 个 len=1 | 35 blocks | 35 < 64，**不满足** |
| 理想情况 | 10 个大 dc | 200 blocks | 200 > 64，**满足** |

## 预期效果

| 场景 | 原减少幅度 | 预期减少幅度 |
|------|------------|--------------|
| varmail | -79% | **-10% ~ -30%** |
| fileserver | -11% | 保持或略有下降 |

## 实现检查清单

- [x] 修改 `swod.c` 中的 `pend_blks` 条件
- [x] 编译测试
- [ ] 运行 varmail 测试验证
- [ ] 运行 fileserver 测试验证
- [ ] 对比 dc_create 变化

## 回滚方案

如果效果仍不理想：
- `pend_blks >= discard_granularity * 8`（128 blocks）
- 或调整 `min_cmds: 12→20`

---

# SFI 修改方案 v3

## 修改日期

2026-06-12

## 背景

### v2 问题

v2 修改后，varmail 测试中 dc_create 仍然减少 **79%**，远超预期（目标 10-30%）。

### 根因分析

通过分析 log 文件和代码发现：

**关键发现**：varmail 负载的文件是冷数据（mail 文件），在 `f2fs_should_update_inplace()` 中：

```c
/* if this is cold file, we should overwrite to avoid fragmentation */
if (file_is_cold(inode))
    return true;  // cold 直接返回 true，SFI 没有介入机会！
```

**问题所在**：
- cold 文件直接走 IPU 路径，不经过 SFI 判断
- `swod_frag_ipu_enable=0` 在 overwrite.sh 中关闭，但不影响这个路径
- **dc_create 减少 79% 是 stock F2FS 的 cold 文件 IPU 行为，不是 SFI！**

### 数据对比

| 负载 | 文件类型 | cold 占比 | dc_create 减少 |
|------|----------|-----------|----------------|
| fileserver | 混合 | 较低 | **11.8%** |
| varmail | 主要是 cold mail | 高 | **79.4%** |

### SFI 设计意图澄清（再次强调）

用户明确指出：
- **SFI 设计要求**：cold 是条件之一，不是"判断出 cold 就原地更新"
- 还需要满足 segment 的"老、小、碎"条件
- SFI 的 IPU 例外是**有条件的**，不是 cold 文件就直接 IPU

### 问题总结

| 条件 | 原行为 | SFI 设计意图 |
|------|--------|-------------|
| cold 文件 | 直接 IPU | cold + 满足"老小碎" → IPU |
| cold + 不满足条件 | - | 应该 OPU |

## 修改方案

### 修改文件

- `data.c`：`f2fs_should_update_inplace()` 函数

### 代码修改

```c
// 修改前（data.c:2606-2620）
bool f2fs_should_update_inplace(struct inode *inode, struct f2fs_io_info *fio)
{
    /* swap file is migrating in aligned write mode */
    if (is_inode_flag_set(inode, FI_ALIGNED_WRITE))
        return false;

    if (f2fs_is_pinned_file(inode))
        return true;

    /* if this is cold file, we should overwrite to avoid fragmentation */
    if (file_is_cold(inode))
        return true;  // ← 直接返回 true，SFI 没有介入机会！

    return check_inplace_update_policy(inode, fio);
}

// 修改后（data.c:2606-2620）
bool f2fs_should_update_inplace(struct inode *inode, struct f2fs_io_info *fio)
{
    /* swap file is migrating in aligned write mode */
    if (is_inode_flag_set(inode, FI_ALIGNED_WRITE))
        return false;

    if (f2fs_is_pinned_file(inode))
        return true;

    /* if this is cold file, let SFI decide whether to IPU */
    if (file_is_cold(inode))
        return f2fs_swod_should_frag_ipu(inode, fio);  // ← SFI 判断，非直接 IPU

    return check_inplace_update_policy(inode, fio);
}
```

### 新的行为逻辑

| 条件 | 结果 |
|------|------|
| cold + 满足"老小碎"条件 | SFI 允许 → **原地更新** |
| cold + 不满足"老小碎"条件 | SFI 拒绝 → **异地更新** |
| 非 cold 文件 | 正常 OPU |

### 设计优势

1. **SFI 是唯一的判断路径**：cold 文件不再直接 IPU，统一由 SFI 判断
2. **条件化 IPU 例外**：满足 segment 碎片化条件才允许 IPU
3. **复用 SFI 已有逻辑**：已有"老小碎"判断逻辑完整保留

## 默认参数（来自 v2 调整）

| 参数 | 值 | 说明 |
|------|-----|------|
| `swod_frag_ipu_min_cmds` | **12** | 只对极度碎片化的 segment 启用 |
| `swod_frag_ipu_age_ms` | **15000** | 只碰非常老的数据 |

## 预期效果

| 场景 | 原减少幅度 | 预期减少幅度 |
|------|------------|--------------|
| varmail | -79% | **-10% ~ -30%** |
| fileserver | -11% | 保持或略有下降 |

## 实现检查清单

- [x] 修改 `data.c` 中 `f2fs_should_update_inplace()` 函数
- [x] 编译测试
- [ ] 运行 varmail 测试验证
- [ ] 运行 fileserver 测试验证
- [ ] 对比 dc_create 变化
- [ ] 检查 SFI 统计（`swod_frag_ipu_*_cnt`）

## 验证方法

### 1. 检查 SFI 开关状态
```bash
cat /sys/fs/ef2fs/nvme2n1/swod_frag_ipu_enable
```

### 2. 检查 SFI 统计
```bash
cat /sys/fs/ef2fs/nvme2n1/swod_frag_ipu_pick_cnt
cat /sys/fs/ef2fs/nvme2n1/swod_frag_ipu_skip_target_cnt
cat /sys/fs/ef2fs/nvme2n1/swod_frag_ipu_skip_hot_cnt
cat /sys/fs/ef2fs/nvme2n1/swod_frag_ipu_skip_age_cnt
cat /sys/fs/ef2fs/nvme2n1/swod_frag_ipu_skip_shape_cnt
```

### 3. 对比 dc_create
```bash
# f2fs baseline
grep "dc_create" f2fs_log | wc -l

# redisc with SFI
grep "dc_create" redisc_log | wc -l

# 计算减少幅度
```

## 回滚方案

如果效果仍不理想：

1. 恢复 cold 文件直接 IPU 的逻辑
2. 调整 SFI 参数（`min_cmds: 12→16`, `age_ms: 15000→20000`）
3. 或完全关闭 SFI（`swod_frag_ipu_enable=0`）