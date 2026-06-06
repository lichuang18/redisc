#!/usr/bin/env bash
set -euo pipefail

# =========================
# Basic configuration
# =========================
DEV="/dev/nvme2n1p1"
DEV_NAME="nvme2n1p1"
MNT="/mnt"
F2FS_SYS="/sys/fs/f2fs/${DEV_NAME}"

UTIL=70
RUNTIME=300
DRAIN=100
NUMJOBS=4
IODEPTH=32
BS="4k"

# 0: only run default stock F2FS
# 1: run default + discard_granularity=1 sensitivity
DO_GRAN1=0

OUTDIR="./f2fs_fig2_traces"
mkdir -p "$OUTDIR"

# =========================
# Safety check
# =========================
echo "WARNING: This script will format ${DEV} and erase all data on it."
echo "Mount point: ${MNT}"
read -p "Type YES to continue: " ans
if [[ "$ans" != "YES" ]]; then
    echo "Abort."
    exit 1
fi

if [[ "$(id -u)" -ne 0 ]]; then
    echo "Please run as root."
    exit 1
fi

# =========================
# Tool and tracepoint check
# =========================
for cmd in mkfs.f2fs fio trace-cmd; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
        echo "Missing command: $cmd"
        exit 1
    fi
done

echo "[Check] Available F2FS discard tracepoints:"
# Cache the event list once. Do not pipe trace-cmd directly into `grep -q`
# under `set -o pipefail`: when grep exits early after a match, trace-cmd may
# receive SIGPIPE and the whole pipeline can be reported as failed.
EVENT_LIST="$(trace-cmd list -e)"

echo "$EVENT_LIST" | grep f2fs | grep -i discard || true

for ev in \
    "f2fs:f2fs_queue_discard" \
    "f2fs:f2fs_issue_discard" \
    "f2fs:f2fs_remove_discard"
do
    if ! grep -Fxq "$ev" <<< "$EVENT_LIST"; then
        echo "Missing trace event: ${ev}"
        exit 1
    fi
done


