# mkdir -p /mnt/test3

# 512K 250s     16K 480s  128K 270s  try: 64K 270 

echo 1 > /sys/fs/ef2fs/nvme2n1/swod_enable
echo 0 > /sys/fs/ef2fs/nvme2n1/swod_completion_enable
echo 0 > /sys/fs/ef2fs/nvme2n1/swod_gc_bg_enable
echo 0 > /sys/fs/ef2fs/nvme2n1/swod_gc_fg_enable 
echo 200 > /sys/fs/ef2fs/nvme2n1/swod_hold_min_ms                           
echo 1000 > /sys/fs/ef2fs/nvme2n1/swod_hold_max_ms  

echo 1 > /sys/fs/ef2fs/nvme2n1/gc_urgent

cat  /sys/fs/ef2fs/nvme2n1/swod_enable
cat  /sys/fs/ef2fs/nvme2n1/swod_held_groups
cat  /sys/fs/ef2fs/nvme2n1/swod_skip_cnt

cat  /sys/fs/ef2fs/nvme2n1/swod_hold_cnt 
cat  /sys/fs/ef2fs/nvme2n1/swod_success_release_cnt
cat /sys/fs/ef2fs/nvme2n1/swod_timeout_release_cnt                          
cat /sys/fs/ef2fs/nvme2n1/swod_pressure_release_cnt  

echo "$(date '+%F %T')"
sync
fio --name=fill \
    --filename=/mnt/test3 \
    --rw=randwrite \
    --bsrange=4K-512K \
    --size=20G \
    --offset=0 \
    --direct=1 \
    --ioengine=libaio \
    --numjobs=4 \
    --fallocate=none \
    --iodepth=16 \
    --runtime=250 \
    --ramp_time=20 \
    --randrepeat=1 \
    --randseed=12345 \
    --group_reporting=1 \
    --write_bw_log=bw \
    --log_avg_msec=1000 \
    --per_job_logs=0 \
    --time_based=1
    # --status-interval=1 \
    

    # --write_bw_log=bw \
    # --write_iops_log=iops \

    # --offset=10G \

    # --status-interval=1 \
    # --rw=randwrite \
    # --rw=randread \
echo "end test1..."    

echo "$(date '+%F %T')"

cat  /sys/fs/ef2fs/nvme2n1/swod_held_groups
cat  /sys/fs/ef2fs/nvme2n1/swod_skip_cnt
cat  /sys/fs/ef2fs/nvme2n1/swod_success_release_cnt
cat  /sys/fs/ef2fs/nvme2n1/swod_hold_cnt 