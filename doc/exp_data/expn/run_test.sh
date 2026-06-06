#!/usr/bin/env bash
# Experiment 1: End-to-End Performance for ReDisc
#
# Configs:
#   f2fs16, redisc16, f2fs512, redisc512
#
# Workloads:
#   fileserver, varmail
#
# WARNING:
#   This script formats DEV for every run.
#   Check DEV carefully before running.

set -euo pipefail

############################
# Edit here
############################

DEV="/dev/nvme2n1p1"
MNT="/mnt"
RESULT_ROOT="$PWD/redisc_exp1_results"

REPEAT=3

FILESERVER_PROFILE="/usr/share/filebench/workloads/fileserver.f"
VARMAIL_PROFILE="/usr/share/filebench/workloads/varmail.f"

DO_BLKDISCARD=0

CONFIGS=(f2fs16 redisc16 f2fs512 redisc512)
WORKLOADS=(fileserver varmail)

CURRENT_LOG="/dev/null"
MON_PIDS=()

log() {
    echo "[$(date '+%F %T')] $*" | tee -a "$CURRENT_LOG"
}

require_root() {
    if [[ "$(id -u)" -ne 0 ]]; then
        echo "ERROR: run as root." >&2
        exit 1
    fi
}

confirm_destructive() {
    echo "============================================================"
    echo "DANGER: this script will format the device repeatedly:"
    echo "  DEV = $DEV"
    echo "  MNT = $MNT"
    echo
    echo "Results will be saved to:"
    echo "  RESULT_ROOT = $RESULT_ROOT"
    echo "============================================================"
    read -r -p "Type YES to continue: " ans

    if [[ "$ans" != "YES" ]]; then
        echo "Abort."
        exit 1
    fi
}

need_cmd() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "ERROR: missing command: $1" >&2
        exit 1
    }
}

