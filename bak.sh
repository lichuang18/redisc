# mkdir -p /mnt/test3

sync
smartctl -a /dev/nvme2n1
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
    --iodepth=16 \
    --runtime=500 \
    --write_lat_log=./lat_log1 \
    --time_based=1
    # --status-interval=1 \
    

    # --write_bw_log=bw \
    # --write_iops_log=iops \

    # --offset=10G \

    # --status-interval=1 \
    # --rw=randwrite \
    # --rw=randread \
    # --rw=write \
    # --rw=read \
echo "epoch 2: start..."

sync
smartctl -a /dev/nvme2n1
sync

fio --name=fill \
    --filename=/mnt/test4 \
    --rw=randwrite \
    --bs=128K \
    --size=20G \
    --offset=0 \
    --direct=1 \
    --ioengine=libaio \
    --numjobs=1 \
    --fallocate=none \
    --iodepth=16 \
    --runtime=500 \
    --write_lat_log=./lat_log2 \
    --time_based=1

echo "epoch 3: start..."
sync
smartctl -a /dev/nvme2n1
sync


fio --name=fill \
    --filename=/mnt/test4 \
    --rw=randwrite \
    --bs=128K \
    --size=20G \
    --offset=0 \
    --direct=1 \
    --ioengine=libaio \
    --numjobs=1 \
    --fallocate=none \
    --iodepth=16 \
    --runtime=500 \
    --write_lat_log=./lat_log3 \
    --time_based=1

echo "epoch 4: start..."
sync
smartctl -a /dev/nvme2n1
sync

fio --name=fill \
    --filename=/mnt/test4 \
    --rw=randwrite \
    --bs=128K \
    --size=20G \
    --offset=0 \
    --direct=1 \
    --ioengine=libaio \
    --numjobs=1 \
    --fallocate=none \
    --iodepth=16 \
    --runtime=500 \
    --write_lat_log=./lat_log4 \
    --time_based=1

echo "epoch 5: start..."
sync
smartctl -a /dev/nvme2n1
sync

fio --name=fill \
    --filename=/mnt/test4 \
    --rw=randwrite \
    --bs=128K \
    --size=20G \
    --offset=0 \
    --direct=1 \
    --ioengine=libaio \
    --numjobs=1 \
    --fallocate=none \
    --iodepth=16 \
    --runtime=500 \
    --write_lat_log=./lat_log5 \
    --time_based=1
sync
smartctl -a /dev/nvme2n1
sync

