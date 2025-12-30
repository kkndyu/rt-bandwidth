#define _GNU_SOURCE
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <sched.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include "chrdev_ioctl_common.h"  // 包含共用头文件

// ==================== 可配置参数 ====================
#define CPU_CORE 127                // 绑定的CPU核心
#define CPU_FREQ_GHZ 2.7           // CPU主频（GHz）
#define SAMPLING_LOOP 1000         // 空循环次数（调小到1000，≈0.33微秒/次）
#define DEFAULT_RDMA_DEV "mlx5_0"  // 默认RDMA设备名
#define RDMA_PORT 1                // RDMA端口号
#define CACHE_SIZE 10000000         // 缓存大小（支持1秒内百万级采样）
#define PRINT_INTERVAL_S 2.0       // 1秒打印一次峰值
// =============================================================================

// 带宽缓存结构体
typedef struct {
    double rx_bw_gbps;
    double tx_bw_gbps;
    int delta_us;
} BandwidthCache;

// 全局变量
char rdma_dev_name[64] = {0};
int rcv_fd = -1;                   // 预打开的接收计数器文件描述符
int xmit_fd = -1;                  // 预打开的发送计数器文件描述符
int counter_fd = -1;                  // 预打开的发送计数器文件描述符
BandwidthCache *bw_cache = NULL;
int cache_idx = 0;
uint64_t start_cycle = 0;
struct chrdev_ioctl_out_args user_data;  // 用户态缓冲区，用于接收传出参数

// 1. 获取CPU cycle值（RDTSCP）
static inline uint64_t get_cycle(void) {
    uint32_t lo, hi;
    __asm__ __volatile__ ("rdtscp" : "=a"(lo), "=d"(hi) : : "rcx");
    return (uint64_t)hi << 32 | lo;
}

static double CPU_FREQ;
static double calibrate_tsc_hz(void) {
  struct timespec a,b; clock_gettime(CLOCK_MONOTONIC_RAW,&a);
  uint64_t c1=get_cycle(); struct timespec req={.tv_nsec=200*1000*1000}; nanosleep(&req,NULL);
  uint64_t c2=get_cycle(); clock_gettime(CLOCK_MONOTONIC_RAW,&b);
  double dt=(b.tv_sec-a.tv_sec)+(b.tv_nsec-a.tv_nsec)/1e9; return (c2-c1)/dt;
}

uint64_t read_rdma_counter_1(int fd) {
    int ret;
    ret = ioctl(fd, CHRDEV_IOCTL_GET_TWO_INT64, &user_data);
    if (ret < 0) {
        perror("ioctl failed");
        exit(EXIT_FAILURE);
    }
    return user_data.val1;
}

// 3. 绑定进程到固定CPU核心
void bind_cpu(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    if (sched_setaffinity(0, sizeof(cpu_set_t), &cpuset) < 0) {
        perror("sched_setaffinity failed");
        exit(EXIT_FAILURE);
    }
}

