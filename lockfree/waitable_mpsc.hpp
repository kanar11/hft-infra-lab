/*
 * WaitableMPSCQueue<T, SIZE> — MPSCQueue with a blocking pop_wait(timeout).
 *
 * Wraps lockfree::MPSCQueue and adds an optional condition variable so the
 * consumer can sleep when the queue is empty instead of busy-spinning.
 *
 * Fast path stays lock-free: push() does push to the underlying MPSCQueue,
 * then a single atomic load on has_waiter_ — if no consumer is parked, it
 * skips the notify entirely (no syscall, no mutex). Only the slow path
 * (consumer about to sleep) takes the mutex.
 *
 * Use case: audit/journal flush threads — trading threads push at hot-path
 * speed, the flush thread sleeps when nothing is happening and wakes
 * within microseconds when an event lands.
 *
 * If you don't need blocking semantics, prefer MPSCQueue directly — the
 * notify-load in push() is cheap but not free.
 */
#pragma once

#include "mpsc_queue.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>


namespace lockfree {

template <typename T, std::size_t SIZE>
class WaitableMPSCQueue {
    MPSCQueue<T, SIZE>      q_;
    std::mutex              mtx_;
    std::condition_variable cv_;
    std::atomic<bool>       has_waiter_{false};

public:
    WaitableMPSCQueue()                                    = default;
    WaitableMPSCQueue(const WaitableMPSCQueue&)            = delete;
    WaitableMPSCQueue& operator=(const WaitableMPSCQueue&) = delete;
    WaitableMPSCQueue(WaitableMPSCQueue&&)                 = delete;
    WaitableMPSCQueue& operator=(WaitableMPSCQueue&&)      = delete;

    bool push(const T& item) noexcept {
        const bool ok = q_.push(item);
        if (ok && has_waiter_.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lk(mtx_);
            cv_.notify_one();
        }
        return ok;
    }

    bool try_pop(T& out) noexcept { return q_.pop(out); }

    template <typename Rep, typename Period>
    bool pop_wait(T& out, std::chrono::duration<Rep, Period> timeout) {
        if (q_.pop(out)) return true;  // fast path

        std::unique_lock<std::mutex> lk(mtx_);
        has_waiter_.store(true, std::memory_order_release);
        const bool woke = cv_.wait_for(lk, timeout, [&]() { return !q_.empty(); });
        has_waiter_.store(false, std::memory_order_release);
        lk.unlock();

        return woke && q_.pop(out);
    }

    bool empty() const noexcept { return q_.empty(); }
    static constexpr std::size_t capacity() noexcept { return SIZE; }
};

}  // namespace lockfree
