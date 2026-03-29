make -j 16
umount /mnt
modprobe f2fs
rmmod ef2fs.ko
insmod ef2fs.ko
mkfs.f2fs -f /dev/nvme2n1
mount -t ef2fs  -o mode=lfs /dev/nvme2n1 /mnt
# mount -t ef2fs  -o mode=lfs /dev/nvme2n1 /mnt
