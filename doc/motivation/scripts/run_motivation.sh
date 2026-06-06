#!/usr/bin/env bash
# run_motivation.sh - End-to-end motivation experiment
#
# Uses the EXISTING /mnt mount (ef2fs).
# Test files are placed under /mnt/mot_test/ to avoid conflicts
# with any zombie processes stuck on /mnt.
#
# Usage: sudo ./run_motivation.sh
#
# Env vars:
#   MNT=/mnt  TESTDIR=mot_test  OUT=./results
#   RUNTIME=300  RAMP=60

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MNT="${MNT:-/mnt}"
TESTDIR="${TESTDIR:-mot_test}"
OUT="${OUT:-$SCRIPT_DIR/results}"
RUNTIME="${RUNTIME:-300}"
RAMP="${RAMP:-60}"

mkdir -p "$OUT"
mkdir -p "$MNT/$TESTDIR"

echo "[run_motivation] MNT=$MNT TESTDIR=$TESTDIR OUT=$OUT"
echo "[run_motivation] RUNTIME=$RUNTIME RAMP=$RAMP"

# Get device from mount
devbase="$(findmnt -n -o SOURCE "$MNT" | xargs basename 2>/dev/null || echo nvme2n1p1)"
echo "[run_motivation] device: $devbase"

# ── Generate fio job file ───────────────────────────────────────────────────
echo "[run_motivation] === Step 1: generate fio job ==="

cat > "$MNT/$TESTDIR/fio.job" <<JOBEOF
[global]
rw=randwrite
bs=4k
ioengine=libaio
direct=1
size=20G
runtime=${RUNTIME}
time_based=1
group_reporting=1
filename=$MNT/$TESTDIR/testfile

[job1]
numjobs=4
iodepth=32
JOBEOF

# ── Step 2: Start state sampler ─────────────────────────────────────────────
echo "[run_motivation] === Step 2: start state sampler ==="

STATE_OUT="$OUT/state.csv"
sudo bash "$SCRIPT_DIR/collect_state.sh" "$devbase" "$STATE_OUT" 0.1 &
SAMPLER_PID=$!
echo "[run_motivation] sampler PID=$SAMPLER_PID"

# ── Step 3: Enable tracepoints, start trace-cmd ─────────────────────────────
echo "[run_motivation] === Step 3: start trace-cmd ==="

TRACE_OUT="$OUT/f2fs-discard.dat"
trace-cmd reset >/dev/null 2>&1 || true
echo 1 > /sys/kernel/tracing/events/f2fs/f2fs_queue_discard/enable 2>/dev/null || true
echo 1 > /sys/kernel/tracing/events/f2fs/f2fs_issue_discard/enable 2>/dev/null || true
echo 1 > /sys/kernel/tracing/events/block/block_rq_issue/enable 2>/dev/null || true
echo 1 > /sys/kernel/tracing/events/block/block_rq_complete/enable 2>/dev/null || true

trace-cmd record -o "$TRACE_OUT" \
    -e f2fs:f2fs_queue_discard \
    -e f2fs:f2fs_issue_discard \
    -e block:block_rq_issue \
    -e block:block_rq_complete \
    > "$OUT/trace.log" 2>&1 &
TRACE_PID=$!
echo "[run_motivation] trace PID=$TRACE_PID"

# ── Step 4: Run fio (foreground) ─────────────────────────────────────────────
echo "[run_motivation] === Step 4: run fio workload (${RUNTIME}s) ==="

fio "$MNT/$TESTDIR/fio.job" \
    --output="$OUT/fio.json" \
    --output-format=json \
    --write_bw_log="$OUT/fio_bw" \
    --write_lat_log="$OUT/fio_lat"

# ── Step 5: Stop trace and sampler ──────────────────────────────────────────
echo "[run_motivation] === Step 5: stop trace + sampler ==="

kill -INT $TRACE_PID 2>/dev/null || true
wait $TRACE_PID 2>/dev/null || true
trace-cmd report "$TRACE_OUT" > "$OUT/f2fs-discard.trace" 2>/dev/null

echo 0 > /sys/kernel/tracing/events/f2fs/f2fs_queue_discard/enable 2>/dev/null || true
echo 0 > /sys/kernel/tracing/events/f2fs/f2fs_issue_discard/enable 2>/dev/null || true
echo 0 > /sys/kernel/tracing/events/block/block_rq_issue/enable 2>/dev/null || true
echo 0 > /sys/kernel/tracing/events/block/block_rq_complete/enable 2>/dev/null || true

kill $SAMPLER_PID 2>/dev/null || true
wait $SAMPLER_PID 2>/dev/null || true

# ── Step 6: Save lifetime write, clean test files ───────────────────────────
echo "[run_motivation] === Step 6: save counters, cleanup ==="

life_before=$(cat "/sys/fs/ef2fs/$devbase/lifetime_write_kbytes" 2>/dev/null || echo 0)
echo "$life_before" > "$OUT/life_before_kb.txt"
echo "$life_after_placeholder" > "$OUT/life_after_kb.txt"

# Remove test files (don't rm -f since zombie processes might block)
rm -rf "$MNT/$TESTDIR/testfile" "$MNT/$TESTDIR/fio.job" 2>/dev/null || true
rmdir "$MNT/$TESTDIR" 2>/dev/null || true

echo "[run_motivation] done. results in $OUT"
echo "[run_motivation] next:"
echo "  python3 $SCRIPT_DIR/parse_trace.py $OUT/f2fs-discard.trace $OUT/"
echo "  python3 $SCRIPT_DIR/analyze_motivation.py $OUT/ $RAMP"