#!/bin/bash
#
# 收集 SWOD 核心指标（5个）+ undiscard
# 用法: ./collect_swod_5.sh [duration_seconds]
#

BASE=${BASE:-/sys/fs/ef2fs/nvme2n1}
STATUS=${STATUS:-/sys/kernel/debug/ef2fs/status}
OUT=${OUT:-swod_metrics_5.csv}
DURATION=${1:-60}

read_node() {
    local p="$1"
    if [ -r "$p" ]; then
        cat "$p"
    else
        printf 'NA'
    fi
}

read_undiscard() {
    if [ -r "$STATUS" ]; then
        grep 'undiscard:' "$STATUS" | sed -n 's/.*undiscard:\([0-9]*\).*/\1/p'
    else
        printf 'NA'
    fi
}

# CSV header
printf '%s\n' "ts,swod_hold_cnt,swod_success_release_cnt,swod_timeout_release_cnt,swod_pressure_release_cnt,undiscard_blks" > "$OUT"

end_ts=$(( $(date +%s) + DURATION ))

while [ "$(date +%s)" -lt "$end_ts" ]; do
    ts=$(date '+%F %T')

    hold=$(read_node "$BASE/swod_hold_cnt")
    succ=$(read_node "$BASE/swod_success_release_cnt")
    tout=$(read_node "$BASE/swod_timeout_release_cnt")
    press=$(read_node "$BASE/swod_pressure_release_cnt")
    undiscard=$(read_undiscard)

    printf '%s,%s,%s,%s,%s,%s\n' \
        "$ts" "$hold" "$succ" "$tout" "$press" "$undiscard" >> "$OUT"

    sleep 1
done

echo "done: $OUT"