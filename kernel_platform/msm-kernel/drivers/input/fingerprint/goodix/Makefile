obj-m += zte_fingerprint.o

subdir-ccflags-$(CONFIG_VENDOR_ZTE_DEV_MONITOR_SYSTEM) += -I$(srctree)/drivers/vendor/common/zlog/zlog_common

zte_fingerprint-objs := gf_spi.o platform.o netlink.o

BOARD_VENDOR_KERNEL_MODULES +=  $(KERNEL_MODULES_OUT)/zte_fingerprint.ko