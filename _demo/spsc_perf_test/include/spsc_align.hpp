// 版本 3：无锁 SPSC —— folly 风格 + cached head/tail + tail_/head_/cached* 缓存行全分隔
// 与 spsc_noalign 唯一区别：tail_/head_/cachedHead_/cachedTail_ 各自 alignas(64) 独占缓存行，
// 两个物理核各写各的行，不乒乓。
#pragma once

#include <atomic>
#include <cstdint>

namespace spsc {

template <typename T>
class Queue {
public:
    explicit Queue(std::size_t size)
        : buf_(new T[size]), size_(size), tail_(0),
          head_(0), cachedHead_(0), cachedTail_(0) {}

    Queue(const Queue&) = delete;
    Queue& operator=(const Queue&) = delete;

    ~Queue() { delete[] buf_; }

    std::size_t capacity() const { return size_ - 1; }

    void push(const T& v) {
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        std::size_t next = t + 1;
        if (next == size_) next = 0;
        if (next == cachedHead_) {
            cachedHead_ = head_.load(std::memory_order_acquire);
            if (next == cachedHead_) {
                do { } while (next == head_.load(std::memory_order_acquire));
                cachedHead_ = head_.load(std::memory_order_acquire);
            }
        }
        buf_[t] = v;
        tail_.store(next, std::memory_order_release);
    }

    bool pop(T& v) {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        if (h == cachedTail_) {
            cachedTail_ = tail_.load(std::memory_order_acquire);
            if (h == cachedTail_)
                return false;  // 空
        }
        v = buf_[h];
        std::size_t next = h + 1;
        if (next == size_) next = 0;
        head_.store(next, std::memory_order_release);
        return true;
    }

private:
    T* buf_;
    std::size_t size_;
    alignas(64) std::atomic<std::size_t> tail_;   // 仅 producer 写，独占缓存行
    alignas(64) std::atomic<std::size_t> head_;   // 仅 consumer 写，独占缓存行
    alignas(64) std::size_t cachedHead_;          // producer 本地缓存，独占缓存行
    alignas(64) std::size_t cachedTail_;          // consumer 本地缓存，独占缓存行
    
};

}  // namespace spsc
