---
title: "C++11 shared_ptr: 原理和使用规范"
date: 2026-07-30 22:00:00 +0800
---

`std::shared_ptr` 是 C++11 引入的共享所有权智能指针。用好它的关键不在于记住 API，而在于理解其内部原理和由此推导出的使用规范。本文聚焦这两点。

- 目录
{:toc}

## 实现原理

### 控制块架构

`shared_ptr` 内部由两块堆内存组成：

```
shared_ptr 对象 (栈)
    ├── ptr ────────► 被管理的对象
    └── ctrl_block ─► 控制块
                       ├── shared_count (引用计数)
                       └── deleter      (删除器，类型擦除)
```

**核心规则**：`shared_count` 降到 0 时析构被管理对象并释放控制块。

### 为什么控制块必须在堆上？

控制块之所以是堆分配，根源在于它的**生命周期不由任何一个 `shared_ptr` 单独决定**。

考虑一个简单场景：`sp1` 创建后拷贝出 `sp2`，然后 `sp1` 销毁——`sp2` 仍然需要通过控制块管理对象。如果控制块放在 `sp1` 的栈帧上或内嵌在 `sp1` 内部，`sp1` 销毁时控制块就没了，`sp2` 的引用计数指针变成悬垂指针。

更关键的是跨线程场景：

```cpp
void worker(std::shared_ptr<Foo> sp) {
    // sp 析构 → 控制块 → 如果是栈上的？灾难
}

auto sp = std::make_shared<Foo>();
std::thread t(worker, sp);  // 拷贝给了线程
// 主线程的 sp 可能先析构，但 worker 还没用完
```

控制块必须活得比任何一个 `shared_ptr` 都久——直到**最后一个**引用它的 `shared_ptr` 析构为止。这种「所有者离去但资源留下」的语义，只能用堆分配配合引用计数来实现。

`make_shared` 虽然把对象和控制块合并在同一块内存里，但本质还是堆分配——只是从两次合并为一次，控制块仍不在栈上。

### 引用计数的原子操作

```cpp
// 拷贝构造/赋值
shared_count.fetch_add(1, std::memory_order_relaxed);

// 析构/重置
if (shared_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    delete_object();
    delete_control_block();
}
```

加法用 `relaxed`，减法用 `acq_rel`——这个不对称设计是下文线程安全规范的根基。

### make_shared 的一次分配

`shared_ptr<T>(new T(...))` 触发两次堆分配（对象 + 控制块），`make_shared<T>(...)` 合并为一次：

```
make_shared 内存布局：
┌─────────────────────────────────┐
│   T object    │  control_block  │
└─────────────────────────────────┘
```

收益：少一次分配，缓存局部性更好。

> **规范**：除非需要自定义删除器，否则始终用 `make_shared`。

## 线程安全：分层分析

| 层面 | 线程安全？ | 说明 |
|------|-----------|------|
| 控制块的引用计数 | ✅ | 原子操作保证；遵守使用规范则引用计数与资源的申请/释放均安全，违规则 UB |
| shared_ptr 变量本身 | ❌ | 多个线程同时读写同一个变量是 data race |
| 被管理的对象 | ❌ | 只管理生命周期，不管内部状态同步 |

## 使用规范

### ⭐ 规范一：传递靠拷贝，不要共享变量

```cpp
auto sp = std::make_shared<int>(42);

// ✅ 每个线程按值捕获——各持独立副本
std::thread t1([sp] { /* ... */ });
std::thread t2([sp] { /* ... */ });

// ❌ 引用捕获——两个线程操作同一个 shared_ptr 变量
std::thread t1([&sp] { sp.reset();   });  // 写
std::thread t2([&sp] { auto p = sp; });  // 读 → UB
```

每个线程拿到独立副本，引用计数的增减全是原子操作，最后一个离开的线程安全释放对象。这是 `shared_ptr` 最核心的使用范式。

### 规范二：被管理对象的同步自理

```cpp
auto sp = std::make_shared<std::vector<int>>();
// ❌ 多线程同时 push_back——vector 不是线程安全的
std::thread t1([sp] { sp->push_back(1); });
std::thread t2([sp] { sp->push_back(2); });

// ✅ 对象内部带锁
struct SafeVec {
    std::mutex mtx;
    std::vector<int> data;
    void push(int v) { std::lock_guard lk(mtx); data.push_back(v); }
};
```

`shared_ptr` 只保证"最后一个离开的人关灯"，不保证"屋里的人不打架"。对象内部的并发访问需要另外的同步机制。

### 规范三：需要动态替换时用 atomic

如果确实需要多线程替换同一个 `shared_ptr` 的指向，C++20 提供了 `std::atomic<std::shared_ptr<T>>`：

```cpp
std::atomic<std::shared_ptr<int>> asp = std::make_shared<int>(42);

asp.store(std::make_shared<int>(100));  // 写
auto local = asp.load();                 // 读
```

内部在支持 128-bit CAS 的平台上（如 x86-64 的 `cmpxchg16b`）可做到真正无锁。可以通过 `asp.is_lock_free()` 运行时确认。C++11 的 `atomic_load`/`atomic_store` 自由函数也能用，但内部通常用 mutex。

大多数场景不需要这个——让每个线程持有自己的副本就够了。

## 内存序选择的原理

这是推导上述规范的关键。为什么引用计数 `+1` 用 `relaxed`，`-1` 用 `acq_rel`？

### ⭐ fetch_add 用 relaxed

拷贝时只需要记录"多了一个引用"，这件事不需要和任何其他内存操作建立顺序关系。拷贝发生在同一线程的程序顺序中；跨线程传递时，同步由 `std::thread` 构造函数、mutex 等机制保证——不需要引用计数操作越俎代庖。`relaxed` 不插入内存屏障，开销极低。

### fetch_sub 用 acq_rel

当 `fetch_sub` 返回 1 时，当前线程是最后一个持有者，即将 delete 对象。此时需要：

- **release**：确保本线程的写入能被后续 acquire 看到
- **acquire**：确保能看到之前所有释放者的写入

每次 `fetch_sub(release)` 和最后一次的 `fetch_sub(acquire)` 形成一条 release-acquire 链：

```
线程 A                          线程 B（最后一个析构）
  写入对象数据
  spA 析构
  fetch_sub(release) ───────►  fetch_sub(acquire) → 返回 1
                                 读取数据 ✓——能看到 A 的写入
                                 delete 对象
```

注意这条链主要保护控制块内部状态的可见性，被管理对象内部的同步仍是用户责任。

## 并发边界：最后一个析构 vs. 并发拷贝

最后一个引用正在析构时，另一个线程能否安全地增加引用计数？

- **从同一个 `shared_ptr` 变量拷贝**：UB。两个线程同时读写同一变量，这不是引用计数的锅，而是变量本身并发读写的问题。
- **从另一个活副本拷贝**：不会发生。存在活副本意味着 `shared_count ≥ 2`，`fetch_sub` 不可能返回 1。逻辑自洽。

---

> **一口诀**：`shared_ptr` 保证「最后一个离开的人关灯」，不保证「屋里的人不打架」。跨线程靠拷贝传递，变量本身不共享引用，对象内部同步自理。
