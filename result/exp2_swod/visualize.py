#!/usr/bin/env python3
"""
分析 ReDisc vs F2FS 的 discard cmd 创建数据
生成可视化图表保存到当前目录
"""

import re
import os
import matplotlib.pyplot as plt
import matplotlib

matplotlib.use('Agg')
plt.rcParams['font.size'] = 12
plt.rcParams['figure.figsize'] = (12, 6)

# 中文支持
plt.rcParams['font.sans-serif'] = ['DejaVu Sans', 'Arial', 'Liberation Sans']
plt.rcParams['axes.unicode_minus'] = False


def parse_log(filepath):
    """解析日志文件，提取 cmd 创建记录"""
    pattern = r'\[redisc_dc_create\].* len=(\d+) segno=(\d+)'
    lens = []
    with open(filepath, 'r') as f:
        for line in f:
            m = re.search(pattern, line)
            if m:
                lens.append(int(m.group(1)))
    return lens


def len_distribution(lens):
    """统计各长度区间的 CMD 数量"""
    bins = [0, 16, 32, 64, 128, 256, 512, float('inf')]
    dist = {}
    for i in range(len(bins)-1):
        if bins[i+1] == float('inf'):
            key = f"{bins[i]}+"
        else:
            key = f"{bins[i]}-{bins[i+1]}"
        dist[key] = 0

    for l in lens:
        for i in range(len(bins)-1):
            if bins[i+1] == float('inf'):
                if l >= bins[i]:
                    dist[f"{bins[i]}+"] += 1
                    break
            elif bins[i] <= l < bins[i+1]:
                dist[f"{bins[i]}-{bins[i+1]}"] += 1
                break
    return dist


def analyze_workload(f2fs_lens, redisc_lens):
    """分析单个负载的数据"""
    total_f2fs = sum(f2fs_lens)
    total_redisc = sum(redisc_lens)

    return {
        'f2fs_count': len(f2fs_lens),
        'f2fs_avg_len': total_f2fs / len(f2fs_lens) if f2fs_lens else 0,
        'f2fs_total_blocks': total_f2fs,
        'redisc_count': len(redisc_lens),
        'redisc_avg_len': total_redisc / len(redisc_lens) if redisc_lens else 0,
        'redisc_total_blocks': total_redisc,
        'count_change_pct': (len(redisc_lens) - len(f2fs_lens)) / len(f2fs_lens) * 100 if f2fs_lens else 0,
        'avg_len_change_pct': ((total_redisc / len(redisc_lens)) - (total_f2fs / len(f2fs_lens))) / (total_f2fs / len(f2fs_lens)) * 100 if f2fs_lens else 0,
    }


