# mkdir -p /mnt/test3

# 512K 250s     16K 480s  128K 270s  try: 64K 270 
echo "$(date '+%F %T')"
sync
fio --name=fill \
    --filename=/mnt/test3 \
    --rw=randwrite \
    --bs=128K \
    --size=20G \
    --offset=0 \
    --direct=1 \
    --ioengine=libaio \
    --numjobs=1 \
    --fallocate=none \
    --iodepth=32 \
    --write_lat_log=./tmp \
    --randrepeat=1 \
    --randseed=12345 \
    --runtime=300 \
    --group_reporting=1 \
    --time_based=1

    # --status-interval=1 \
    # --write_bw_log=bw \
    # --write_iops_log=iops \

    # --offset=10G \

    # --status-interval=1 \
    # --rw=randwrite \
    # --rw=randread \
echo "$(date '+%F %T')"