/*
 * SignalThrottle — a throttle for excessive trading (expansion #104).
 *
 * Strategies (mean-reversion, momentum, bollinger) can generate a signal on
 * EVERY tick while the condition holds — that is overtrading: commissions
 * eat the edge, and more orders on the same side only grow the position.
 * The throttle enforces a MINIMUM gap (in "ticks"/sequence) between accepted
 * signals per symbol. Reusable: it wraps any strategy on the caller's side.
 *
 *   allow(sym, seq) -> true when >= cooldown has elapsed since the last accepted
 *   signal for `sym`; then it remembers seq. Otherwise false (suppressed).
 */
#pragma once

#include "../common/symbol_key.hpp"

#include <cstdint>
#include <unordered_map>

class SignalThrottle {
    int64_t cooldown_;
    std::unordered_map<uint64_t, int64_t> last_fire_;   // sym_key -> seq of the last signal
    uint64_t suppressed_ = 0;

public:
    explicit SignalThrottle(int64_t cooldown) noexcept
        : cooldown_(cooldown > 0 ? cooldown : 0) {}

    // allow: whether to let a signal through for `sym` at the current sequence `seq`.
    // The first signal always passes; subsequent ones only after the cooldown.
    bool allow(const char* sym, int64_t seq) {
        const uint64_t k = sym_to_key(sym);
        const auto it = last_fire_.find(k);
        if (it == last_fire_.end() || (seq - it->second) >= cooldown_) {
            last_fire_[k] = seq;
            return true;
        }
        ++suppressed_;
        return false;
    }

    // reset_symbol: forget the state for a symbol (e.g. after the position goes flat).
    void reset_symbol(const char* sym) noexcept { last_fire_.erase(sym_to_key(sym)); }
    void reset() noexcept { last_fire_.clear(); suppressed_ = 0; }

    uint64_t suppressed() const noexcept { return suppressed_; }
    int64_t  cooldown()   const noexcept { return cooldown_; }
};
