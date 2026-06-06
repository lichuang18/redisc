#!/usr/bin/env bash
# prepare_device.sh - Prepare nvme2n1p1 (70G) with stock F2FS for motivation experiment
# GC triggers faster on this small partition, making discard behavior meaningful.
# Usage: sudo ./prepare_device.sh

set -euo pipefail

DEV="/dev/nvme2n1p1"
MNT="/mnt/f2fs_stock"

echo "[prepare] DEV=$DEV MNT=$MNT"

# Unmount if already mounted (ef2fs or stock)
if mountpoint -q "$MNT" 2>/dev/null; then
    echo "[prepare] umount $MNT"
    umount "$MNT"
fi

mkdir -p "$MNT"

# Wipe partition (destroys any existing filesystem)
echo "[prepare] blkdiscard $DEV"
blkdiscard -f "$DEV"

# Format as stock F2FS (no ef2fs modifications)
echo "[prepare] mkfs.f2fs $DEV"
mkfs.f2fs -f "$DEV"

# Mount with stock discard enabled + BG GC on (key: not nodiscard)
echo "[prepare] mount $DEV $MNT"
mount -t f2fs -o discard,background_gc=on "$DEV" "$MNT"

echo "[prepare] done. $DEV mounted at $MNT (stock F2FS, discard=on, bg_gc=on)"

devbase="$(basename "$DEV")"
echo "[prepare] sysfs path: /sys/fs/f2fs/$devbase"
