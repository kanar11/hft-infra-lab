/*
 * POVExecutor — Percentage-of-Volume execution algo (expansion #99).
 *
 * TWAP tnie zlecenie po CZASIE, VWAP po profilu wolumenu z gory; POV reaguje
 * ADAPTACYJNIE na biezacy wolumen rynku: w kazdym interwale wysyla child =
 * participation_rate * zaobserwowany wolumen, az wyczerpie zlecenie-rodzica.
 * Dzieki temu uczestniczy proporcjonalnie do plynnosci — wiecej gdy rynek
 * aktywny, mniej gdy cichy (mniejszy market impact, mniej sygnalizacji).
 *
 *   child = round(rate * market_volume), clamp do pozostalej ilosci rodzica
 *
 * Header-only, deterministyczny, zero alokacji. Caller karmi on_market_volume()
 * obserwowanym wolumenem (np. z TradeMsg/Executed) i wysyla zwrocony child.
 */
#pragma once

#include <cstdint>

class POVExecutor {
    int64_t  remaining_;   // ile rodzica jeszcze do wykonania
    int64_t  total_;       // pierwotna ilosc rodzica
    int64_t  executed_;    // ile wyslane w childach
    double   rate_;        // udzial w wolumenie [0..1]
    uint64_t slices_;      // ile childow wygenerowano

public:
    // parent_qty: calkowita ilosc do wykonania; participation_rate: np. 0.10 = 10%.
    POVExecutor(int64_t parent_qty, double participation_rate) noexcept
        : remaining_(parent_qty > 0 ? parent_qty : 0),
          total_(remaining_),
          executed_(0),
          rate_(participation_rate < 0.0 ? 0.0 : (participation_rate > 1.0 ? 1.0 : participation_rate)),
          slices_(0) {}

    // on_market_volume: zaobserwowany wolumen w tym interwale → rozmiar childa.
    // 0 gdy rodzic wyczerpany, brak wolumenu, albo rate*vol < 0.5 (zbyt malo by
    // sensownie uczestniczyc w tym interwale).
    int64_t on_market_volume(int64_t market_volume) noexcept {
        if (remaining_ <= 0 || market_volume <= 0) return 0;
        int64_t child = static_cast<int64_t>(rate_ * static_cast<double>(market_volume) + 0.5);
        if (child <= 0) return 0;
        if (child > remaining_) child = remaining_;   // nie przekrocz rodzica
        remaining_ -= child;
        executed_  += child;
        ++slices_;
        return child;
    }

    bool     done()       const noexcept { return remaining_ <= 0; }
    int64_t  remaining()  const noexcept { return remaining_; }
    int64_t  executed()   const noexcept { return executed_; }
    uint64_t slices()     const noexcept { return slices_; }
    // realized_participation: faktyczny udzial = wykonane / rodzic (sanity).
    double   completion() const noexcept {
        return total_ > 0 ? static_cast<double>(executed_) / static_cast<double>(total_) : 0.0;
    }
};
