/*
 * SignalThrottle — dławik nadmiernego handlu (expansion #104).
 *
 * Strategie (mean-reversion, momentum, bollinger) potrafia generowac sygnal na
 * KAZDYM ticku gdy warunek trzyma sie dluzej — to przehandlowanie: prowizje
 * zjadaja edge, a kolejne zlecenia w te sama strone tylko powiekszaja pozycje.
 * Throttle wymusza MINIMALNY odstep (w "tickach"/sekwencji) miedzy przyjetymi
 * sygnalami per symbol. Reuzywalny: owija dowolna strategie po stronie callera.
 *
 *   allow(sym, seq) -> true gdy od ostatniego przyjetego sygnalu dla `sym`
 *   uplynelo >= cooldown; wtedy zapamietuje seq. Inaczej false (stlumione).
 */
#pragma once

#include "../common/symbol_key.hpp"

#include <cstdint>
#include <unordered_map>

class SignalThrottle {
    int64_t cooldown_;
    std::unordered_map<uint64_t, int64_t> last_fire_;   // sym_key -> seq ostatniego sygnalu
    uint64_t suppressed_ = 0;

public:
    explicit SignalThrottle(int64_t cooldown) noexcept
        : cooldown_(cooldown > 0 ? cooldown : 0) {}

    // allow: czy przepuscic sygnal dla `sym` przy biezacej sekwencji `seq`.
    // Pierwszy sygnal zawsze przechodzi; kolejne dopiero po cooldown.
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

    // reset_symbol: zapomnij stan dla symbolu (np. po flacie pozycji).
    void reset_symbol(const char* sym) noexcept { last_fire_.erase(sym_to_key(sym)); }
    void reset() noexcept { last_fire_.clear(); suppressed_ = 0; }

    uint64_t suppressed() const noexcept { return suppressed_; }
    int64_t  cooldown()   const noexcept { return cooldown_; }
};