// 4. 统计并打印1秒内的峰值带宽
void print_peak_bandwidth(uint64_t elapsed_cycle) {
    if (cache_idx == 0) return;

    double elapsed_s = (double)elapsed_cycle / (CPU_FREQ * 1000000000.0);
    double rx_peak_gbps = 0.0, tx_peak_gbps = 0.0;
    //for (int i = 0; i < cache_idx; i++) {
    //    if (bw_cache[i].rx_bw_gbps > rx_peak_gbps) rx_peak_gbps = bw_cache[i].rx_bw_gbps;
    //    if (bw_cache[i].tx_bw_gbps > tx_peak_gbps) tx_peak_gbps = bw_cache[i].tx_bw_gbps;
    //}

    char time_buf[32];
    time_t now = time(NULL);
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", localtime(&now));
    //printf("[%s] 1秒周期内峰值带宽 - RX: %.2f Gbps, TX: %.2f Gbps (采样次数: %d, 实际耗时: %.3f 秒, 平均采样间隔: %.2f 微秒)\n",
    //       time_buf, rx_peak_gbps, tx_peak_gbps, cache_idx, elapsed_s,
    //       (elapsed_s * 1000000) / cache_idx); // 计算平均采样间隔（微秒）
    //fflush(stdout);

    // -------------------------- 1. 单循环完成RX/TX TOP8筛选 --------------------------
    typedef struct {
        double bw_value;    // 带宽值
        int sample_idx;     // 对应的原始采样索引
	int us;
    } BW_TOP_T;

    #define TOP_NUM 8  // TOP8数量，便于修改
    BW_TOP_T rx_top[TOP_NUM] = {0};
    BW_TOP_T tx_top[TOP_NUM] = {0};

    // 初始化top数组：带宽值设为-1.0，索引设为-1（区分有效/无效数据）
    for (int i = 0; i < TOP_NUM; i++) {
        rx_top[i].bw_value = 0;
        tx_top[i].bw_value = 0;
        rx_top[i].sample_idx = -1;
        tx_top[i].sample_idx = -1;
    }
    int rx_flag = 0;
    int tx_flag = 0;

    // 单轮遍历bw_cache，同时筛选RX和TX的TOP8
    for (int i = 0; i < cache_idx; i++) {
        double current_rx = bw_cache[i].rx_bw_gbps;
        double current_tx = bw_cache[i].tx_bw_gbps;
        int current_sample_idx = i;

        // 处理RX TOP8插入
        for (int j = 0; j < TOP_NUM; j++) {
            if (current_rx > rx_top[j].bw_value) {
                rx_flag = 1;
                for (int k = TOP_NUM - 1; k > j; k--) {
                    rx_top[k] = rx_top[k-1];
                }
                rx_top[j].bw_value = current_rx;
                rx_top[j].sample_idx = current_sample_idx;
                rx_top[j].us = bw_cache[i].delta_us;
                break;
            }
        }

        // 处理TX TOP8插入
        for (int j = 0; j < TOP_NUM; j++) {
            if (current_tx > tx_top[j].bw_value) {
                tx_flag = 1;
                for (int k = TOP_NUM - 1; k > j; k--) {
                    tx_top[k] = tx_top[k-1];
                }
                tx_top[j].bw_value = current_tx;
                tx_top[j].sample_idx = current_sample_idx;
                tx_top[j].us = bw_cache[i].delta_us;
                break;
            }
        }
    }

    // -------------------------- 2. 拼接TOP8字符串（无循环打印，仅拼接） --------------------------
    #define TOP_STR_BUF_SIZE 1024  // 足够容纳RX+TX TOP8的字符串内容
    char top_str_buf[TOP_STR_BUF_SIZE] = {0};  // 总拼接缓冲区
    int buf_offset = 0;                        // 缓冲区偏移量，用于逐段拼接

    if (rx_flag) {
    // 拼接RX带宽TOP8标题
    buf_offset += snprintf(top_str_buf + buf_offset, TOP_STR_BUF_SIZE - buf_offset,
                           "[%s] RX TOP8：", time_buf);

    // 拼接RX TOP8有效数据（无循环打印，仅循环拼接）
    int valid_rx_count = 0;
    for (int i = 0; i < TOP_NUM; i++) {
        if (rx_top[i].sample_idx != -1) {
            valid_rx_count++;
            // 逐段拼接RX TOP8每条数据，更新偏移量避免缓冲区溢出
            buf_offset += snprintf(top_str_buf + buf_offset, TOP_STR_BUF_SIZE - buf_offset,
                                   "  %d：%d，%.2f Gbps",
                                   rx_top[i].us, rx_top[i].sample_idx, rx_top[i].bw_value);
        }
    }
    buf_offset += snprintf(top_str_buf + buf_offset, TOP_STR_BUF_SIZE - buf_offset, "\n");
    // 若无可效RX数据，拼接提示信息
    if (valid_rx_count == 0) {
        buf_offset += snprintf(top_str_buf + buf_offset, TOP_STR_BUF_SIZE - buf_offset,
                               "  无有效RX带宽采样数据\n");
    }
    }
    if (tx_flag) {

    // 拼接TX带宽TOP8标题
    buf_offset += snprintf(top_str_buf + buf_offset, TOP_STR_BUF_SIZE - buf_offset,
                           "[%s] TX TOP8：", time_buf);

    // 拼接TX TOP8有效数据（无循环打印，仅循环拼接）
    int valid_tx_count = 0;
    for (int i = 0; i < TOP_NUM; i++) {
        if (tx_top[i].sample_idx != -1) {
            valid_tx_count++;
            // 逐段拼接TX TOP8每条数据，更新偏移量
            buf_offset += snprintf(top_str_buf + buf_offset, TOP_STR_BUF_SIZE - buf_offset,
                                   "  %d：%d，%.2f Gbps",
                                   tx_top[i].us, tx_top[i].sample_idx, tx_top[i].bw_value);
        }
    }
    buf_offset += snprintf(top_str_buf + buf_offset, TOP_STR_BUF_SIZE - buf_offset, "\n");
    // 若无可效TX数据，拼接提示信息
    if (valid_tx_count == 0) {
        buf_offset += snprintf(top_str_buf + buf_offset, TOP_STR_BUF_SIZE - buf_offset,
                               "  无有效TX带宽采样数据\n");
    }
    }

    rx_peak_gbps = rx_top[0].bw_value;
    tx_peak_gbps = tx_top[0].bw_value;
    printf("[%s] 1秒周期内峰值带宽 - RX: %.2f Gbps, TX: %.2f Gbps (采样次数: %d, 实际耗时: %.3f 秒, 平均采样间隔: %.2f 微秒)\n",
           time_buf, rx_peak_gbps, tx_peak_gbps, cache_idx, elapsed_s,
           (elapsed_s * 1000000) / cache_idx); // 计算平均采样间隔（微秒）

    //-------------------------- 3. 单次printf输出完整TOP8字符串 --------------------------
    printf("%s", top_str_buf);
    double tsc_hz = calibrate_tsc_hz();
    fprintf(stderr,"TSC ~= %.3f GHz\n", tsc_hz/1e9);
    fflush(stdout);

    cache_idx = 0;
}

