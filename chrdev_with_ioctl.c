#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>  // 用于copy_to_user
#include "chrdev_ioctl_common.h"  // 包含共用头文件
#include <linux/mlx5/vport.h>

// 驱动核心参数定义
#define DEV_NAME        "chrdev_ioctl_dev"  // 设备名称
#define DEV_MINOR_NUM   0                   // 次设备号起始值
#define DEV_COUNT       1                   // 设备数量

// 全局变量
static dev_t dev_num;                      // 设备号（主+次）
static struct cdev chr_dev;                // 字符设备对象
static struct class *dev_class;            // 设备类
static struct device *dev_device;
struct mlx5_ifc_query_vport_counter_out_bits out;

// 核心：ioctl实现（无传入参数，两个int64_t传出参数）
static long chr_dev_unlocked_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    // 1. 校验命令码的魔数和命令号合法性
    if (_IOC_TYPE(cmd) != CHRDEV_MAGIC) {
        printk(KERN_ERR "ioctl magic number error!\n");
        return -EINVAL;  // 非法参数
    }
    if (_IOC_NR(cmd) != 0x01) {
        printk(KERN_ERR "ioctl command number error!\n");
        return -EINVAL;
    }

    // 2. 校验参数是否为有效指针（传出参数需要用户态提供有效缓冲区）
    if (!arg) {
        printk(KERN_ERR "ioctl arg is NULL!\n");
        return -EFAULT;  // 地址错误
    }

    struct chrdev_ioctl_out_args user_data;
    if (copy_from_user(&user_data, (void __user *)arg, sizeof(struct chrdev_ioctl_out_args))) {
        return -EFAULT;  // 内存拷贝失败
    }

    int devfn = PCI_DEVFN(user_data.slot, user_data.func);
    struct pci_dev *pdev = pci_get_domain_bus_and_slot(0, user_data.bus, devfn);
    if (!pdev)
	return -EFAULT;
    void *mdev = pci_get_drvdata(pdev);
    if (!mdev)
	return -EFAULT;


    //printk("%d %d %d %p %p\n", user_data.bus, user_data.slot, user_data.func, pdev, mdev);

    // 3. 处理我们定义的ioctl命令（传出两个int64_t参数）
    if (cmd == CHRDEV_IOCTL_GET_TWO_INT64) {
	int err;
	//int sz = MLX5_ST_SZ_BYTES(query_vport_counter_out);
	err = mlx5_core_query_vport_counter(mdev, 0, 0, 1, &out);
	if (!err) {
#define MLX5_SUM_CNT(p, cntr1, cntr2)   \
        (MLX5_GET64(query_vport_counter_out, p, cntr1) + \
        MLX5_GET64(query_vport_counter_out, p, cntr2))
	    user_data.val1 = MLX5_SUM_CNT(&out, transmitted_ib_unicast.octets,
                         transmitted_ib_multicast.octets) >> 2;
	    user_data.val2 = MLX5_SUM_CNT(&out, received_ib_unicast.octets,
                         received_ib_multicast.octets) >> 2;

	} else {
	    printk(KERN_ERR "query counter failed!\n");
	    return -EFAULT;
	}

        // 给两个int64_t参数赋值（自定义值，作为传出数据，无传入参数）
        //user_data.val1 = 123456789012345LL;  // int64_t常量（加LL表示long long）
        //user_data.val2 = 987654321098765LL;

        // 核心：将内核态数据拷贝到用户态（传出参数的关键操作）
        // copy_to_user：返回未拷贝成功的字节数，返回0表示拷贝成功
        if (copy_to_user((void __user *)arg, &user_data, sizeof(struct chrdev_ioctl_out_args))) {
            printk(KERN_ERR "copy_to_user failed!\n");
            return -EFAULT;
        }

        printk(KERN_INFO "ioctl success: val1=%lld, val2=%lld\n", user_data.val1, user_data.val2);
        return 0;  // 处理成功
    }

    // 未知命令
    printk(KERN_ERR "unknown ioctl command!\n");
    return -EINVAL;
}

// file_operations 结构体（绑定ioctl操作）
static const struct file_operations chr_dev_fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = chr_dev_unlocked_ioctl,  // 绑定现代ioctl函数
    .open           = NULL,
    .release        = NULL,
};

// 驱动入口函数（加载驱动）
static int __init chrdev_ioctl_init(void)
{
    int ret;

    // 1. 动态分配设备号
    ret = alloc_chrdev_region(&dev_num, DEV_MINOR_NUM, DEV_COUNT, DEV_NAME);
    if (ret < 0) {
        printk(KERN_ERR "alloc chrdev region failed! ret: %d\n", ret);
        goto err_alloc;
    }
    printk(KERN_INFO "alloc chrdev success: major=%d, minor=%d\n", MAJOR(dev_num), MINOR(dev_num));

    // 2. 初始化cdev并绑定file_operations
    cdev_init(&chr_dev, &chr_dev_fops);
    chr_dev.owner = THIS_MODULE;

    // 3. 添加cdev到内核
    ret = cdev_add(&chr_dev, dev_num, DEV_COUNT);
    if (ret < 0) {
        printk(KERN_ERR "cdev add failed! ret: %d\n", ret);
        goto err_cdev_add;
    }

    // 4. 创建设备类
    dev_class = class_create(THIS_MODULE, DEV_NAME);
    if (IS_ERR(dev_class)) {
        printk(KERN_ERR "class create failed!\n");
        ret = PTR_ERR(dev_class);
        goto err_class_create;
    }

    // 5. 创建设备节点（/dev/chrdev_ioctl_dev）
    dev_device = device_create(dev_class, NULL, dev_num, NULL, DEV_NAME);
    if (IS_ERR(dev_device)) {
        printk(KERN_ERR "device create failed!\n");
        ret = PTR_ERR(dev_device);
        goto err_device_create;
    }

    printk(KERN_INFO "chrdev with ioctl init success!\n");
    return 0;

    // 异常处理（反向释放资源）
err_device_create:
    class_destroy(dev_class);
err_class_create:
    cdev_del(&chr_dev);
err_cdev_add:
    unregister_chrdev_region(dev_num, DEV_COUNT);
err_alloc:
    return ret;
}

// 驱动出口函数（卸载驱动）
static void __exit chrdev_ioctl_exit(void)
{
    // 释放所有资源
    device_destroy(dev_class, dev_num);
    class_destroy(dev_class);
    cdev_del(&chr_dev);
    unregister_chrdev_region(dev_num, DEV_COUNT);

    printk(KERN_INFO "chrdev with ioctl exit success!\n");
}

// 内核模块声明
module_init(chrdev_ioctl_init);
module_exit(chrdev_ioctl_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Test");
MODULE_DESCRIPTION("Char device driver with ioctl (2 int64_t output params, no input params)");
MODULE_VERSION("1.0");
