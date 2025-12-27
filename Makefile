# 1. 内核源码目录（默认使用当前系统内核）
KERNELDIR ?= /lib/modules/$(shell uname -r)/build
# 2. 当前目录（驱动源码目录）
PWD := $(shell pwd)
OFED_PATH = /usr/src/ofa_kernel/x86_64/5.15.0-164-generic

EXTRA_CFLAGS += -I$(OFED_PATH)/include \
                -include $(OFED_PATH)/include/linux/compat-2.6.h
#                -DCONFIG_MLX5_CORE_EN

# 3. 要编译的模块名称（去掉.c后缀）
obj-m := chrdev_with_ioctl.o

# 4. 编译目标
all:
	# 标准内核模块编译命令
	$(MAKE) V=1 -C $(KERNELDIR) M=$(PWD) KBUILD_EXTRA_SYMBOLS=$(OFED_PATH)/Module.symvers modules

# 5. 清理目标
clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean
	# 额外清理临时文件
	rm -rf *.o *.mod.c *.mod.o *.symvers *.order *.ko.unsigned
