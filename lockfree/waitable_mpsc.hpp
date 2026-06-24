/*
 * WaitableMPSCQueue<T, SIZE> — an MPSCQueue with a blocking pop_wait(timeout).
 *
 * A wrapper over lockfree::MPSCQueue + an optional condition variable so the
 * consumer can sleep when the queue is empty, instead of busy-spinning.
 *
 * The fast path stays lock-free: push() pushes into the MPSCQueue, then
 * one atomic-load on has_waiter_ — if nobody is waiting, we skip the
 * notify (zero syscalls, zero mutex). Only the slow path (a consumer that
 * goes to sleep) takes the mutex.
 *
 * Use case: threads flushing audit/journal — trading threads push at
 * hot-path speed, the flush thread sleeps when nothing happens and wakes
 * in microseconds when an event arrives.
 *
 * If you don't need the blocking semantics, use MPSCQueue directly
 * — the notify-load in push() is cheap but not free (1 cache miss).
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
        if (q_.pop(out)) return true;  // fast path — we don't go to sleep

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
