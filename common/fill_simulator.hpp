/*
 * FillSimulator — modeluje realne zachowanie giełdy przy egzekucji zleceń.
 *
 * Domyślny model w labie ("każdy submit = fill 100% po quoted price") jest
 * KOMPLETNIE NIEREALISTYCZNY. Prawdziwa giełda:
 *
 *   1. Partial fills      — venue daje 60 akcji na request 100, reszta resting.
 *   2. Slippage           — egzekucja po gorszej cenie niż quote (market moved
 *                            między wysłaniem zlecenia a wykonaniem).
 *   3. Market impact      — DUŻE zlecenie samo rusza cenę. Model Almgren-Chriss:
 *                            impact_bps = coefficient * sqrt(qty / ADV)
 *                            (ADV = Average Daily Volume).
 *   4. Rejection          — venue odrzuca: price out of band, locked market,
 *                            throttle (>N orders/sec), credit check, halt.
 *
 * Klasa udostępnia czysty model probabilistyczny — deterministyczny przy
 * stałym seed (do testów / replay) i konfigurowalny do różnych warunków
 * rynkowych (calm vs volatile).
 *
 * Użycie:
 *   FillSimulator sim(config, seed=42);
 *   FillResult r = sim.simulate(side, requested_qty, quoted_price_ticks,
 *                                displayed_size, urgency);
 *   if (r.rejected) // venue REJECT — log + skip
 *   else            // r.fill_qty <= requested_qty, r.fill_price_ticks ±slippage
 *
 * To pass-through component — nie ma persistent state poza RNG. Konsument
 * (OMS / strategy demo) decyduje co zrobić z resztą (zostawić jako rest /
 * cancel / retry).
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>   // std::llabs (POSIX/C-style; nie w <cmath>)

#include "types.hpp"   // Side enum


namespace common {

// `Side` żyje w globalnym namespace (common/types.hpp) — używamy bezpośrednio.


struct FillSimulatorConfig {
    // Partial fill — prawdopodobieństwo że dostaniemy MNIEJ niż request.
    // 0.0 = zawsze full fill, 0.3 = 30% szans na partial.
    double partial_fill_prob   = 0.20;

    // Min ratio wypełnienia przy partial — 0.5 = dostajemy 50-100% requested.
    double min_partial_ratio   = 0.50;

    // Slippage — losowy offset ceny w tickach (gęstość rozkładu wykładnicza).
    // mean = średnia w tickach; przy mean=0 brak slippage'u.
    // urgency mnoży ten parametr: marketable=aggressive idzie do księgi i je
    // jadowicie wybiera; passive limit czeka → mniej slippage.
    double slippage_mean_ticks = 1.0;

    // Market impact — coefficient w Almgren-Chriss permanent impact formula.
    // impact_bps = coeff * sqrt(qty / typical_size). Typical_size ~ widoczna
    // płynność. Dla zleceń << displayed_size impact ~0, dla >> rośnie ostro.
    double market_impact_coeff = 5.0;   // bps per sqrt(qty_ratio)

    // Rejection — prawdopodobieństwo REJECT z giełdy. Realne giełdy mają
    // baseline 1-2% (locked market, throttle, halt).
    double reject_prob         = 0.02;
};


struct FillResult {
    bool     rejected;            // true → venue REJECT, nic się nie wykonało
    int32_t  fill_qty;            // ile faktycznie wypełnione (≤ requested)
    int64_t  fill_price_ticks;    // cena egzekucji w tickach (z slippage + impact)
    int64_t  slippage_ticks;      // ile odjechało od quote (signed: + = gorzej dla nas)
    double   impact_bps;          // market impact w basis points (informational)

    FillResult() noexcept : rejected(false), fill_qty(0), fill_price_ticks(0),
                             slippage_ticks(0), impact_bps(0.0) {}
};


// Urgency — jak agresywne jest nasze zlecenie. Wpływa głównie na slippage.
enum class Urgency : uint8_t {
    PASSIVE      = 0,   // limit order, czekamy na fill (mały slippage)
    MARKETABLE   = 1,   // limit price = best bid/ask (default)
    AGGRESSIVE   = 2,   // limit przebija → market order (większy slippage)
};


class FillSimulator {
    FillSimulatorConfig cfg_;
    uint64_t            rng_state_;

    // LCG — deterministyczny, szybki, bez zależności (replay-friendly).
    uint64_t next() noexcept {
        rng_state_ = rng_state_ * 6364136223846793005ULL + 1442695040888963407ULL;
        return rng_state_ >> 16;
    }

    double rand01() noexcept {
        return static_cast<double>(next() & 0xFFFFFFFF) / 4294967296.0;
    }

    // exp_random: próbka z rozkładu wykładniczego ze średnią `mean`.
    double exp_random(double mean) noexcept {
        const double u = rand01();
        return -std::log(u > 1e-12 ? u : 1e-12) * mean;
    }

public:
    explicit FillSimulator(const FillSimulatorConfig& cfg = {}, uint64_t seed = 42) noexcept
        : cfg_(cfg), rng_state_(seed ? seed : 1) {}

    // simulate: jedna potencjalna egzekucja. Zwraca FillResult.
    //
    //   side              — BUY/SELL (kierunek slippage'u: BUY płaci więcej, SELL dostaje mniej)
    //   requested_qty     — ile akcji chcemy wykonać
    //   quoted_price_ticks — cena z quote'a (mid albo best bid/ask)
    //   displayed_size    — widoczna płynność na tym poziomie (do impactu)
    //   urgency           — passive/marketable/aggressive
    FillResult simulate(Side side, int32_t requested_qty, int64_t quoted_price_ticks,
                        int32_t displayed_size = 1000,
                        Urgency urgency = Urgency::MARKETABLE) noexcept {
        FillResult r;
        if (requested_qty <= 0 || quoted_price_ticks <= 0) return r;

        // 1. Rejection.
        if (rand01() < cfg_.reject_prob) {
            r.rejected = true;
            return r;
        }

        // 2. Partial fill — z prawdopodobieństwem partial_fill_prob, qty redukcja.
        int32_t fill_qty = requested_qty;
        if (rand01() < cfg_.partial_fill_prob) {
            const double ratio = cfg_.min_partial_ratio
                              + rand01() * (1.0 - cfg_.min_partial_ratio);
            fill_qty = static_cast<int32_t>(requested_qty * ratio);
            if (fill_qty <= 0) fill_qty = 1;
        }

        // 3. Slippage — exp distribution, urgency multiplier.
        double urg_mult = 1.0;
        if      (urgency == Urgency::PASSIVE)    urg_mult = 0.2;
        else if (urgency == Urgency::AGGRESSIVE) urg_mult = 2.0;
        const double slippage = exp_random(cfg_.slippage_mean_ticks * urg_mult);

        // 4. Market impact — Almgren-Chriss permanent impact ~ sqrt(qty/typical).
        const double ratio_to_displayed = (displayed_size > 0)
            ? static_cast<double>(requested_qty) / displayed_size : 0.0;
        const double impact_bps = cfg_.market_impact_coeff * std::sqrt(ratio_to_displayed);
        // Konwersja bps → ticki: 1 bp = 0.01% ceny. cena=22381 ticków, 1 bp = 2.24 ticka.
        const double impact_ticks_d = (impact_bps / 10000.0) * static_cast<double>(quoted_price_ticks);

        // Slippage + impact zawsze NA NIEKORZYŚĆ. BUY → cena wyższa, SELL → niższa.
        const int64_t signed_offset = static_cast<int64_t>(slippage + impact_ticks_d);
        const int64_t penalty = (side == Side::BUY) ? signed_offset : -signed_offset;

        r.rejected         = false;
        r.fill_qty         = fill_qty;
        r.fill_price_ticks = quoted_price_ticks + penalty;
        if (r.fill_price_ticks < 1) r.fill_price_ticks = 1;
        r.slippage_ticks   = std::llabs(signed_offset);
        r.impact_bps       = impact_bps;
        return r;
    }

    const FillSimulatorConfig& config() const noexcept { return cfg_; }
};


}  // namespace common
