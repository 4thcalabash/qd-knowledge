---
title: "SPSC的两种工业级SOTA实现"
date: 2026-08-10 14:00:00 +0800
model: DeepSeek-V4-Flash
tool: Claude Code CLI
---

SPSC（Single Producer Single Consumer）无锁队列是低延迟系统中核间通信的标准设施。工业界存在两种并行流传的 SOTA（state-of-the-art）实现，在内存布局上共享同一骨架，却在索引机制、满空编码与正确性论证上分道扬镳。本文从实现源码出发，对比两种风格的原理、正确性与性能特征，并给出使用场景的选型依据。

> 本文分析的两种实现，其原始出处为：
>
> - **指针回转式**（rigtorp::SPSCQueue）：[rigtorp/SPSCQueue](https://github.com/rigtorp/SPSCQueue)，Erik Rigtorp 著，MIT 协议。本文源码分析基于 [SPSCQueue.h](https://github.com/rigtorp/SPSCQueue/blob/master/include/rigtorp/SPSCQueue.h)。
> - **指针掩码式**（Vyukov 有界队列的 SPSC 特化）：[bounded-mpmc-queue](https://sites.google.com/site/1024cores/home/lock-free-algorithms/queues/bounded-mpmc-queue)（原 1024cores.net，Dmitry Vyukov 著）。Vyukov 本人未单独发表有界 SPSC 专文，该风格由社区从其有界 MPMC 队列推导特化。

- 目录
{:toc}

## 指针回转式（rigtorp::SPSCQueue）

### 原理概述

#### 内存布局

![SPSC 内存布局]({{ site.baseurl }}/assets/images/spsc-memory-layout.svg)

##### 状态变量区

队列的控制状态以 Head/Tail 两个原子索引为核心，各自以 `alignas(kCacheLineSize)` 独占一个缓存行（图中每个字段两侧的 Pad 即隔离带）。每个原子变量只有唯一写者——这是 SPSC 无锁的本质来源：不需要 CAS，普通 store/load 即可。

| 图中标签 | 源码字段 | 类型 | 谁写 | 谁读 |
|---------|---------|------|------|------|
| Head | `readIdx_` | `std::atomic<size_t>` | 消费者 | 生产者 |
| Tail | `writeIdx_` | `std::atomic<size_t>` | 生产者 | 消费者 |

**索引的开闭语义**：Tail 指向下一个待写入槽位、Head 指向下一个待读取槽位，队列元素位于半开区间 `[Head, Tail)`，元素数量为 `Tail - Head`。二者为 `size_t`，无符号溢出截断——索引回绕后差值仍正确。

图上另有 Head Cache / Tail Cache 两个字段，是 Head/Tail 的本地缓存副本，其动机与机制留待性能优化细节一节展开，此处不表。

##### 数据槽位区

数据槽位区无填充：`capacity_` 个槽位紧密排列，槽位之间**不做**缓存行填充，这是有意为之——数据流方向固定（生产者写槽位 → 消费者读槽位），多个小对象共行一次性搬运，无填充是特性而非缺陷。

##### 首尾填充 kPadding

数组首尾 `kPadding`（隔离堆邻接对象）：

```cpp
static constexpr size_t kPadding = (kCacheLineSize - 1) / sizeof(T) + 1;
```

`slots_` 是独立堆分配，首尾各 `kPadding` 个元素永不使用——避免数组最边上真实槽位与堆中相邻对象共享缓存行。公式按元素数计算：`(64-1)/sizeof(T)+1` 保证填充元素跨满至少一个缓存行，与 T 大小无关。

##### slack 元素

**slack 元素：有效容量比申请量少 1**。即使申请了 capacity 的有效空间（有效空间不包含首尾 padding 元素，见上），有效容量也只有 capacity-1——剩余一个槽位永不装数据，专门用于判断队列为满。

空/满判定由此确定：

- 空：`Tail == Head`（区间为空）；
- 满：`Tail + 1 == Head`（下一个待写入位置追上读位置）；
- 若满/空都允许全部槽位装数据，则"满"与"空"将出现同一种状态（`Tail == Head`），无法区分；slack 元素保证任何时刻至少有一个空槽，使 `Tail == Head` 唯一对应"空"——满/空从此无歧义。

构造时 `capacity_ = capacity + 1` 实现这一点，对外暴露 `capacity() = capacity_ - 1`。

### ⭐ 内存序正确性分析

正确性分析从具体变量的读写出发：关注每个变量的读写方，而不是笼统的"无锁"。

**1. 唯一读者/写者**。在读者 pop 时，`readIdx_` 只会被自己读写，不需要施加任何内存序限制；只需要读 `writeIdx_` 时使用了正确内存序（acquire）；写者同理。

**2. 跨线程变量使用 release/acquire 配对**。两个变量各自被对方线程读，形成两条 happens-before 同步链：

- 生产侧：写槽位（普通写）→ `writeIdx_.store(release)` ⇢ 消费者 `writeIdx_.load(acquire)` → 读槽位（普通读）。release 保证"槽位写"先于"索引发布"对其他核可见（不允许之前的写被指令重排到 release 指令之后）；acquire 解决读侧的对偶问题——不允许之后的读操作被重排到 acquire 指令之前，确保先读到新索引，再读新数据；
- 消费侧：读槽位 → `readIdx_.store(release)` ⇢ 生产者 `readIdx_.load(acquire)` → 覆写槽位。原理同上。

**总结：有写后读因果的原子变量，要用 release 写 + acquire 读（release 为发布、acquire 为索取）**。两条同步链是同一模式的两次应用：数据（槽位）与状态（索引）存在写后读因果——索引的发布依赖槽位的写入（写侧），索引的读取依赖数据的读取（读侧）。凡是这种因果关系的变量，写者 release、读者 acquire，SPSC 里正是 `writeIdx_` 和 `readIdx_`。

任一侧放松为 relaxed 即破坏同步链：读到对方索引的新值不保证对应数据可见。

**3. 自己读自己的变量用 relaxed**。消费者读 `readIdx_`、生产者读 `writeIdx_` 都是读自己刚写的值——单写者下读到的必是自己上次写出的值，无竞争，relaxed 足够。

**为什么 release/acquire 就是最优**：

- relaxed 不够：消费侧 relaxed 读到新 `writeIdx_` 不保证槽位数据可见——数据竞争；
- seq_cst 浪费：seq_cst 解决多写者仲裁（如无锁栈 push 竞争），SPSC 单写者无此需求。x86 上 release store / acquire load 是普通 `mov`，seq_cst store 需要 `xchg`/`mfence` 破坏 store buffer 合并；ARM64 上需要全屏障 `dmb.ish`。

![内存序：release 发布 / acquire 索取]({{ site.baseurl }}/assets/images/memory-order.png)

> 图片出处：Dmitry Vyukov, [*Applied ordering*](https://sites.google.com/site/1024cores/home/lock-free-algorithms/so-what-is-a-memory-model-and-how-to-cook-it/applied-ordering), 1024cores。

### 性能优化细节

##### 缓存行隔离

状态变量独占缓存行是关键（实验：紧邻同缓存行 4.90 s → 独占 3.80 s，快 29%）；false sharing 消除后接近纯数据搬运成本。

##### Head/Tail Cache

动机：满/空判定读对端索引，每次都是跨核缓存行拉取。机制：把对端索引缓存到本地（`readIdxCache_` 是生产者的缓存、`writeIdxCache_` 是消费者的——谁缓存"对方的索引"归谁管），仅当缓存值与下一索引相等（可能满）时才 acquire 读对端索引刷新；否则完全不触碰对端缓存行。正确性边界：缓存只用于"还有空间"这类乐观判断，过期只会多触发一次刷新，绝不误判"已满"——纯性能优化，不改变语义。

##### 官方 benchmark

AMD Ryzen 9 3900X，同 chiplet 不同核，`int` 元素（rigtorp README）：

| 队列 | 吞吐（ops/ms） | 延迟 RTT（ns） |
|------|---------------|---------------|
| SPSCQueue | **362723** | **133** |
| boost::lockfree::spsc | 209877 | 222 |
| folly::ProducerConsumerQueue | 148818 | 147 |

## 前进指针掩码式（Vyukov）

### 原理概述

内存布局与指针回转式同构：状态变量各占独立缓存行、cached 副本、数据槽位无填充，仅字段命名不同（`enqueue_pos_`/`dequeue_pos_` 等），见上节，不再重复。

核心思想：索引单调递增**永不回绕**，取槽用掩码。

```cpp
// SPSC 特化：单写者侧位置为普通变量（无需 CAS）
size_t pos = enqueue_pos_;                       // 非原子（SPSC 特化）
cell_t* cell = &buffer_[pos & mask_];            // mask_ = capacity_ - 1
while (cell->sequence_.load(std::memory_order_acquire) != pos) {
  // 槽位 sequence 戳 ≠ pos：该槽尚未被消费完，队列满，自旋
}
store_data(cell);
cell->sequence_.store(pos + 1, std::memory_order_release);
enqueue_pos_ = pos + 1;
```

- **单调计数器 + 掩码取槽**：`enqueue_pos_`/`dequeue_pos_` 只增不减，`pos & mask_` 定位槽位（`mask_ = buffer_size - 1`，要求容量为 2 的幂）。计数器值域 64 位，单调增长需数千年才溢出，永不回绕；
- **每槽 sequence 戳**：第 i 槽初始化 `sequence_[i] = i`；生产者写完数据 store `pos + 1`，消费者读完 store `pos + mask_ + 1`。满/空判定看**槽位戳与 pos 的差值**：差值为 0 说明该槽可占用，负值说明未消费完（满）或未生产完（空）；
- **容量可满用**：戳判定不需要 slack 元素，所有槽位都能装数据（capacity 即 buffer_size）；
- **SPSC 特化**：MPMC 原版用 CAS 竞争 `enqueue_pos_`，SPSC 单生产者下 `enqueue_pos_`/`dequeue_pos_` 各仅一方写，可降为普通变量——这正是作者所述"Accesses to data are synchronized by cell->sequence_. enqueue/dequeue_pos_ only arbitrate concurrent enqueue/dequeue requests."（作者在 lock-free 讨论组明确说明单生产者/单消费者时该侧位置可非原子）。

### ⭐ 内存序正确性分析

**1. sequence 戳是同步载体**。数据同步不靠位置计数器，靠每槽 `sequence_` 的 release/acquire 配对：生产者写数据 → `sequence_.store(release)`；消费者 `sequence_.load(acquire)` → 读数据。位置计数器只回答"轮到哪个槽"，不承载数据可见性。

**2. 掩码取槽正确**。`pos & mask_` 等价 `pos mod capacity`（2 的幂），单调递增 pos 与回绕寻址解耦：计数器永不回转，回转发生在寻址处。

**3. 满/空判定无歧义**。槽位戳携带"该槽最后一次被生产/消费的轮次"，`dif == 0` 可占用、负值满/空——不需要 slack 元素，容量可满用。

**代价**：

- **2 的幂容量约束**：`mask = capacity - 1` 要求容量为 2 的幂，非 2 幂无法用；
- **每槽一个原子状态**：`sequence_` 是 `std::atomic<size_t>`，槽位状态开销大（对齐 + 原子操作），且可用性检查要读**目标槽位的戳**（跨核读的是对端刚写的槽行）；
- **64 位防溢出**：计数器需足够大，实践中 64 位安全。

### 性能优化细节

- **SPSC 特化消除 RMW**：单写者侧位置降为非原子后，生产/消费路径无 CAS、无 RMW，与回转式同样只有 store/load；
- **cached 优化可叠加**：生产者本地缓存对端 `dequeue_pos_` 的副本、消费者缓存 `enqueue_pos_` 副本（回绕式用 `readIdxCache_`/`writeIdxCache_`，掩码式同理），减少每操作一次跨核 acquire 读；
- **结构上可扩展**：同一布局加回 CAS 即恢复 MPMC，加单侧 CAS 即 MPSC——掩码式从 MPSC 演进只需恢复 CAS 与竞争逻辑，而回转式的唯一写者论证依赖 SPSC，无法直接扩展；
- **衍生实现**：该风格被广泛移植，如 MySQL `mpmc_bq`、spdlog、Zephyr RTOS 的 MPSC、Rust crossbeam 的 `ArrayQueue`（无戳变体）。

## 两种风格的使用场景区别

| 维度 | 指针回转式（rigtorp） | 指针掩码式（Vyukov） |
|------|----------------------|---------------------|
| 索引语义 | `[0, capacity)` 回绕，等值判定满空 | 单调递增，掩码取槽，戳判定满空 |
| 容量约束 | **任意容量 ≥ 1** | **必须 2 的幂** |
| 容量利用 | 牺牲 1 槽（capacity() = 内部 - 1） | 满用 |
| 每槽状态 | 无（纯数据） | 每槽一个 atomic `sequence_` |
| 满空判定 | 读对端计数器（cached 优化后低频） | 读目标槽位戳 |
| 接口 | `front()`/`pop()` 分离，零拷贝 peek | 一体化 push/pop |
| 正确性基础 | 唯一写者 + release/acquire + slack 消歧 | sequence 戳同步 + 掩码取槽 |
| 可扩展性 | 仅 SPSC（论证依赖单写者） | 可恢复 CAS 演进为 MPSC/MPMC |

**选型判断**：

- **选指针回转式**：容量不能是 2 的幂（如业务容量恰好 10000）；需要 `front()`/`pop()` 分离的 peek/批读接口；需要最小每槽内存开销（槽位无戳）。
- **选指针掩码式**：容量天然 2 的幂（如 2^16）；需要满用全部槽位；有演进为 MPSC/MPMC 的规划；代码需要与现有掩码式家族（MPMC/MPSC）共用同一套状态管理心智。

---

> **小结**：SPSC 的两种工业级 SOTA 实现共享同一骨架——唯一写者的原子状态、release/acquire 同步、缓存行隔离。差异仅为索引机制：回转式让计数器回绕并牺牲一槽换取任意容量与等值判定；掩码式让寻址回绕并牺牲 2 的幂约束换取满用容量与 MPMC 演进潜力。选型依据容量约束、接口形态与扩展规划而定，性能上两者都逼近"纯数据搬运"成本下限。
>
> **待补**：两风格同机实测对比数据（同机、同元素、同容量档位跑 rigtorp 与掩码式实现）。
