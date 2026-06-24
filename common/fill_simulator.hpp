/*
 * FillSimulator — models realistic exchange behavior when executing orders.
 *
 * The lab's default model ("every submit = 100% fill at the quoted price") is
 * COMPLETELY UNREALISTIC. A real exchange has:
 *
 *   1. Partial fills      — the venue gives 60 shares on a request for 100, the rest resting.
 *   2. Slippage           — execution at a worse price than the quote (the market moved
 *                            between sending the order and execution).
 *   3. Market impact      — a LARGE order moves the price itself. Almgren-Chriss model:
 *                            impact_bps = coefficient * sqrt(qty / ADV)
 *                            (ADV = Average Daily Volume).
 *   4. Rejection          — the venue rejects: price out of band, locked market,
 *                            throttle (>N orders/sec), credit check, halt.
 *
 * The class exposes a clean probabilistic model — deterministic with a
 * fixed seed (for tests / replay) and configurable for different market
 * conditions (calm vs volatile).
 *
 * Usage:
 *   FillSimulator sim(config, seed=42);
 *   FillResult r = sim.simulate(side, requested_qty, quoted_price_ticks,
 *                                displayed_size, urgency);
 *   if (r.rejected) // venue REJECT — log + skip
 *   else            // r.fill_qty <= requested_qty, r.fill_price_ticks ±slippage
 *
 * A pass-through component — no persistent state besides the RNG. The consumer
 * (OMS / strategy demo) decides what to do with the remainder (leave as rest /
 * cancel / retry).
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>   // std::llabs (POSIX/C-style; not in <cmath>)

#include "types.hpp"   // Side enum


namespace common {

// `Side` lives in the global namespace (common/types.hpp) — used directly.


struct FillSimulatorConfig {
    // Partial fill — the probability that we get LESS than requested.
    // 0.0 = always full fill, 0.3 = 30% chance of a partial.
    double partial_fill_prob   = 0.20;

    // Min fill ratio on a partial — 0.5 = we get 50-100% of requested.
    double min_partial_ratio   = 0.50;

    // Slippage — a random price offset in ticks (exponential distribution density).
    // mean = the average in ticks; with mean=0 there is no slippage.
    // urgency multiplies this parameter: marketable=aggressive goes into the book and
    // sweeps it viciously; a passive limit waits → less slippage.
    double slippage_mean_ticks = 1.0;

    // Market impact — coefficient in the Almgren-Chriss permanent impact formula.
    // impact_bps = coeff * sqrt(qty / typical_size). typical_size ~ visible
    // liquidity. For orders << displayed_size impact ~0, for >> it rises sharply.
    double market_impact_coeff = 5.0;   // bps per sqrt(qty_ratio)

    // Rejection — the probability of a REJECT from the exchange. Real exchanges have a
    // baseline of 1-2% (locked market, throttle, halt).
    double reject_prob         = 0.02;
};


struct FillResult {
    bool     rejected;            // true → venue REJECT, nothing executed
    int32_t  fill_qty;            // how much was actually filled (≤ requested)
    int64_t  fill_price_ticks;    // execution price in ticks (with slippage + impact)
    int64_t  slippage_ticks;      // how far it moved from the quote (signed: + = worse for us)
    double   impact_bps;          // market impact in basis points (informational)

    FillResult() noexcept : rejected(false), fill_qty(0), fill_price_ticks(0),
                             slippage_ticks(0), impact_bps(0.0) {}
};


// Urgency — how aggressive our order is. Mainly affects slippage.
enum class Urgency : uint8_t {
    PASSIVE      = 0,   // limit order, we wait for a fill (small slippage)
    MARKETABLE   = 1,   // limit price = best bid/ask (default)
    AGGRESSIVE   = 2,   // limit crosses → market order (larger slippage)
};


class FillSimulator {
    FillSimulatorConfig cfg_;
    uint64_t            rng_state_;

    // LCG — deterministic, fast, no dependencies (replay-friendly).
    uint64_t next() noexcept {
        rng_state_ = rng_state_ * 6364136223846793005ULL + 1442695040888963407ULL;
        return rng_state_ >> 16;
    }

    double rand01() noexcept {
        return static_cast<double>(next() & 0xFFFFFFFF) / 4294967296.0;
    }

    // exp_random: a sample from an exponential distribution with mean `mean`.
    double exp_random(double mean) noexcept {
        const double u = rand01();
        return -std::log(u > 1e-12 ? u : 1e-12) * mean;
    }

public:
    explicit FillSimulator(const FillSimulatorConfig& cfg = {}, uint64_t seed = 42) noexcept
        : cfg_(cfg), rng_state_(seed ? seed : 1) {}

    // simulate: one potential execution. Returns a FillResult.
    //
    //   side              — BUY/SELL (slippage direction: BUY pays more, SELL gets less)
    //   requested_qty     — how many shares we want to execute
    //   quoted_price_ticks — the price from the quote (mid or best bid/ask)
    //   displayed_size    — visible liquidity at this level (for impact)
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

        // 2. Partial fill — with probability partial_fill_prob, a qty reduction.
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
        // bps → ticks conversion: 1 bp = 0.01% of price. price=22381 ticks, 1 bp = 2.24 ticks.
        const double impact_ticks_d = (impact_bps / 10000.0) * static_cast<double>(quoted_price_ticks);

        // Slippage + impact always AGAINST us. BUY → higher price, SELL → lower.
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