def plot_summary(workloads, data, output_dir):
    """生成汇总对比图"""
    fig, axes = plt.subplots(2, 2, figsize=(14, 10))
    fig.suptitle('ReDisc vs F2FS Discard CMD Analysis', fontsize=16, fontweight='bold')

    # 图1: CMD 数量对比
    ax1 = axes[0, 0]
    x = range(len(workloads))
    width = 0.35

    f2fs_counts = [data[w]['f2fs_count'] for w in workloads]
    redisc_counts = [data[w]['redisc_count'] for w in workloads]

    bars1 = ax1.bar([i - width/2 for i in x], f2fs_counts, width, label='F2FS', color='#1f77b4', alpha=0.8)
    bars2 = ax1.bar([i + width/2 for i in x], redisc_counts, width, label='ReDisc', color='#ff7f0e', alpha=0.8)

    ax1.set_xlabel('Workload')
    ax1.set_ylabel('CMD Count')
    ax1.set_title('CMD Creation Count')
    ax1.set_xticks(x)
    ax1.set_xticklabels([w.upper() for w in workloads])
    ax1.legend()
    ax1.ticklabel_format(style='scientific', axis='y', scilimits=(0, 0))

    # 在柱子上标注变化百分比
    for i, (fc, rc) in enumerate(zip(f2fs_counts, redisc_counts)):
        pct = (rc - fc) / fc * 100
        ax1.annotate(f'{pct:+.1f}%', xy=(i, max(fc, rc)),
                    ha='center', va='bottom', fontsize=10, color='red')

    # 图2: 平均长度对比
    ax2 = axes[0, 1]
    f2fs_avgs = [data[w]['f2fs_avg_len'] for w in workloads]
    redisc_avgs = [data[w]['redisc_avg_len'] for w in workloads]

    bars1 = ax2.bar([i - width/2 for i in x], f2fs_avgs, width, label='F2FS', color='#1f77b4', alpha=0.8)
    bars2 = ax2.bar([i + width/2 for i in x], redisc_avgs, width, label='ReDisc', color='#ff7f0e', alpha=0.8)

    ax2.set_xlabel('Workload')
    ax2.set_ylabel('Average CMD Length (blocks)')
    ax2.set_title('Average CMD Length')
    ax2.set_xticks(x)
    ax2.set_xticklabels([w.upper() for w in workloads])
    ax2.legend()

    for i, (fa, ra) in enumerate(zip(f2fs_avgs, redisc_avgs)):
        pct = (ra - fa) / fa * 100 if fa > 0 else 0
        ax2.annotate(f'{pct:+.1f}%', xy=(i, max(fa, ra)),
                    ha='center', va='bottom', fontsize=10, color='red')

    # 图3: CMD 数量变化百分比
    ax3 = axes[1, 0]
    changes = [data[w]['count_change_pct'] for w in workloads]
    colors = ['green' if c <= 0 else 'red' for c in changes]

    bars = ax3.bar(workloads, changes, color=colors, alpha=0.7)
    ax3.axhline(y=0, color='black', linestyle='-', linewidth=0.5)
    ax3.set_xlabel('Workload')
    ax3.set_ylabel('Change (%)')
    ax3.set_title('CMD Count Change (ReDisc vs F2FS)')

    for bar, change in zip(bars, changes):
        height = bar.get_height()
        ax3.annotate(f'{change:+.1f}%',
                    xy=(bar.get_x() + bar.get_width() / 2, height),
                    ha='center', va='bottom' if height > 0 else 'top',
                    fontsize=11, fontweight='bold')

    # 图4: 平均长度变化百分比
    ax4 = axes[1, 1]
    len_changes = [data[w]['avg_len_change_pct'] for w in workloads]
    colors = ['green' if c >= 0 else 'red' for c in len_changes]

    bars = ax4.bar(workloads, len_changes, color=colors, alpha=0.7)
    ax4.axhline(y=0, color='black', linestyle='-', linewidth=0.5)
    ax4.set_xlabel('Workload')
    ax4.set_ylabel('Change (%)')
    ax4.set_title('Average Length Change (ReDisc vs F2FS)')

    for bar, change in zip(bars, len_changes):
        height = bar.get_height()
        ax4.annotate(f'{change:+.1f}%',
                    xy=(bar.get_x() + bar.get_width() / 2, height),
                    ha='center', va='bottom' if height > 0 else 'top',
                    fontsize=11, fontweight='bold')

    plt.tight_layout()
    output_path = os.path.join(output_dir, 'summary_comparison.png')
    plt.savefig(output_path, dpi=150, bbox_inches='tight')
    plt.close()
    print(f"Saved: {output_path}")


def plot_length_distribution(workloads, all_data, output_dir):
    """生成各负载的长度分布对比图"""
    n_workloads = len(workloads)
    fig, axes = plt.subplots(1, n_workloads, figsize=(6 * n_workloads, 5))
    fig.suptitle('CMD Length Distribution (ReDisc vs F2FS)', fontsize=14, fontweight='bold')

    if n_workloads == 1:
        axes = [axes]

    for idx, wl in enumerate(workloads):
        ax = axes[idx]
        d = all_data[wl]

        fd = d['f2fs_dist']
        rd = d['redisc_dist']

        # 获取所有区间
        all_keys = list(set(fd.keys()) | set(rd.keys()))
        # 按数值排序
        def sort_key(k):
            if '+' in k:
                return float(k.replace('+', ''))
            elif '-' in k:
                return float(k.split('-')[0])
            return 0

        sorted_keys = sorted(all_keys, key=sort_key)

        x = range(len(sorted_keys))
        width = 0.4

        f2fs_vals = [fd.get(k, 0) for k in sorted_keys]
        redisc_vals = [rd.get(k, 0) for k in sorted_keys]

        ax.bar([i - width/2 for i in x], f2fs_vals, width, label='F2FS', color='#1f77b4', alpha=0.8)
        ax.bar([i + width/2 for i in x], redisc_vals, width, label='ReDisc', color='#ff7f0e', alpha=0.8)

        ax.set_xlabel('Length Range (blocks)')
        ax.set_ylabel('CMD Count')
        ax.set_title(f'{wl.upper()}')
        ax.set_xticks(x)
        ax.set_xticklabels(sorted_keys, rotation=45, ha='right', fontsize=8)
        ax.legend()
        ax.ticklabel_format(style='scientific', axis='y', scilimits=(0, 0))

    plt.tight_layout()
    output_path = os.path.join(output_dir, 'length_distribution.png')
    plt.savefig(output_path, dpi=150, bbox_inches='tight')
    plt.close()
    print(f"Saved: {output_path}")


