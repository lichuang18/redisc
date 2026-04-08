#!/bin/bash
BASE=${BASE:-/sys/fs/ef2fs/nvme2n1}
OUT=${OUT:-wce_metrics.csv}
INTERVAL=${INTERVAL:-1}
DURATION=${1:-60}

printf '%s\n' "ts,swod_held_groups,swod_hold_cnt,swod_skip_check_cnt,swod_skip_cnt,swod_success_release_cnt,swod_timeout_release_cnt,swod_pressure_release_cnt,swod_gc_pick_bg_cnt,swod_gc_pick_fg_cnt,swod_gc_fallback_cnt,gc_background_calls,moved_blocks_background,moved_blocks_foreground,avg_vblocks,dirty_segments,free_segments,ovp_segments,gc_urgent,gc_idle" > "$OUT"

read_node() {
    local p="$1"
    if [ -r "$p" ]; then
        cat "$p"
    else
        printf 'NA'
    fi
}

end_ts=$(( $(date +%s) + DURATION ))

while [ "$(date +%s)" -lt "$end_ts" ]; do
    ts=$(date '+%F %T')

    held=$(read_node "$BASE/swod_held_groups")
    hold=$(read_node "$BASE/swod_hold_cnt")
    skip_check=$(read_node "$BASE/swod_skip_check_cnt")
    skip=$(read_node "$BASE/swod_skip_cnt")
    succ=$(read_node "$BASE/swod_success_release_cnt")
    tout=$(read_node "$BASE/swod_timeout_release_cnt")
    press=$(read_node "$BASE/swod_pressure_release_cnt")

    pick_bg=$(read_node "$BASE/swod_gc_pick_bg_cnt")
    pick_fg=$(read_node "$BASE/swod_gc_pick_fg_cnt")
    fallback=$(read_node "$BASE/swod_gc_fallback_cnt")

    bggc=$(read_node "$BASE/gc_background_calls")
    moved_bg=$(read_node "$BASE/moved_blocks_background")
    moved_fg=$(read_node "$BASE/moved_blocks_foreground")
    avg_vblocks=$(read_node "$BASE/avg_vblocks")
    dirty=$(read_node "$BASE/dirty_segments")
    free=$(read_node "$BASE/free_segments")
    ovp=$(read_node "$BASE/ovp_segments")
    gc_urgent=$(read_node "$BASE/gc_urgent")
    gc_idle=$(read_node "$BASE/gc_idle")

    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$ts" "$held" "$hold" "$skip_check" "$skip" "$succ" "$tout" "$press" \
        "$pick_bg" "$pick_fg" "$fallback" \
        "$bggc" "$moved_bg" "$moved_fg" "$avg_vblocks" \
        "$dirty" "$free" "$ovp" "$gc_urgent" "$gc_idle" >> "$OUT"

    sleep "$INTERVAL"
done

echo "done: $OUT"