check_result_dir() {
    mkdir -p "$RESULT_ROOT"

    local result_real mount_real
    result_real="$(realpath -m "$RESULT_ROOT")"
    mount_real="$(realpath -m "$MNT")"

    case "$result_real/" in
        "$mount_real"/*)
            echo "ERROR: RESULT_ROOT is inside the tested F2FS mount point." >&2
            echo "  RESULT_ROOT = $result_real" >&2
            echo "  MNT         = $mount_real" >&2
            echo "Please put RESULT_ROOT on another filesystem to avoid extra I/O interference." >&2
            exit 1
            ;;
    esac
}

write_sysfs() {
    local file="$1"
    local val="$2"

    if [[ -e "$file" ]]; then
        echo "$val" > "$file"
        echo "sysfs: $(basename "$file") <= $val" >> "$CURRENT_LOG"
    else
        echo "sysfs: skip missing $file" >> "$CURRENT_LOG"
    fi
}

resolve_f2fs_sysfs() {
    local dev_name
    dev_name="$(basename "$(readlink -f "$DEV")")"

    if [[ -d "/sys/fs/f2fs/$dev_name" ]]; then
        echo "/sys/fs/f2fs/$dev_name"
        return
    fi

    local dirs=()
    local d

    for d in /sys/fs/f2fs/*; do
        [[ -d "$d" ]] && dirs+=("$d")
    done

    if [[ "${#dirs[@]}" -eq 1 ]]; then
        echo "${dirs[0]}"
        return
    fi

    echo "ERROR: cannot resolve /sys/fs/f2fs path for $DEV." >&2
    echo "Check whether the F2FS partition is mounted." >&2
    exit 1
}

set_config() {
    local cfg="$1"
    local sys="$2"

    write_sysfs "$sys/gc_urgent" 0

    write_sysfs "$sys/swod_enable" 0
    write_sysfs "$sys/swod_completion_enable" 0
    write_sysfs "$sys/swod_frag_ipu_enable" 0

    case "$cfg" in
        f2fs16)
            write_sysfs "$sys/discard_granularity" 16
            ;;
        redisc16)
            write_sysfs "$sys/discard_granularity" 16
            write_sysfs "$sys/swod_enable" 1
            write_sysfs "$sys/swod_completion_enable" 1
            write_sysfs "$sys/swod_frag_ipu_enable" 1
            ;;
        f2fs512)
            write_sysfs "$sys/discard_granularity" 512
            ;;
        redisc512)
            write_sysfs "$sys/discard_granularity" 512
            write_sysfs "$sys/swod_enable" 1
            write_sysfs "$sys/swod_completion_enable" 1
            write_sysfs "$sys/swod_frag_ipu_enable" 1
            ;;
        *)
            echo "ERROR: unknown config: $cfg" >&2
            exit 1
            ;;
    esac
}

format_and_mount() {
    if mountpoint -q "$MNT"; then
        log "umount $MNT"
        umount "$MNT"
    fi

    if [[ "$DO_BLKDISCARD" -eq 1 ]]; then
        log "blkdiscard $DEV"
        blkdiscard -f "$DEV" || true
    fi

    log "mkfs.f2fs $DEV"
    mkfs.f2fs -f "$DEV" >> "$CURRENT_LOG" 2>&1

    log "mount $DEV $MNT"
    mount -t f2fs "$DEV" "$MNT"
}

clear_runtime_state() {
    dmesg -C || true
    sync
    echo 3 > /proc/sys/vm/drop_caches || true
}

collect_sysfs_snapshot() {
    local sys="$1"
    local out="$2"

    mkdir -p "$out"

    {
        echo "# timestamp: $(date '+%F %T')"
        echo "# sysfs path: $sys"
        echo
    } > "$out/sysfs_values.txt"

    if [[ -d "$sys" ]]; then
        find "$sys" -maxdepth 1 -type f | sort | while read -r f; do
            {
                echo "--- $(basename "$f") ---"
                timeout 2 cat "$f" 2>/dev/null || true
                echo
            } >> "$out/sysfs_values.txt"
        done
    fi

    df -h "$MNT" > "$out/df_h.txt" 2>/dev/null || true
    df -B1 "$MNT" > "$out/df_B1.txt" 2>/dev/null || true
}

start_monitors() {
    local out="$1"
    MON_PIDS=()

    if command -v iostat >/dev/null 2>&1; then
        iostat -x 1 > "$out/iostat_x_1s.log" 2>&1 &
        MON_PIDS+=("$!")
    fi

    if command -v vmstat >/dev/null 2>&1; then
        vmstat 1 > "$out/vmstat_1s.log" 2>&1 &
        MON_PIDS+=("$!")
    fi

    if command -v pidstat >/dev/null 2>&1; then
        pidstat -dur 1 > "$out/pidstat_1s.log" 2>&1 &
        MON_PIDS+=("$!")
    fi
}

stop_monitors() {
    local pid

    for pid in "${MON_PIDS[@]:-}"; do
        kill "$pid" 2>/dev/null || true
    done

    wait 2>/dev/null || true
    MON_PIDS=()
}

prepare_filebench_profile() {
    local src="$1"
    local workload="$2"
    local outdir="$3"
    local dst="$outdir/${workload}.f"

    [[ -f "$src" ]] || {
        echo "ERROR: filebench profile not found: $src" >&2
        exit 1
    }

    cp "$src" "$dst"
    echo "$dst"
}

run_filebench_workload() {
    local workload="$1"
    local outdir="$2"
    local src profile

    case "$workload" in
        fileserver)
            src="$FILESERVER_PROFILE"
            ;;
        varmail)
            src="$VARMAIL_PROFILE"
            ;;
        *)
            echo "ERROR: not a filebench workload: $workload" >&2
            exit 1
            ;;
    esac

    profile=$(prepare_filebench_profile "$src" "$workload" "$outdir")

    log "run filebench: workload=$workload profile=$profile"

    /usr/bin/time -v filebench -f "$profile" \
        > "$outdir/workload.out" \
        2> "$outdir/workload.time"
}

run_workload() {
    local workload="$1"
    local outdir="$2"

    case "$workload" in
        fileserver|varmail)
            run_filebench_workload "$workload" "$outdir"
            ;;
        *)
            echo "ERROR: unknown workload: $workload" >&2
            exit 1
            ;;
    esac
}

cleanup() {
    stop_monitors || true

    if mountpoint -q "$MNT"; then
        sync || true
        umount "$MNT" || true
    fi
}

trap cleanup EXIT

main() {
    require_root
    confirm_destructive
    check_result_dir

    need_cmd mkfs.f2fs
    need_cmd filebench

    mkdir -p "$RESULT_ROOT"

    local workload cfg rep outdir sys

    for workload in "${WORKLOADS[@]}"; do
        for cfg in "${CONFIGS[@]}"; do
            for rep in $(seq 1 "$REPEAT"); do
                outdir="$RESULT_ROOT/${workload}/${cfg}/run_${rep}"
                mkdir -p "$outdir"

                CURRENT_LOG="$outdir/run.log"
                : > "$CURRENT_LOG"

                {
                    echo "workload=$workload"
                    echo "config=$cfg"
                    echo "repeat=$rep"
                    echo "DEV=$DEV"
                    echo "MNT=$MNT"
                    echo "RESULT_ROOT=$RESULT_ROOT"
                    echo "FILESERVER_PROFILE=$FILESERVER_PROFILE"
                    echo "VARMAIL_PROFILE=$VARMAIL_PROFILE"
                } > "$outdir/config.txt"

                log "START workload=$workload config=$cfg repeat=$rep"

                format_and_mount

                sys=$(resolve_f2fs_sysfs)
                log "F2FS sysfs=$sys"

                set_config "$cfg" "$sys"
                clear_runtime_state

                collect_sysfs_snapshot "$sys" "$outdir/before"

                start_monitors "$outdir"

                run_workload "$workload" "$outdir"

                stop_monitors
                sync

                collect_sysfs_snapshot "$sys" "$outdir/after"

                dmesg > "$outdir/dmesg.log" 2>/dev/null || true

                log "END workload=$workload config=$cfg repeat=$rep"

                cleanup
            done
        done
    done

    echo "All done. Results: $RESULT_ROOT"
}

main "$@"