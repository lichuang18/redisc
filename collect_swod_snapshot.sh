#!/bin/bash
# SWOD snapshot 采集脚本 - 用于验证 SWOD 模块有效性
# 用法: ./collect_swod_snapshot.sh [间隔秒数，默认1]
#
# 输出文件: ./swod_snapshot_YYYYMMDD_HHMMSS.log
# 按 Ctrl+C 停止

DEV=nvme2n1p1
INTERVAL=${1:-1}
LOGFILE="./swod_snapshot_$(date '+%Y%m%d_%H%M%S').log"

echo "logging to $LOGFILE"
echo "# $(date)" > "$LOGFILE"

while sleep "$INTERVAL"; do
    echo "$(date '+%H:%M:%S') \
held_groups=$(cat /sys/fs/ef2fs/$DEV/swod_held_groups) \
hold_cnt=$(cat /sys/fs/ef2fs/$DEV/swod_hold_cnt) \
skip_cnt=$(cat /sys/fs/ef2fs/$DEV/swod_skip_cnt) \
skip_check=$(cat /sys/fs/ef2fs/$DEV/swod_skip_check_cnt) \
success=$(cat /sys/fs/ef2fs/$DEV/swod_success_release_cnt) \
timeout=$(cat /sys/fs/ef2fs/$DEV/swod_timeout_release_cnt) \
pressure=$(cat /sys/fs/ef2fs/$DEV/swod_pressure_release_cnt) \
miss_noheld=$(cat /sys/fs/ef2fs/$DEV/swod_skip_miss_noheld_cnt) \
miss_timeout=$(cat /sys/fs/ef2fs/$DEV/swod_skip_miss_timeout_cnt) \
miss_success=$(cat /sys/fs/ef2fs/$DEV/swod_skip_miss_success_cnt) \
miss_overlap=$(cat /sys/fs/ef2fs/$DEV/swod_skip_miss_overlap_cnt) \
overlap_bypass=$(cat /sys/fs/ef2fs/$DEV/swod_overlap_bypass_cnt) \
eval_blocked=$(cat /sys/fs/ef2fs/$DEV/swod_eval_blocked_cnt) \
eval_nocand=$(cat /sys/fs/ef2fs/$DEV/swod_eval_no_candidate_cnt)" >> "$LOGFILE"
done