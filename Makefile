# SPDX-License-Identifier: GPL-2.0
obj-m += ef2fs.o

ef2fs-y		:= dir.o file.o inode.o namei.o hash.o super.o inline.o
ef2fs-y		+= checkpoint.o gc.o data.o node.o segment.o recovery.o
ef2fs-y		+= shrinker.o extent_cache.o sysfs.o compress.o
ef2fs-y		+= compress.o xattr.o acl.o verity.o
ef2fs-$(CONFIG_F2FS_STAT_FS) += debug.o
ef2fs-$(CONFIG_F2FS_FS_XATTR) += xattr.o
ef2fs-$(CONFIG_F2FS_FS_POSIX_ACL) += acl.o
ef2fs-$(CONFIG_FS_VERITY) += verity.o
ef2fs-$(CONFIG_F2FS_FS_COMPRESSION) += compress.o
ef2fs-$(CONFIG_F2FS_IOSTAT) += iostat.o
EXTRA_CFLAGS += -I$(PWD)

default:
	$(MAKE) -C /lib/modules/$(shell uname -r)/build M=$(shell pwd) modules

clean:
	$(MAKE) -C /lib/modules/$(shell uname -r)/build M=$(shell pwd) clean