echo "please clear dmesg..."
sleep 5
# =========================
# Function: one experiment run
# =========================
run_one() {
    local TAG="$1"
    local SET_GRAN="$2"

    local RUN="fig2_${TAG}_u${UTIL}_r1"
    local DAT="${OUTDIR}/${RUN}.dat"
    local TXT="${OUTDIR}/${RUN}.txt"
    local LOG="${OUTDIR}/${RUN}.log"
    local PARAM="${OUTDIR}/${RUN}_params.txt"

    echo
    echo "============================================================"
    echo "[Run] ${RUN}"
    echo "============================================================"

    echo "[1] Format and mount stock F2FS"
    umount "$MNT" 2>/dev/null || true
    mkdir -p "$MNT"

    mkfs.f2fs -f "$DEV" | tee "$LOG"
    mount -t f2fs -o discard "$DEV" "$MNT"

    echo "[2] F2FS sysfs path: ${F2FS_SYS}"
    if [[ ! -d "$F2FS_SYS" ]]; then
        echo "Cannot find ${F2FS_SYS}"
        echo "Check /sys/fs/f2fs/"
        ls /sys/fs/f2fs || true
        exit 1
    fi

    if [[ -n "$SET_GRAN" ]]; then
        echo "[3] Set discard_granularity=${SET_GRAN}"
        echo "$SET_GRAN" > "${F2FS_SYS}/discard_granularity"
    else
        echo "[3] Keep default discard_granularity"
    fi

    echo "10" > "${F2FS_SYS}/cp_interval"

    echo "[4] Record F2FS parameters"
    {
        echo "DEV=${DEV}"
        echo "MNT=${MNT}"
        echo "UTIL=${UTIL}"
        echo "RUNTIME=${RUNTIME}"
        echo "DRAIN=${DRAIN}"
        echo "NUMJOBS=${NUMJOBS}"
        echo "IODEPTH=${IODEPTH}"
        echo "BS=${BS}"
        echo
        echo "mount:"
        mount | grep "$DEV" || true
        echo
        echo "discard_granularity:"
        cat "${F2FS_SYS}/discard_granularity" 2>/dev/null || true
        echo
        echo "max_small_discards:"
        cat "${F2FS_SYS}/max_small_discards" 2>/dev/null || true
        echo
        echo "max_discard_request:"
        cat "${F2FS_SYS}/max_discard_request" 2>/dev/null || true
        echo
        echo "pending_discard:"
        cat "${F2FS_SYS}/pending_discard" 2>/dev/null || true
        echo
        echo "discard_io_aware:"
        cat "${F2FS_SYS}/discard_io_aware" 2>/dev/null || true
        echo
        echo "discard_io_aware_gran:"
        cat "${F2FS_SYS}/discard_io_aware_gran" 2>/dev/null || true
        echo
        echo "min/mid/max_discard_issue_time:"
        cat "${F2FS_SYS}/min_discard_issue_time" 2>/dev/null || true
        cat "${F2FS_SYS}/mid_discard_issue_time" 2>/dev/null || true
        cat "${F2FS_SYS}/max_discard_issue_time" 2>/dev/null || true
    } | tee "$PARAM"

    echo "[5] Prefill filesystem to ${UTIL}%"
    local FS_BYTES
    local FILL_BYTES
    FS_BYTES=$(df -B1 --output=size "$MNT" | tail -1 | tr -d ' ')
    FILL_BYTES=$((FS_BYTES * UTIL / 100))

    echo "FS_BYTES=${FS_BYTES}"
    echo "FILL_BYTES=${FILL_BYTES}"

    fio --name=prefill \
        --filename="${MNT}/prefill.dat" \
        --rw=write \
        --bs=1M \
        --size="${FILL_BYTES}" \
        --direct=1 \
        --ioengine=libaio \
        --iodepth=32 \
        --numjobs=1 \
        --end_fsync=1 \
        --group_reporting | tee -a "$LOG"

    sync
    df -h "$MNT" | tee -a "$LOG"

    echo "[6] Start trace-cmd and run update workload"
    echo "Trace output: ${DAT}"
    rm -f "$DAT" "$TXT"

    trace-cmd record \
        -e f2fs:f2fs_queue_discard \
        -e f2fs:f2fs_issue_discard \
        -e f2fs:f2fs_remove_discard \
        -o "$DAT" \
        -- bash -c "
            fio --name=randupdate \
                --filename=${MNT}/prefill.dat \
                --rw=randwrite \
                --bs=${BS} \
                --size=${FILL_BYTES} \
                --iodepth=${IODEPTH} \
                --numjobs=${NUMJOBS} \
                --runtime=${RUNTIME} \
                --time_based \
                --direct=1 \
                --ioengine=libaio \
                --fsync=1000 \
                --group_reporting;
            sync;
            sleep ${DRAIN}
        " | tee -a "$LOG"

    echo "[7] Export trace text"
    trace-cmd report "$DAT" > "$TXT"

    echo "[8] Quick event counts"
    {
        echo "queue_discard count:"
        grep -c "f2fs_queue_discard" "$TXT" || true
        echo "issue_discard count:"
        grep -c "f2fs_issue_discard" "$TXT" || true
        echo "remove_discard count:"
        grep -c "f2fs_remove_discard" "$TXT" || true
        # echo "block rq issue discard-like lines:"
        # grep "block_rq_issue" "$TXT" | grep -c " D" || true
        # echo "block rq complete discard-like lines:"
        # grep "block_rq_complete" "$TXT" | grep -c " D" || true
        echo
        echo "First f2fs_queue_discard lines:"
        grep "f2fs_queue_discard" "$TXT" | head -5 || true
        echo
        echo "First f2fs_issue_discard lines:"
        grep "f2fs_issue_discard" "$TXT" | head -5 || true
    } | tee "${OUTDIR}/${RUN}_summary.txt"

    echo "[Done] ${RUN}"
    echo "Data:"
    echo "  ${DAT}"
    echo "  ${TXT}"
    echo "  ${PARAM}"
    echo "  ${OUTDIR}/${RUN}_summary.txt"

    umount "$MNT" 2>/dev/null || true
}

# =========================
# Run default stock F2FS
# =========================
run_one "default_gran16" ""

# =========================
# Optional sensitivity: discard_granularity=1
# =========================
if [[ "$DO_GRAN1" -eq 1 ]]; then
    run_one "gran1" "1"
fi

echo
echo "All done. Output directory: ${OUTDIR}"
