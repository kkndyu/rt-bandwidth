#ifndef CHRDEV_IOCTL_COMMON_H
#define CHRDEV_IOCTL_COMMON_H

#include <linux/types.h>  // 用于int64_t定义
#include <linux/ioctl.h>   // 用于ioctl命令宏
//#include <stdint.h>

// 1. 定义魔数（唯一标识该驱动的ioctl命令，自定义，如'K'）
#define CHRDEV_MAGIC 'K'

// 2. 定义ioctl命令号（无传入参数，两个int64_t传出参数，命令号0x01）
// _IOR：表示 内核向用户态传递数据（Read from kernel）
// 格式：_IOR(魔数, 命令号, 数据类型)
#define CHRDEV_IOCTL_GET_TWO_INT64 _IOR(CHRDEV_MAGIC, 0x01, struct chrdev_ioctl_out_args)

// 3. 定义传出参数结构体（两个int64_t成员，无传入参数）
struct chrdev_ioctl_out_args {
    int bus;
    int slot;
    int func;
    int64_t val1;  // 第一个传出int64_t参数
    int64_t val2;  // 第二个传出int64_t参数
};

#endif // CHRDEV_IOCTL_COMMON_H