int main(int argc, char *argv[]) {
    // 分配缓存内存
    bw_cache = (BandwidthCache *)malloc(CACHE_SIZE * sizeof(BandwidthCache));
    if (bw_cache == NULL) {
        perror("malloc bw_cache failed");
        exit(EXIT_FAILURE);
    }
    memset(bw_cache, 0, CACHE_SIZE * sizeof(BandwidthCache));

    // 处理RDMA设备名参数
    char bdf_str[8];
    if (argc >= 2) {
        strncpy(bdf_str, argv[1], 7);
        bdf_str[8] = '\0';
    } else {
        printf("no RDMA bdf, quit\n");
	exit(1);
    }

    int loop = SAMPLING_LOOP;
    if (argc >= 3) {
        loop = atoi(argv[2]);
	loop = loop == 0 ? SAMPLING_LOOP : loop;
    }

    int bus, slot, func;
    int ret = sscanf(bdf_str, "%x:%x.%x", &user_data.bus, &user_data.slot, &user_data.func);
    if (ret != 3) {
        printf("bdf pattern error, quit\n");
	exit(1);
    }

    printf("%d  %d  %d\n", user_data.bus, user_data.slot, user_data.func);

    counter_fd = open("/dev/chrdev_ioctl_dev", O_RDWR);
    if (counter_fd < 0) {
        perror("open device failed");
        return -1;
    }
    printf("open device /dev/chrdev_ioctl_dev success (fd=%d)\n", counter_fd);

    double tsc_hz = calibrate_tsc_hz();
    fprintf(stderr,"TSC ~= %.3f GHz\n", tsc_hz/1e9);
    CPU_FREQ = tsc_hz > 0 ? tsc_hz/1e9 : CPU_FREQ_GHZ;

    // 绑定CPU核心
    bind_cpu(CPU_CORE);
    printf("已绑定进程到CPU核心 %d\n", CPU_CORE);
    printf("RDMA设备：%s，端口：%d\n", rdma_dev_name, RDMA_PORT);
    printf("CPU主频：%.2f GHz\n", CPU_FREQ);
    printf("采样空循环次数：%d，打印间隔：%.1f秒\n", loop, PRINT_INTERVAL_S);
    printf("------------------------------------------------------------\n");

    // 初始化变量
    uint64_t t1, t2, cycle_diff;
    uint64_t rcv1, rcv2, xmit1, xmit2;
    double time_diff_s;
    uint64_t rcv_diff, xmit_diff;
    double rx_bw_gbps, tx_bw_gbps;
    uint64_t interval = PRINT_INTERVAL_S * CPU_FREQ * 1000000000;

    // 初始化1秒周期起始cycle
    start_cycle = get_cycle();

    // 步骤1：读取初始cycle和RDMA counter
    t1 = get_cycle();
    read_rdma_counter_1(counter_fd);
    uint64_t tmp = get_cycle();
    t2 = t1 + ((tmp - t1) >> 1);

    // 无限采样循环
    while (1) {
	t1 = t2;
	xmit1 = user_data.val1;
	rcv1 = user_data.val2;

        // 步骤2：微秒级等待（空循环，无syscall开销）
        for (uint64_t i = 0; i < loop; i++) {
            __asm__ __volatile__ ("nop"); // 空操作，避免编译器优化
        }

        // 步骤3：读取当前cycle和RDMA counter
        t2 = get_cycle();
	read_rdma_counter_1(counter_fd);
	tmp = get_cycle();
	t2 = t2 + ((tmp - t2) >> 1);
	xmit2 = user_data.val1;
	rcv2 = user_data.val2;

        // 步骤4：计算时间差和带宽
        cycle_diff = t2 - t1;
        time_diff_s = (double)cycle_diff / (CPU_FREQ);
        rcv_diff = (rcv2 > rcv1) ? (rcv2 - rcv1) : 0;
        xmit_diff = (xmit2 > xmit1) ? (xmit2 - xmit1) : 0;
        rx_bw_gbps = (rcv_diff * 8.0 * 4) / (time_diff_s);
        tx_bw_gbps = (xmit_diff * 8.0 * 4) / (time_diff_s);

        // 步骤5：存入缓存（纯内存操作）
        if (cache_idx < CACHE_SIZE) {
            bw_cache[cache_idx].rx_bw_gbps = rx_bw_gbps;
            bw_cache[cache_idx].tx_bw_gbps = tx_bw_gbps;
            bw_cache[cache_idx].delta_us = time_diff_s / 1000;
            cache_idx++;
        } else {
            fprintf(stderr, "缓存已满，丢弃本次采样数据\n");
        }

        // 步骤6：判断是否达到1秒打印周期
        uint64_t current_cycle = get_cycle();
        uint64_t elapsed_s = (current_cycle - start_cycle);
        if (elapsed_s >= interval) {
            print_peak_bandwidth(elapsed_s);
            start_cycle = current_cycle;

	    t2 = get_cycle();
	    read_rdma_counter_1(counter_fd);
	    tmp = get_cycle();
	    t2 = t2 + ((tmp - t2) >> 1);
        }
    }

    // 关闭文件描述符（实际不会执行）
    close(counter_fd);
    close(rcv_fd);
    close(xmit_fd);
    free(bw_cache);
    return 0;
}
