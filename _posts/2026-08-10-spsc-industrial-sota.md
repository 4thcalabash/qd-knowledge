---
title: "SPSC的两种工业级SOTA实现"
date: 2026-08-10 14:00:00 +0800
model: DeepSeek-V4-Flash
tool: Claude Code CLI
---

SPSC（Single Producer Single Consumer）无锁队列是低延迟系统中核间通信的标准设施。工业界存在两种并行流传的 SOTA 实现，内存布局共享同一骨架，索引机制与满空编码却分道扬镳。本文从实现源码出发，对比两种风格的原理、正确性与性能，并给出选型依据。

> 原始出处：
>
> - **指针回转式**（rigtorp::SPSCQueue）：[rigtorp/SPSCQueue](https://github.com/rigtorp/SPSCQueue)（[SPSCQueue.h](https://github.com/rigtorp/SPSCQueue/blob/master/include/rigtorp/SPSCQueue.h)，MIT）
> - **绝对指针掩码式**（Vyukov 有界队列的 SPSC 特化）：[bounded-mpmc-queue](https://sites.google.com/site/1024cores/home/lock-free-algorithms/queues/bounded-mpmc-queue)。Vyukov 未单独发表有界 SPSC 专文，该风格由社区从其有界 MPMC 队列推导特化

- 目录
{:toc}

## 指针回转式（rigtorp::SPSCQueue）

### 原理概述

#### 内存布局

![SPSC 内存布局]({{ site.baseurl }}/assets/images/spsc-memory-layout.svg)

##### 状态变量区

控制状态以 Head/Tail 两个原子索引为核心，各自 `alignas(kCacheLineSize)` 独占缓存行（图中字段两侧的 Pad 即隔离带）。

| 图中标签 | 源码字段 | 类型 | 谁写 | 谁读 |
|---------|---------|------|------|------|
| Head | `readIdx_` | `std::atomic<size_t>` | 消费者 | 生产者 |
| Tail | `writeIdx_` | `std::atomic<size_t>` | 生产者 | 消费者 |

开闭语义：Tail 指向下一个待写入槽位、Head 指向下一个待读取槽位，元素位于半开区间 `[Head, Tail)`，数量为 `Tail - Head`；二者为 `size_t`，无符号溢出截断，回绕后差值仍正确。

图上另有 Head Cache / Tail Cache 两个字段，是 Head/Tail 的本地缓存副本，动机与机制见性能优化细节。

##### 数据槽位区

槽位无填充、紧密排列——数据流方向固定（生产者写 → 消费者读），多个小对象共行一次性搬运，无填充是特性而非缺陷。

##### 首尾填充 kPadding

```cpp
static constexpr size_t kPadding = (kCacheLineSize - 1) / sizeof(T) + 1;
```

`slots_` 独立堆分配，首尾各 `kPadding` 个元素永不使用，避免最边上真实槽位与堆中相邻对象共享缓存行。

##### slack 元素

有效容量比申请量少 1（有效空间不包含首尾 padding）：`capacity_ = capacity + 1`，暴露 `capacity() = capacity_ - 1`，剩余一个槽位永不装数据，用于满判定：

- 空：`Tail == Head`（区间为空）；
- 满：`Tail + 1 == Head`（下一写入位置追上读位置）；
- 若无 slack，满与空同为 `Tail == Head`，无法区分；slack 保证任何时刻至少一个空槽，使 `Tail == Head` 唯一对应空。

### ⭐ 内存序正确性分析

从具体变量的读写出发，而非笼统的"无锁"。以下面 rigtorp 完整类结构为分析对象（省去分配器泛型与模板 trait）：

```cpp
template <typename T>
class SPSCQueue {
public:
  explicit SPSCQueue(const size_t capacity) : capacity_(capacity) {
    capacity_++; // slack 元素：有效容量 = capacity_-1，见上
    slots_ = std::allocator_traits<Allocator>::allocate(
        allocator_, capacity_ + 2 * kPadding);
  }

  ~SPSCQueue() {
    while (front()) {
      pop(); // 排空：析构所有剩余元素
    }
    std::allocator_traits<Allocator>::deallocate(
        allocator_, slots_, capacity_ + 2 * kPadding);
  }

  // 生产者：先写槽位 → 再发布 writeIdx_（release）
  template <typename... Args>
  void emplace(Args &&...args) {
    auto const writeIdx = writeIdx_.load(std::memory_order_relaxed); // 读自己：relaxed
    auto nextWriteIdx = writeIdx + 1;
    if (nextWriteIdx == capacity_) {
      nextWriteIdx = 0; // 指针回绕
    }
    // 满判定：读对端 readIdx_（acquire，可能满才读）
    while (nextWriteIdx == readIdxCache_) { // “可能”满了
      // 第一次执行时：加载真实 readIdx_ 准确判断是否满了。
      // 后续执行时：队列真的满了，spinning等待。
      readIdxCache_ = readIdx_.load(std::memory_order_acquire); 
    }
    new (&slots_[writeIdx + kPadding]) T(std::forward<Args>(args)...); // 队列已有空位，执行写入。
    writeIdx_.store(nextWriteIdx, std::memory_order_release); // 发布：其他核先看到槽位写，再看到索引
  }

  // 消费者：front()/pop() 分离，先读后消费
  RIGTORP_NODISCARD T *front() noexcept {
    auto const readIdx = readIdx_.load(std::memory_order_relaxed); // 读自己：relaxed
    if (readIdx == writeIdxCache_) { // 可能为空
      writeIdxCache_ = writeIdx_.load(std::memory_order_acquire); // 加载真实值
      if (writeIdxCache_ == readIdx) { // 准确判断是否为空。
        return nullptr; // 空
      }
    }
    return &slots_[readIdx + kPadding];
  }

  // 消费者：先弹出槽位 → 再发布 readIdx_（release）
  void pop() noexcept {
    auto const readIdx = readIdx_.load(std::memory_order_relaxed); // 读自己：relaxed
    assert(writeIdx_.load(std::memory_order_acquire) != readIdx &&
           "Can only call pop() after front() has returned a non-nullptr"); // only compile for debug build
    slots_[readIdx + kPadding].~T(); // 先弹出析构。
    auto nextReadIdx = readIdx + 1;
    if (nextReadIdx == capacity_) {
      nextReadIdx = 0; // 指针回绕
    }
    readIdx_.store(nextReadIdx, std::memory_order_release); // 发布
  }

private:
  static constexpr size_t kCacheLineSize =
      std::hardware_destructive_interference_size; // 无则 64
  static constexpr size_t kPadding = (kCacheLineSize - 1) / sizeof(T) + 1;

  size_t capacity_;
  T *slots_;

  // 各占缓存行，避免 false sharing
  // Cache 字段是对方索引的本地快照，用于减少跨核缓存一致性流量
  alignas(kCacheLineSize) std::atomic<size_t> writeIdx_ = {0}; // 仅 producer 写
  alignas(kCacheLineSize) size_t readIdxCache_ = 0;            // producer 本地快照
  alignas(kCacheLineSize) std::atomic<size_t> readIdx_ = {0};  // 仅 consumer 写
  alignas(kCacheLineSize) size_t writeIdxCache_ = 0;           // consumer 本地快照
};
```

**1. 唯一读者/写者**。每个原子变量只有一方写。在读者 pop 时，`readIdx_` 只会被自己读写，不需要施加任何内存序限制；只需确保读 `writeIdx_` 时使用了正确内存序（acquire）；写者同理。

**2. release/acquire 配对**。两条同步链，方向相反、机制相同（有写后读因果的变量用 release 写 + acquire 读；release 为发布、acquire 为索取）：

- 生产侧：写槽位 → `writeIdx_.store(release)` ⇢ 消费者 `writeIdx_.load(acquire)` → 读槽位。release 保证槽位写先于索引发布对其他核可见；acquire 保证之后的读不重排到 acquire 之前——先读到新索引、再读新数据；
- 消费侧：读槽位 → `readIdx_.store(release)` ⇢ 生产者 `readIdx_.load(acquire)` → 覆写槽位。保证消费者读完槽位后生产者才覆写，否则数据竞争。

**3. 自己读自己的变量用 relaxed**。读自己刚写的值，单写者下必是自己上次写出的值，无竞争。

![内存序：release 发布 / acquire 索取]({{ site.baseurl }}/assets/images/memory-order.png)

> 图片出处：Dmitry Vyukov, [*Applied ordering*](https://sites.google.com/site/1024cores/home/lock-free-algorithms/so-what-is-a-memory-model-and-how-to-cook-it/applied-ordering), 1024cores。

### 性能优化细节

##### 缓存行隔离

状态变量独占缓存行是关键（约提升 30%）；false sharing 消除后接近纯数据搬运成本。

##### Head/Tail Cache

动机：atomic load 本身有代价（跨核缓存行拉取）。机制：把对端索引的"快照"缓存到本地（`readIdxCache_` 是生产者的、`writeIdxCache_` 是消费者的——谁缓存"对方的索引"归谁管），仅当缓存值与下一索引相等（可能满）时才 acquire 读对端索引刷新，否则不触碰对端缓存行。正确性边界：快照只用于"还有空间"这类乐观判断，过期只会多触发一次刷新，绝不误判"已满"——纯性能优化，不改变语义。

##### 官方 benchmark

AMD Ryzen 9 3900X，同 chiplet 不同核，`int` 元素（rigtorp README）：

| 队列 | 吞吐（ops/ms） | 延迟 RTT（ns） |
|------|---------------|---------------|
| SPSCQueue | **362723** | **133** |
| boost::lockfree::spsc | 209877 | 222 |
| folly::ProducerConsumerQueue | 148818 | 147 |

## 绝对指针掩码式（Vyukov）

### 原理概述

内存布局与回转式同构（状态变量独占缓存行、cached 副本、槽位无填充），仅字段命名不同（`enqueue_pos_`/`dequeue_pos_`），见上节。

核心思想：**绝对值索引**——`enqueue_pos_` 表示**总入队元素数量**、`dequeue_pos_` 表示**总出队元素数量**，单调递增永不回绕。队列元素位于 `[dequeue_pos_ % capacity_, enqueue_pos_ % capacity_)`；由于取模运算较慢，限制 capacity_ 为 2 的幂次，取模优化为 `&` 位运算（`pos & mask`）。

要点：

- **单调计数器 + 掩码取槽**：`pos & mask_` 定位槽位（`mask_ = buffer_size - 1`，要求容量 2 的幂）。计数器 64 位单调增长，永不回绕；
- **满/空差值判定**：在队元素数 `enqueue_pos_ - dequeue_pos_`；等于容量即满、等于 0 即空，天然无歧义；
- **容量满用**：差值判定无需 slack 元素，全部槽位可装数据。

### ⭐ 内存序正确性分析

以下面 SPSC 特化完整实现为分析对象（参考 Vyukov 原版 `mpmc_bounded_queue` 改写，省去 CAS 与 sequence 戳）：

```cpp
template <typename T>
class spsc_bounded_queue {
public:
  spsc_bounded_queue(size_t buffer_size)
      : buffer_(new cell_t[buffer_size]), buffer_mask_(buffer_size - 1) {
    assert((buffer_size >= 2) &&
           ((buffer_size & (buffer_size - 1)) == 0)); // 必须 2 的幂
    enqueue_pos_.store(0, std::memory_order_relaxed);
    dequeue_pos_.store(0, std::memory_order_relaxed);
  }

  bool enqueue(T const& data) {
    size_t pos = enqueue_pos_.load(std::memory_order_relaxed); // 读自己：relaxed
    cell_t* cell = &buffer_[pos & buffer_mask_];               // 掩码取槽
    while (pos - dequeue_pos_.load(std::memory_order_acquire) >= buffer_size) {
      // 满：在队元素数达到容量，自旋等待消费者推进 dequeue_pos_
    }
    cell->data_ = data;
    enqueue_pos_.store(pos + 1, std::memory_order_release);    // 发布：绝对值 +1
    return true;
  }

  bool dequeue(T& data) {
    size_t pos = dequeue_pos_.load(std::memory_order_relaxed); // 读自己：relaxed
    cell_t* cell = &buffer_[pos & buffer_mask_];               // 掩码取槽
    while (pos == enqueue_pos_.load(std::memory_order_acquire)) {
      // 空：在队元素数为 0，自旋等待生产者推进 enqueue_pos_
    }
    data = cell->data_;
    dequeue_pos_.store(pos + 1, std::memory_order_release);    // 发布：绝对值 +1
    return true;
  }

private:
  struct cell_t {
    T data_;
  };

  static size_t const cacheline_size = 64;
  typedef char cacheline_pad_t[cacheline_size];
  cacheline_pad_t pad0_;                              // 与相邻分配隔离
  cell_t* const buffer_;
  size_t const buffer_mask_;
  cacheline_pad_t pad1_;                              // 状态变量各占独立缓存行
  std::atomic<size_t> enqueue_pos_;
  cacheline_pad_t pad2_;
  std::atomic<size_t> dequeue_pos_;
  cacheline_pad_t pad3_;
};
```

**1. 位置计数器是同步载体**。原版 MPMC 由每槽 `sequence_` 的 release/acquire 配对同步数据（CAS 抢占与数据写入之间有空窗，只能靠槽位戳承载）；SPSC 特化省去 sequence 后，位置计数器自身承担：生产者写数据 → `enqueue_pos_.store(release)`；消费者 `enqueue_pos_.load(acquire)` → 读数据——读到新 `enqueue_pos_` 即保证新数据可见。

**2. 掩码取槽正确**。`pos & mask_` 等价 `pos mod capacity`（2 的幂），计数器永不回转。

**3. 满/空差值判定无歧义**。在队元素数由绝对值差值给出，无需 slack 元素，容量满用。

**代价**：

- **2 的幂容量约束**：`mask = capacity - 1` 要求容量 2 的幂；
- **64 位防溢出**：计数器需足够大，实践中 64 位安全。

### 性能优化细节

- **SPSC 特化消除 CAS**：单写者侧无竞争，去掉 CAS 抢占循环，路径上只剩原子 store/load；
- **cached 优化可叠加**：生产者缓存 `dequeue_pos_`、消费者缓存 `enqueue_pos_`，减少跨核 acquire 读。

## 两种风格的使用场景区别

| 维度 | 指针回转式（rigtorp） | 绝对指针掩码式（Vyukov） |
|------|----------------------|---------------------|
| 容量约束 | **任意容量 ≥ 1，可灵活适配动态容量需求** | **必须 2 的幂，否则取模导致性能劣化。容量设置不够灵活** |
| 容量利用 | 牺牲 1 槽（capacity() = 内部 - 1） | 满用 |
| 状态积累 | 指针始终在 `[0, capacity)` 内，永不溢出 | 绝对指针长期运行后溢出（64 位也终会耗尽），导致错误 |

---

> **小结**：两种 SOTA 实现共享同一骨架——唯一写者的原子状态、release/acquire 同步、缓存行隔离。差异仅为索引机制：回转式让计数器回绕，以牺牲一槽换取任意容量与等值判定；掩码式让寻址回绕，以 2 的幂约束换取满用容量与 MPMC 演进潜力。选型看容量约束、接口形态与扩展规划，性能上两者都逼近"纯数据搬运"成本下限。
