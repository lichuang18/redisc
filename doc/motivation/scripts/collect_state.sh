#!/usr/bin/env bash
# collect_state.sh - Poll ef2fs sysfs state at regular interval
# Reads from /sys/fs/ef2fs/ (ef2fs module sysfs).
# Outputs: state.csv
# Usage: sudo ./collect_state.sh <DEV_NAME> <OUT_CSV> [INTERVAL_SEC]
#
# Note: ef2fs (out-of-tree F2FS) doesn't export stat/queued_discard,
# stat/issued_discard, stat/undiscard_blks. We collect what's available:
# dirty_segments, free_segments, avg_vblocks, gc counters, lifetime_write.

set -euo pipefail

DEV="${1:-nvme2n1p1}"
OUT="${2:-./results/state.csv}"
INTERVAL="${3:-0.1}"

read_ef2fs() {
    cat "/sys/fs/ef2fs/$DEV/$1" 2>/dev/null || echo -1
}

mkdir -p "$(dirname "$OUT")"

# Columns: ts_ns, dirty_segs, free_segs, avg_vblocks, lifetime_kb,
#          gc_bg, gc_fg, moved_bg, moved_fg, gc_urgent, unusable_blks
echo "ts_ns,dirty_segments,free_segments,avg_vblocks,lifetime_kb,gc_bg,gc_fg,moved_bg,moved_fg,gc_urgent,unusable_blks" > "$OUT"

while true; do
    ts=$(date +%s%N)
    ds=$(read_ef2fs "dirty_segments")
    fs=$(read_ef2fs "free_segments")
    av=$(read_ef2fs "avg_vblocks")
    lw=$(read_ef2fs "lifetime_write_kbytes")
    gbg=$(read_ef2fs "gc_background_calls")
    gfg=$(read_ef2fs "gc_foreground_calls")
    mb=$(read_ef2fs "moved_blocks_background")
    mf=$(read_ef2fs "moved_blocks_foreground")
    gu=$(read_ef2fs "gc_urgent")
    ub=$(read_ef2fs "unusable")

    echo "$ts,$ds,$fs,$av,$lw,$gbg,$gfg,$mb,$mf,$gu,$ub" >> "$OUT"
    sleep "$INTERVAL"
done