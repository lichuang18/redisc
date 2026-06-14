#!/usr/bin/env python3
"""
ReDisc 测试结果可视化脚本
生成核心指标对比图（归一化，以 F2FS=1 为基准）
"""

import matplotlib.pyplot as plt
import matplotlib
matplotlib.use('Agg')

plt.rcParams['font.sans-serif'] = ['DejaVu Sans', 'Arial', 'Helvetica']
plt.rcParams['axes.unicode_minus'] = False

# 测试数据
workloads = ['varmail\n16KB', 'varmail\n512KB', 'fileserver\n16KB', 'fileserver\n512KB']

# 原始数据
f2fs_create = [64424, 17578, 248540, 2117]
redisc_create = [14457, 3288, 219288, 2012]

f2fs_avg = [12.11, 42.0, 0.31, 20.8]
redisc_avg = [52.23, 45.6, 0.31, 22.0]

# 归一化（F2FS=1, ReDisc 相对于 F2FS 的比例）
norm_f2fs_create = [1.0] * 4
norm_redisc_create = [r / f for r, f in zip(redisc_create, f2fs_create)]

norm_f2fs_avg = [1.0] * 4
norm_redisc_avg = [r / f for r, f in zip(redisc_avg, f2fs_avg)]

# 计算变化百分比用于标注
def pct_change(new, old):
    return f'{(new - old) / old * 100:+.0f}%'

fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 6))

x = range(len(workloads))
width = 0.35

# ==================== 左侧：DC Create Count (Normalized) ====================
bars1 = ax1.bar([i - width/2 for i in x], norm_f2fs_create, width,
                label='F2FS', color='#ff7f0e', alpha=0.8, edgecolor='white')
bars2 = ax1.bar([i + width/2 for i in x], norm_redisc_create, width,
                label='ReDisc', color='#1f77b4', alpha=0.8, edgecolor='white')

# 数值标签（显示原始值和百分比变化）
for i, (b1, b2, orig_f2fs, orig_red) in enumerate(zip(bars1, bars2, f2fs_create, redisc_create)):
    # F2FS 标签
    ax1.annotate(f'{orig_f2fs:,}',
                xy=(b1.get_x() + b1.get_width() / 2, 0.05),
                xytext=(0, 0), textcoords="offset points",
                ha='center', va='bottom', fontsize=9, fontweight='bold',
                color='#cc7000')
    # ReDisc 标签
    ax1.annotate(f'{orig_red:,}',
                xy=(b2.get_x() + b2.get_width() / 2, b2.get_height() + 0.05),
                xytext=(0, 0), textcoords="offset points",
                ha='center', va='bottom', fontsize=9, fontweight='bold',
                color='#1565a0')
    # 百分比变化
    pct = pct_change(orig_red, orig_f2fs)
    ax1.annotate(pct,
                xy=(i, max(norm_redisc_create[i], 1.0) + 0.08),
                ha='center', va='bottom', fontsize=10, color='green', fontweight='bold')

ax1.set_ylabel('Normalized DC Create Count (F2FS=1)', fontsize=11)
ax1.set_title('DC Create Count\n(F2FS=1, Lower is Better)', fontsize=13, fontweight='bold')
ax1.set_xticks(x)
ax1.set_xticklabels(workloads, fontsize=10)
ax1.legend(fontsize=10, loc='upper right')
ax1.yaxis.grid(True, linestyle='--', alpha=0.4)
ax1.set_axisbelow(True)
ax1.set_ylim(0, 1.5)
ax1.axhline(y=1.0, color='gray', linestyle='--', linewidth=1.5, alpha=0.7)

# ==================== 右侧：Avg Blocks per DC (Normalized) ====================
bars3 = ax2.bar([i - width/2 for i in x], norm_f2fs_avg, width,
                label='F2FS', color='#ff7f0e', alpha=0.8, edgecolor='white')
bars4 = ax2.bar([i + width/2 for i in x], norm_redisc_avg, width,
                label='ReDisc', color='#1f77b4', alpha=0.8, edgecolor='white')

# 数值标签（显示原始值和倍数变化）
for i, (b3, b4, orig_f2fs, orig_red) in enumerate(zip(bars3, bars4, f2fs_avg, redisc_avg)):
    # F2FS 标签
    ax2.annotate(f'{orig_f2fs:.1f}',
                xy=(b3.get_x() + b3.get_width() / 2, 0.05),
                xytext=(0, 0), textcoords="offset points",
                ha='center', va='bottom', fontsize=9, fontweight='bold',
                color='#cc7000')
    # ReDisc 标签
    ax2.annotate(f'{orig_red:.1f}',
                xy=(b4.get_x() + b4.get_width() / 2, b4.get_height() + 0.05),
                xytext=(0, 0), textcoords="offset points",
                ha='center', va='bottom', fontsize=9, fontweight='bold',
                color='#1565a0')
    # 倍数变化
    ratio = orig_red / orig_f2fs
    ax2.annotate(f'x{ratio:.1f}',
                xy=(i, max(norm_redisc_avg[i], 1.0) + 0.05),
                ha='center', va='bottom', fontsize=10, color='green', fontweight='bold')

ax2.set_ylabel('Normalized Avg Blocks per DC (F2FS=1)', fontsize=11)
ax2.set_title('Average Blocks per DC\n(F2FS=1, Higher is Better)', fontsize=13, fontweight='bold')
ax2.set_xticks(x)
ax2.set_xticklabels(workloads, fontsize=10)
ax2.legend(fontsize=10, loc='upper right')
ax2.yaxis.grid(True, linestyle='--', alpha=0.4)
ax2.set_axisbelow(True)
ax2.set_ylim(0, 5)
ax2.axhline(y=1.0, color='gray', linestyle='--', linewidth=1.5, alpha=0.7)

# ==================== 总标题 ====================
fig.suptitle('ReDisc vs F2FS: Normalized Comparison (F2FS=1)\nVarmail & Fileserver Workloads',
            fontsize=14, fontweight='bold', y=1.02)

plt.tight_layout()

output_path = '/home/lch/work/gf_discard/redisc/doc/redisc_comparison.png'
plt.savefig(output_path, dpi=150, bbox_inches='tight')
print(f"Saved to: {output_path}")