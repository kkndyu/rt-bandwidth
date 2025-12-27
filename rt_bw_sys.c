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

// ==================== 可配置参数 ====================
#define CPU_CORE 127                 // 绑定的CPU核心
#define CPU_FREQ_GHZ 2.7           // CPU主频（GHz）
#define SAMPLING_LOOP 10000         // 空循环次数（调小到1000，≈0.33微秒/次）
#define DEFAULT_RDMA_DEV "mlx5_0"  // 默认RDMA设备名
#define RDMA_PORT 1                // RDMA端口号
#define CACHE_SIZE 100000000         // 缓存大小（支持1秒内百万级采样）
#define PRINT_INTERVAL_S 2.0       // 1秒打印一次峰值
// =============================================================================

// 带宽缓存结构体
typedef struct {
    double rx_bw_gbps;
    double tx_bw_gbps;
} BandwidthCache;

// 全局变量
char rdma_dev_name[64] = {0};
int rcv_fd = -1;                   // 预打开的接收计数器文件描述符
int xmit_fd = -1;                  // 预打开的发送计数器文件描述符
BandwidthCache *bw_cache = NULL;
int cache_idx = 0;
uint64_t start_cycle = 0;

// 1. 获取CPU cycle值（RDTSCP）
static inline uint64_t get_cycle(void) {
    uint32_t lo, hi;
    __asm__ __volatile__ ("rdtscp" : "=a"(lo), "=d"(hi) : : "rcx");
    return (uint64_t)hi << 32 | lo;
}

// 2. 快速读取RDMA counter（复用已打开的fd，无open/close开销）
uint64_t read_rdma_counter(int fd) {
    char buf[32] = {0};
    // 用pread从偏移0读取，避免lseek，减少syscall开销
    ssize_t len = pread(fd, buf, sizeof(buf)-1, 0);
    if (len < 0) {
        perror("pread counter file failed");
        exit(EXIT_FAILURE);
    }
    return strtoull(buf, NULL, 10);
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

    double elapsed_s = (double)elapsed_cycle / (CPU_FREQ_GHZ * 1000000000.0);
    double rx_peak_gbps = 0.0, tx_peak_gbps = 0.0;
    for (int i = 0; i < cache_idx; i++) {
        if (bw_cache[i].rx_bw_gbps > rx_peak_gbps) rx_peak_gbps = bw_cache[i].rx_bw_gbps;
        if (bw_cache[i].tx_bw_gbps > tx_peak_gbps) tx_peak_gbps = bw_cache[i].tx_bw_gbps;
    }

    char time_buf[32];
    time_t now = time(NULL);
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", localtime(&now));
    printf("[%s] 1秒周期内峰值带宽 - RX: %.2f Gbps, TX: %.2f Gbps (采样次数: %d, 实际耗时: %.3f 秒, 平均采样间隔: %.2f 微秒)\n",
           time_buf, rx_peak_gbps, tx_peak_gbps, cache_idx, elapsed_s,
           (elapsed_s * 1000000) / cache_idx); // 计算平均采样间隔（微秒）
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
    if (argc >= 2) {
        strncpy(rdma_dev_name, argv[1], sizeof(rdma_dev_name)-1);
        rdma_dev_name[sizeof(rdma_dev_name)-1] = '\0';
    } else {
        strcpy(rdma_dev_name, DEFAULT_RDMA_DEV);
        printf("未传入RDMA设备名，使用默认设备：%s\n", rdma_dev_name);
    }

    // 拼接RDMA counter路径
    char rcv_data_path[128] = {0};
    char xmit_data_path[128] = {0};
    snprintf(rcv_data_path, sizeof(rcv_data_path),
             "/sys/class/infiniband/%s/ports/%d/counters/port_rcv_data",
             rdma_dev_name, RDMA_PORT);
    snprintf(xmit_data_path, sizeof(xmit_data_path),
             "/sys/class/infiniband/%s/ports/%d/counters/port_xmit_data",
             rdma_dev_name, RDMA_PORT);

    // 预打开RDMA计数器文件（仅打开一次，复用fd）
    rcv_fd = open(rcv_data_path, O_RDONLY);
    xmit_fd = open(xmit_data_path, O_RDONLY);
    if (rcv_fd < 0 || xmit_fd < 0) {
        perror("open counter file failed");
        exit(EXIT_FAILURE);
    }

    // 绑定CPU核心
    bind_cpu(CPU_CORE);
    printf("已绑定进程到CPU核心 %d\n", CPU_CORE);
    printf("RDMA设备：%s，端口：%d\n", rdma_dev_name, RDMA_PORT);
    printf("CPU主频：%.2f GHz\n", CPU_FREQ_GHZ);
    printf("采样空循环次数：%d，打印间隔：%.1f秒\n", SAMPLING_LOOP, PRINT_INTERVAL_S);
    printf("------------------------------------------------------------\n");

    // 初始化变量
    uint64_t t1, t2, cycle_diff;
    uint64_t rcv1, rcv2, xmit1, xmit2;
    double time_diff_s;
    uint64_t rcv_diff, xmit_diff;
    double rx_bw_gbps, tx_bw_gbps;
    uint64_t interval = PRINT_INTERVAL_S * CPU_FREQ_GHZ * 1000000000;

    // 初始化1秒周期起始cycle
    start_cycle = get_cycle();

    // 无限采样循环
    while (1) {
        // 步骤1：读取初始cycle和RDMA counter
        t1 = get_cycle();
        rcv1 = read_rdma_counter(rcv_fd);
        //xmit1 = read_rdma_counter(xmit_fd);

        // 步骤2：微秒级等待（空循环，无syscall开销）
        for (uint64_t i = 0; i < SAMPLING_LOOP; i++) {
            __asm__ __volatile__ ("nop"); // 空操作，避免编译器优化
        }

        // 步骤3：读取当前cycle和RDMA counter
        t2 = get_cycle();
        rcv2 = read_rdma_counter(rcv_fd);
        //xmit2 = read_rdma_counter(xmit_fd);

        // 步骤4：计算时间差和带宽
        cycle_diff = t2 - t1;
        time_diff_s = (double)cycle_diff / (CPU_FREQ_GHZ);
        rcv_diff = (rcv2 > rcv1) ? (rcv2 - rcv1) : 0;
        xmit_diff = (xmit2 > xmit1) ? (xmit2 - xmit1) : 0;
        rx_bw_gbps = (rcv_diff * 8.0 * 4) / (time_diff_s);
        tx_bw_gbps = (xmit_diff * 8.0 * 4) / (time_diff_s);

        // 步骤5：存入缓存（纯内存操作）
        if (cache_idx < CACHE_SIZE) {
            bw_cache[cache_idx].rx_bw_gbps = rx_bw_gbps;
            bw_cache[cache_idx].tx_bw_gbps = tx_bw_gbps;
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
        }
    }

    // 关闭文件描述符（实际不会执行）
    close(rcv_fd);
    close(xmit_fd);
    free(bw_cache);
    return 0;
}
