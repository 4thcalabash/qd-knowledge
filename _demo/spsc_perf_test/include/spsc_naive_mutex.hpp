// 版本 1：最 naive 的 SPSC —— std::mutex + std::deque
// 无界队列，push/pop 每次全量加锁；push_back 可能触发堆分配。
// 后续版本将依次修复：容量、缓存行共享、原子操作。
#pragma once

#include <cstdint>
#include <deque>
#include <mutex>

namespace spsc {

template <typename T>
class Queue {
public:
    explicit Queue(std::size_t) {}  // deque 无界，容量参数忽略
    Queue(const Queue&) = delete;
    Queue& operator=(const Queue&) = delete;

    void push(const T& v) {
        std::lock_guard<std::mutex> lock(m_);
        q_.push_back(v);
    }

    bool pop(T& v) {
        std::lock_guard<std::mutex> lock(m_);
        if (q_.empty())
            return false;
        v = q_.front();
        q_.pop_front();
        return true;
    }

private:
    std::mutex m_;
    std::deque<T> q_;
};

}  // namespace spsc