def plot_log_scale(workloads, all_data, output_dir):
    """生成对数尺度的长度分布图（更好地展示大范围数据）"""
    n_workloads = len(workloads)
    fig, axes = plt.subplots(1, n_workloads, figsize=(6 * n_workloads, 5))
    fig.suptitle('CMD Length Distribution (Log Scale)', fontsize=14, fontweight='bold')

    if n_workloads == 1:
        axes = [axes]

    for idx, wl in enumerate(workloads):
        ax = axes[idx]
        d = all_data[wl]

        fd = d['f2fs_dist']
        rd = d['redisc_dist']

        all_keys = list(set(fd.keys()) | set(rd.keys()))

        def sort_key(k):
            if '+' in k:
                return float(k.replace('+', ''))
            elif '-' in k:
                return float(k.split('-')[0])
            return 0

        sorted_keys = sorted(all_keys, key=sort_key)
        x = range(len(sorted_keys))
        width = 0.4

        f2fs_vals = [fd.get(k, 0) + 1 for k in sorted_keys]  # +1 to avoid log(0)
        redisc_vals = [rd.get(k, 0) + 1 for k in sorted_keys]

        ax.bar([i - width/2 for i in x], f2fs_vals, width, label='F2FS', color='#1f77b4', alpha=0.8)
        ax.bar([i + width/2 for i in x], redisc_vals, width, label='ReDisc', color='#ff7f0e', alpha=0.8)

        ax.set_yscale('log')
        ax.set_xlabel('Length Range (blocks)')
        ax.set_ylabel('CMD Count (log scale)')
        ax.set_title(f'{wl.upper()}')
        ax.set_xticks(x)
        ax.set_xticklabels(sorted_keys, rotation=45, ha='right', fontsize=8)
        ax.legend()

    plt.tight_layout()
    output_path = os.path.join(output_dir, 'length_distribution_log.png')
    plt.savefig(output_path, dpi=150, bbox_inches='tight')
    plt.close()
    print(f"Saved: {output_path}")


def main():
    base_dir = '/home/lch/work/gf_discard/redisc/result/exp2_swod/'
    workloads = ['varmail', 'fileserver']

    # 解析数据
    all_data = {}
    summary_data = {}

    print("Parsing log files...")
    for wl in workloads:
        f2fs_file = os.path.join(base_dir, f'{wl}_f2fs_log')
        redisc_file = os.path.join(base_dir, f'{wl}_redisc_log')

        f2fs_lens = parse_log(f2fs_file)
        redisc_lens = parse_log(redisc_file)

        all_data[wl] = {
            'f2fs_lens': f2fs_lens,
            'redisc_lens': redisc_lens,
            'f2fs_dist': len_distribution(f2fs_lens),
            'redisc_dist': len_distribution(redisc_lens),
        }

        summary_data[wl] = analyze_workload(f2fs_lens, redisc_lens)

        print(f"  {wl}: F2FS={len(f2fs_lens)} CMDS, ReDisc={len(redisc_lens)} CMDS")

    # 生成可视化
    print("\nGenerating visualizations...")
    plot_length_distribution(workloads, all_data, base_dir)

    # 打印汇总表格
    print("\n" + "=" * 100)
    print("SUMMARY TABLE")
    print("=" * 100)
    print(f"{'Workload':<12} {'F2FS CMD':>12} {'ReDisc CMD':>12} {'Change':>10} {'F2FS AvgLen':>14} {'ReDisc AvgLen':>14} {'Change':>10}")
    print("-" * 100)
    for wl in workloads:
        d = summary_data[wl]
        print(f"{wl.upper():<12} {d['f2fs_count']:>12,} {d['redisc_count']:>12,} {d['count_change_pct']:>+9.1f}% "
              f"{d['f2fs_avg_len']:>14.1f} {d['redisc_avg_len']:>14.1f} {d['avg_len_change_pct']:>+9.1f}%")
    print("=" * 100)

    print(f"\nAll plots saved to: {base_dir}")


if __name__ == '__main__':
    main()