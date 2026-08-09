// SPSC 队列性能测试主程序
// 用法: ./bin/<impl> <消息总数> [队列容量]
//
// 每个实现头文件必须定义 namespace spsc 下的 Queue 模板，接口约定：
//   template <typename T> class Queue;
//   explicit Queue(std::size_t capacity)   // 可用槽位 = capacity - 1（牺牲 1 槽）
//   void push(const T& v)
//   bool pop(T& v)                          // 队列为空返回 false
//
// 数据负载为 256 字节 Blob（模拟真实场景的大负载），生产者依次写入 0..n-1，
// 消费者按序取出并校验，防止"优化掉错误的实现"。
// 两个工作线程绑定到固定的物理核（常量可调整），保证观测的可复现性。
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <sched.h>
#include <thread>

#include SPSC_HEADER

// 线程绑定的目标核。**必须选择不同的物理核**：false sharing 需要缓存行在两个物理核间
// 传输，若绑到同一物理核的超线程对（如 cpu 0/1），共享 L1/L2，不会触发。
// 本机 cpu 3/5 是不同物理核（/proc/cpuinfo 的 core id 不同），按需调整。
static const int kProducerCpu = 0;
static const int kConsumerCpu = 2;

// 256 字节数据负载，用于测大数据量下的缓存行传输差异
struct Blob {
    std::int64_t seq;
    char data[256 - sizeof(std::int64_t)];
};

static void pin_to_cpu(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (int rc = pthread_setaffinity_np(pthread_self(), sizeof(set), &set); rc != 0)
        std::fprintf(stderr, "warning: pin to cpu %d failed: %s\n", cpu, std::strerror(rc));
}

int main(int argc, char** argv) {
    const std::int64_t n = argc > 1 ? std::atoll(argv[1]) : 100'000'000LL;
    const std::size_t capacity = argc > 2 ? std::size_t(std::atoll(argv[2])) : (1u << 15);

    spsc::Queue<Blob> q(capacity);
    auto t0 = std::chrono::steady_clock::now();

    std::thread consumer([&] {
        pin_to_cpu(kConsumerCpu);
        Blob v;
        std::int64_t expected = 0;
        while (expected < n) {
            while (!q.pop(v))
                ;  // 空则自旋等待 producer 生产
            if (v.seq != expected) {
                std::fprintf(stderr, "乱序: got %lld, want %lld\n",
                             (long long)v.seq, (long long)expected);
                std::abort();
            }
            ++expected;
        }
    });

    std::thread producer([&] {
        pin_to_cpu(kProducerCpu);
        for (std::int64_t i = 0; i < n; ++i) {
            Blob b;
            b.seq = i;
            std::memset(b.data, (int)i, sizeof(b.data));
            q.push(b);
        }
    });

    producer.join();
    consumer.join();

    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("n = %lld, elapsed = %.6f s, throughput = %.2f M msg/s, avg = %.1f ns/msg\n",
                (long long)n, secs, n / secs / 1e6, 1e9 * secs / n);
    return 0;
}
