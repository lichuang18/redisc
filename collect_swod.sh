#!/bin/bash
BASE=${BASE:-/sys/fs/ef2fs/nvme2n1}
STATUS=${STATUS:-/sys/kernel/debug/ef2fs/status}
OUT=${OUT:-swod_metrics.csv}
INTERVAL=${INTERVAL:-1}
DURATION=${1:-60}

read_node() {
    local p="$1"
    if [ -r "$p" ]; then
        cat "$p"
    else
        printf 'NA'
    fi
}

read_back() {
    if [ -r "$STATUS" ]; then
        grep 'undiscard:' "$STATUS" | sed -n 's/.*undiscard:\([0-9]*\).*/\1/p'
    else
        printf 'NA'
    fi
}

read_mem_available_kb() {
    if [ -r /proc/meminfo ]; then
        sed -n 's/^MemAvailable:[[:space:]]*\([0-9]*\)[[:space:]]*kB$/\1/p' /proc/meminfo
    else
        printf 'NA'
    fi
}

printf '%s\n' "ts,swod_hold_cnt,swod_success_release_cnt,swod_timeout_release_cnt,swod_pressure_release_cnt,swod_held_groups,swod_skip_cnt,back,mem_available_kb,min_mem_available_kb" > "$OUT"

min_mem_available_kb=
end_ts=$(( $(date +%s) + DURATION ))

while [ "$(date +%s)" -lt "$end_ts" ]; do
    ts=$(date '+%F %T')

    hold=$(read_node "$BASE/swod_hold_cnt")
    succ=$(read_node "$BASE/swod_success_release_cnt")
    tout=$(read_node "$BASE/swod_timeout_release_cnt")
    press=$(read_node "$BASE/swod_pressure_release_cnt")
    held=$(read_node "$BASE/swod_held_groups")
    skip=$(read_node "$BASE/swod_skip_cnt")
    back=$(read_back)
    mem_available_kb=$(read_mem_available_kb)

    if [ "$mem_available_kb" != "NA" ]; then
        if [ -z "$min_mem_available_kb" ] || [ "$mem_available_kb" -lt "$min_mem_available_kb" ]; then
            min_mem_available_kb=$mem_available_kb
        fi
    fi

    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$ts" "$hold" "$succ" "$tout" "$press" "$held" "$skip" "$back" "$mem_available_kb" "${min_mem_available_kb:-NA}" >> "$OUT"
    sleep "$INTERVAL"
done

echo "done: $OUT"
