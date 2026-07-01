/*
 * TimestampRing — shared mechanics for sliding-time-window rate limiting /
 * rate metering.
 *
 * Before this header existed, the same fixed-capacity power-of-2 ring of
 * int64 timestamps (mask-wrap indices, no heap, no pointer chasing) was
 * hand-copied into RiskManager's order-rate limiter (risk/risk_manager.hpp)
 * and multicast's FeedRateMeter / SlidingWindowRate (multicast/gap_recovery.hpp).
 * This factors out just the ring mechanics; each call site keeps its own
 * domain logic (reject-on-limit, peak tracking, ...) on top.
 */
#pragma once

#include <cstddef>
#include <cstdint>

template <std::size_t N>
class TimestampRing {
    static_assert(N > 0 && (N & (N - 1)) == 0, "TimestampRing capacity must be a power of 2");
    static constexpr std::size_t MASK = N - 1;

public:
    // Evict all entries <= cutoff_ns (advances head_). Call before count()
    // to get the count within a [cutoff_ns, +inf) window.
    void evict(std::int64_t cutoff_ns) noexcept {
        while (head_ != tail_ && ts_[head_] <= cutoff_ns)
            head_ = (head_ + 1) & MASK;
    }

    // Insert now_ns as the newest entry. If already at capacity, drops the
    // oldest entry first — bounded memory, saturates instead of growing.
    void push(std::int64_t now_ns) noexcept {
        ts_[tail_] = now_ns;
        tail_ = (tail_ + 1) & MASK;
        if (tail_ == head_) head_ = (head_ + 1) & MASK;
    }

    std::size_t count() const noexcept {
        return static_cast<std::size_t>((tail_ - head_ + N) & MASK);
    }

    bool empty() const noexcept { return head_ == tail_; }
    void reset() noexcept { head_ = tail_ = 0; }

private:
    std::int64_t ts_[N] = {};   // value-init: cppcheck uninitMemberVar, never read before write
    std::size_t  head_  = 0;    // oldest entry
    std::size_t  tail_  = 0;    // next write slot
};
