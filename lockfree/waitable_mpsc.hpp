/*
 * WaitableMPSCQueue<T, SIZE> — MPSCQueue z blokującym pop_wait(timeout).
 *
 * Wrapper na lockfree::MPSCQueue + opcjonalny condition variable żeby
 * konsument mógł spać gdy kolejka pusta, zamiast busy-spinować.
 *
 * Fast path pozostaje lock-free: push() robi push do MPSCQueue, potem
 * jeden atomic-load na has_waiter_ — jeśli nikt nie czeka, pomijamy
 * notify (zero syscalli, zero mutexa). Tylko slow path (konsument który
 * zasypia) bierze mutex.
 *
 * Use case: wątki flush'ujące audit/journal — wątki tradingowe push'ują
 * na hot-path speed, flush thread śpi gdy nic się nie dzieje i budzi się
 * w mikrosekundach gdy spadnie event.
 *
 * Jeśli nie potrzebujesz blokującej semantyki, użyj MPSCQueue bezpośrednio
 * — notify-load w push() jest tani ale nie za darmo (1 cache miss).
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
        if (q_.pop(out)) return true;  // fast path — nie idziemy spać

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
