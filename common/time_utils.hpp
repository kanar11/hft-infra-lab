/*
 * Two-clock timestamping helpers shared across modules.
 * Wspólne pomocniki znaczników czasu z dwóch zegarów.
 *
 *   mono_ns()  — CLOCK_MONOTONIC, latency measurement (never jumps with NTP)
 *   wall_ns()  — CLOCK_REALTIME,  UTC epoch, regulatory timestamps (MiFID II)
 */
#pragma once

#include <chrono>
#include <ctime>
#include <cstdint>


inline int64_t mono_ns() noexcept {
    auto now = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();
}

inline int64_t wall_ns() noexcept {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec * 1'000'000'000LL + ts.tv_nsec;
}
