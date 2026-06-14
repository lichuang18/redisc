# SFI 修改方案

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

## 预期效果

| 测试 | 原设计 | 新设计 |
|------|--------|--------|
| varmail dc_create | -77% | **预期 -20% ~ -30%** |
| fileserver dc_create | -11% | 保持 |

## 实现检查清单

- [ ] 修改 `swod.c` 中的 `f2fs_swod_should_frag_ipu()`
- [ ] 删除 `swod_frag_ipu_max_pend_blks` 相关逻辑（可选，保留兼容性）
- [ ] 编译测试
- [ ] 运行 varmail 测试验证
- [ ] 运行 fileserver 测试验证
- [ ] 对比 dc_create 变化

## 回滚方案

如果新设计效果不理想：

1. 恢复 `pend_blks > max_pend_blks` 的检查
2. 调大 `swod_frag_ipu_max_pend_blks` 的值（如 64、128）
3. 或完全关闭 SFI（`swod_frag_ipu_enable=0`）
