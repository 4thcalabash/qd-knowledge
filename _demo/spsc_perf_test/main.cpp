// SPSC 队列性能测试主程序
// 用法: ./bin/<impl> <消息总数> [队列容量]
//
// 每个实现头文件必须定义 namespace spsc 下的 Queue 类型，接口约定：
//   explicit Queue(std::size_t capacity)
//   void push(std::int64_t v)
//   bool pop(std::int64_t& v)      // 队列为空返回 false
//
// 生产者依次写入 0..n-1，消费者按序取出并校验，防止"优化掉错误的实现"。
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

// 线程绑定的目标核。应选择不同的物理核（若 0/1 是同一物理核的超线程对，则需调整）。
static const int kProducerCpu = 0;
static const int kConsumerCpu = 1;

static void pin_to_cpu(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (int rc = pthread_setaffinity_np(pthread_self(), sizeof(set), &set); rc != 0)
        std::fprintf(stderr, "warning: pin to cpu %d failed: %s\n", cpu, std::strerror(rc));
}

int main(int argc, char** argv) {
    const std::int64_t n = argc > 1 ? std::atoll(argv[1]) : 30'000'000LL;
    const std::size_t capacity = argc > 2 ? std::size_t(std::atoll(argv[2])) : (1u << 20);

    spsc::Queue q(capacity);

    auto t0 = std::chrono::steady_clock::now();

    int pcpu = -1, ccpu = -1;

    std::thread producer([&] {
        pin_to_cpu(kProducerCpu);
        pcpu = sched_getcpu();
        for (std::int64_t i = 0; i < n; ++i)
            q.push(i);
    });

    std::thread consumer([&] {
        pin_to_cpu(kConsumerCpu);
        ccpu = sched_getcpu();
        std::int64_t v = 0, expected = 0;
        while (expected < n) {
            while (!q.pop(v))
                std::this_thread::yield();  // 空队列时让出 CPU
            if (v != expected) {
                std::fprintf(stderr, "乱序: got %lld, want %lld\n",
                             (long long)v, (long long)expected);
                std::abort();
            }
            ++expected;
        }
    });

    producer.join();
    consumer.join();

    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("n = %lld, elapsed = %.6f s, throughput = %.2f M msg/s, avg = %.1f ns/msg\n",
                (long long)n, secs, n / secs / 1e6, 1e9 * secs / n);
    std::printf("producer on cpu %d, consumer on cpu %d\n", pcpu, ccpu);
    return 0;
}
