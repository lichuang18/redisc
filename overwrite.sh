# mkdir -p /mnt/test3

# 512K 250s     16K 480s  128K 270s  try: 64K 270 

# 开启/关闭swod
echo 1 > /sys/fs/ef2fs/nvme2n1/swod_enable
# 开启wce
echo 1 > /sys/fs/ef2fs/nvme2n1/swod_completion_enable
echo 1 > /sys/fs/ef2fs/nvme2n1/swod_gc_bg_enable
echo 1 > /sys/fs/ef2fs/nvme2n1/swod_gc_fg_enable 

# 关闭wce
# echo 0 > /sys/fs/ef2fs/nvme2n1/swod_completion_enable
# echo 0 > /sys/fs/ef2fs/nvme2n1/swod_gc_bg_enable
# echo 0 > /sys/fs/ef2fs/nvme2n1/swod_gc_fg_enable 

# 关闭/开启sfi
echo 0 > /sys/fs/ef2fs/nvme2n1/swod_frag_ipu_enable 

# 调整超时swod时间设置
echo 200 > /sys/fs/ef2fs/nvme2n1/swod_hold_min_ms                           
echo 1000 > /sys/fs/ef2fs/nvme2n1/swod_hold_max_ms  

# 手动开启urgent
echo 1 > /sys/fs/ef2fs/nvme2n1/gc_urgent

# 调整swod的hold参数，默认64
# echo 128  > /sys/fs/ef2fs/nvme2n1/swod_max_held_groups  
# echo 4    > /sys/fs/ef2fs/nvme2n1/swod_win_segs                                                 

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
cat /sys/kernel/debug/ef2fs/status