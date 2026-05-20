/*
 * mm_demo — simulate a market with a random-walk mid price, run our
 * MarketMaker against it, and report quote churn / inventory / P&L.
 *
 * Synthetic market model
 * ----------------------
 * Each tick the mid drifts ±1 tick with small probability, otherwise stays.
 * best_bid = mid - 1, best_ask = mid + 1 (a tight one-tick reference spread).
 *
 * Adversary model: with probability `cross_prob` per tick, an aggressive
 * order arrives on a random side and crosses the maker's quote, paying
 * the maker's posted price. Half of those become BUY (we sold), half SELL
 * (we bought).
 *
 * P&L is mark-to-market at the final mid.
 */
#include "market_maker.hpp"
#include "../common/types.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>


// Deterministic LCG mirroring FastRNG in simulator/market_sim.hpp
struct LCG {
    std::uint64_t state;
    explicit LCG(std::uint64_t s) noexcept : state(s) {}
    std::uint64_t next() noexcept {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return state >> 16;
    }
    int    rand_int(int max) noexcept { return static_cast<int>(next() % max); }
    double rand_double() noexcept {
        return static_cast<double>(next() & 0xFFFFFFFFu) / 4294967296.0;
    }
};


int main(int argc, char* argv[]) {
    int N = (argc > 1) ? std::atoi(argv[1]) : 100'000;
    if (N <= 0) N = 100'000;

    mm::MMConfig cfg;
    cfg.quote_size          = 10;
    cfg.half_spread_ticks   = 2;     // post 2 ticks ($0.02) on each side of mid
    cfg.max_inventory       = 200;
    cfg.risk_aversion_ticks = 0.05;  // 1 tick of skew per 20 shares of inventory

    mm::MarketMaker maker(cfg, "AAPL");
    LCG rng(0xC0FFEE);

    int32_t mid = 15000;  // $150.00
    const double cross_prob = 0.20;

    std::printf("=== Market Maker Simulation ===\n");
    std::printf("  symbol         : %s\n", maker.symbol());
    std::printf("  ticks          : %d\n", N);
    std::printf("  quote size     : %d\n", cfg.quote_size);
    std::printf("  half spread    : %d ticks ($0.0%d)\n",
                cfg.half_spread_ticks, cfg.half_spread_ticks);
    std::printf("  max inventory  : %d\n", cfg.max_inventory);
    std::printf("  risk aversion  : %.3f ticks / share\n", cfg.risk_aversion_ticks);
    std::printf("  P(adversary crosses): %.2f\n\n", cross_prob);

    for (int i = 0; i < N; ++i) {
        // Mid random walk (small drift each tick)
        const double r = rng.rand_double();
        if      (r < 0.10) mid--;
        else if (r > 0.90) mid++;

        // Reference book quotes (1-tick around mid)
        const int32_t best_bid = mid - 1;
        const int32_t best_ask = mid + 1;

        // Maker (re)quotes both sides
        mm::Quote q = maker.quote(best_bid, best_ask);

        // Adversary crosses our quote sometimes
        if (rng.rand_double() < cross_prob) {
            const bool aggressive_buy = (rng.rand_int(2) == 0);
            if (aggressive_buy && q.ask_size > 0) {
                // Adversary lifts our ask: we sold
                maker.apply_fill(Side::SELL, q.ask_size, q.ask_price);
            } else if (!aggressive_buy && q.bid_size > 0) {
                // Adversary hits our bid: we bought
                maker.apply_fill(Side::BUY, q.bid_size, q.bid_price);
            }
        }
    }

    const double final_pnl = maker.pnl(mid);
    const double avg_quote_churn = static_cast<double>(maker.quotes_placed()) / N;

    std::printf("=== Results ===\n");
    std::printf("  ticks processed     : %d\n",       N);
    std::printf("  quotes placed       : %lu\n",      (unsigned long)maker.quotes_placed());
    std::printf("  quotes cancelled    : %lu\n",      (unsigned long)maker.quotes_cancelled());
    std::printf("  fills received      : %lu\n",      (unsigned long)maker.fills_received());
    std::printf("  quote churn / tick  : %.3f\n",     avg_quote_churn);
    std::printf("  final position      : %d shares\n",maker.position());
    std::printf("  final mid           : $%.2f\n",    mid / 100.0);
    std::printf("  P&L (mark-to-market): $%.2f\n",    final_pnl);
    return 0;
}
