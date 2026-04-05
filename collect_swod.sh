#!/bin/bash
BASE=/sys/fs/ef2fs/nvme2n1
OUT=swod_metrics.csv
INTERVAL=1
DURATION=${1:-60}

echo "ts,swod_held_groups,swod_skip_cnt,swod_hold_cnt,swod_success_release_cnt" > "$OUT"

end_ts=$(( $(date +%s) + DURATION ))

while [ "$(date +%s)" -lt "$end_ts" ]; do
    ts=$(date '+%F %T')

    held=$(cat "$BASE/swod_held_groups")
    skip=$(cat "$BASE/swod_skip_cnt")
    hold=$(cat "$BASE/swod_hold_cnt")
    succ=$(cat "$BASE/swod_success_release_cnt")
    # tout=$(cat "$BASE/swod_timeout_release_cnt")
    # press=$(cat "$BASE/swod_pressure_release_cnt")
    # skip_check=$(cat "$BASE/swod_skip_check_cnt")
    # skip_noheld=$(cat "$BASE/swod_skip_miss_noheld_cnt")
    # skip_timeout=$(cat "$BASE/swod_skip_miss_timeout_cnt")
    # skip_success=$(cat "$BASE/swod_skip_miss_success_cnt")
    # skip_overlap=$(cat "$BASE/swod_skip_miss_overlap_cnt")
    # eval_blocked=$(cat "$BASE/swod_eval_blocked_cnt")
    # eval_no_candidate=$(cat "$BASE/swod_eval_no_candidate_cnt")

    echo "${ts},${held},${skip},${hold},${succ}" >> "$OUT"
    sleep "$INTERVAL"
done

echo "done: $OUT"
