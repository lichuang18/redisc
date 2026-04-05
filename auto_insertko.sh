make -j 16
umount /mnt
modprobe f2fs
rmmod ef2fs.ko
insmod ef2fs.ko
mkfs.f2fs -f /dev/nvme2n1
#mount -t ef2fs  -o mode=lfs,nodiscard /dev/nvme2n1 /mnt
mount -t ef2fs  -o mode=lfs /dev/nvme2n1 /mnt

#echo 1 > /sys/fs/ef2fs/nvme2n1/swod_completion_enable
#echo 1 > /sys/fs/ef2fs/nvme2n1/swod_enable
