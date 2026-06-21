/*
 * backtest_demo — runs a strategy through the OMS on a synthetic market stream
 * and reports performance metrics (Backtester from backtest.hpp).
 *
 * This answers "why do we trade": the lab used to show throughput, but not
 * whether alpha makes money. Here we compute P&L per closed leg, hit-rate,
 * Sharpe, Sortino, max drawdown, fill-rate, and risk tails for MeanReversion.
 *
 * Compile: g++ -O2 -std=c++17 -Wall -Wextra -o backtest/backtest_demo backtest/backtest_demo.cpp
 * Run:     ./backtest/backtest_demo [num_messages] [seed]
 */
#include "backtest.hpp"
#include "../simulator/market_sim.hpp"   // MarketDataGenerator, ITCHParser, OMS, MeanReversion

#include <cstdio>
#include <cstdlib>
#include <cmath>

static int tests_passed = 0, tests_total = 0;
#define ASSERT(cond, msg) do { ++tests_total; \
    if (!(cond)) std::printf("  FAIL: %s\n", msg); else { ++tests_passed; std::printf("  PASS: %s\n", msg); } \
} while (0)


// Deterministic tests of the metrics core — strategy-independent.
static void unit_tests() {
    std::printf("=== Backtester unit tests ===\n");
    using backtest::Backtester;

    {   // Equity, hit-rate, profit factor on a hand-picked sequence.
        Backtester bt;
        bt.on_order(true); bt.on_trade(+100.0);
        bt.on_order(true); bt.on_trade(-40.0);
        bt.on_order(true); bt.on_trade(+60.0);
        bt.on_order(false);                       // order without a fill
        const auto r = bt.compute();
        ASSERT(r.trades == 3, "bt_trade_count");
        ASSERT(r.wins == 2 && r.losses == 1, "bt_win_loss_split");
        ASSERT(std::fabs(r.total_pnl - 120.0) < 1e-9, "bt_total_pnl");
        ASSERT(std::fabs(r.hit_rate - 2.0/3.0) < 1e-9, "bt_hit_rate");
        ASSERT(std::fabs(r.profit_factor - 160.0/40.0) < 1e-9, "bt_profit_factor");
        ASSERT(std::fabs(r.fill_rate - 3.0/4.0) < 1e-9, "bt_fill_rate");
        ASSERT(std::fabs(r.gross_profit - 160.0) < 1e-9, "bt_gross_profit");
        ASSERT(std::fabs(r.gross_loss + 40.0) < 1e-9, "bt_gross_loss");
        // payoff = avg_win 80 / |avg_loss| 40 = 2.0; kelly = p - (1-p)/payoff
        ASSERT(std::fabs(r.payoff_ratio - 2.0) < 1e-9, "bt_payoff_ratio");
        ASSERT(std::fabs(r.kelly_fraction - (2.0/3.0 - (1.0/3.0)/2.0)) < 1e-9, "bt_kelly");
    }
    {   // Max drawdown: peak 100 -> trough 30 -> drop 70; recovery factor 80/70.
        Backtester bt;
        bt.on_trade(+100.0);   // equity 100 (peak)
        bt.on_trade(-70.0);    // equity 30  -> dd 70
        bt.on_trade(+50.0);    // equity 80
        const auto r = bt.compute();
        ASSERT(std::fabs(r.max_drawdown - 70.0) < 1e-9, "bt_max_drawdown");
        ASSERT(std::fabs(r.recovery_factor - 80.0/70.0) < 1e-9, "bt_recovery_factor");
        // underwater for 1 trade (equity 30 < peak 100), recovered? no (80 < 100)
        ASSERT(bt.max_drawdown_duration() == 2, "bt_dd_duration");
    }
    {   // All-positive pnl -> no losses -> profit_factor = INF, dd = 0, kelly = 1.
        Backtester bt;
        bt.on_trade(10.0); bt.on_trade(10.0);
        const auto r = bt.compute();
        ASSERT(std::isinf(r.profit_factor), "bt_no_losses_inf_pf");
        ASSERT(r.max_drawdown == 0.0, "bt_no_drawdown");
        ASSERT(std::fabs(r.kelly_fraction - 1.0) < 1e-9, "bt_kelly_no_losses");
        ASSERT(bt.ulcer_index() == 0.0, "bt_ulcer_zero_monotone");
    }
    {   // Sortino: downside deviation only. Sequence with one loss.
        Backtester bt;
        bt.on_trade(+30.0); bt.on_trade(-10.0); bt.on_trade(+20.0); bt.on_trade(+40.0);
        const auto r = bt.compute();
        // downside variance = (10^2)/4 = 25, dsd = 5; mean = 80/4 = 20
        // sortino = 20/5 * sqrt(4) = 4 * 2 = 8
        ASSERT(std::fabs(r.sortino - 8.0) < 1e-9, "bt_sortino");
        ASSERT(r.sortino > r.sharpe, "bt_sortino_gt_sharpe");   // fewer down moves
    }
    {   // VaR / CVaR on a known per-trade distribution.
        Backtester bt;
        // pnls: -50, -30, -10, +20, +100 (5 trades)
        bt.on_trade(-50.0); bt.on_trade(-30.0); bt.on_trade(-10.0);
        bt.on_trade(+20.0); bt.on_trade(+100.0);
        // sorted: [-50,-30,-10,20,100]; VaR(0.8) -> frac 0.2 -> idx floor(0.2*4)=0 -> -50 -> 50
        ASSERT(std::fabs(bt.value_at_risk(0.8) - 50.0) < 1e-9, "bt_var_80");
        // CVaR(0.6): worst floor(0.4*5)=2 -> mean(-50,-30) = -40 -> 40
        ASSERT(std::fabs(bt.conditional_value_at_risk(0.6) - 40.0) < 1e-9, "bt_cvar_60");
    }
}


int main(int argc, char* argv[]) {
    unit_tests();

    const int      N    = (argc > 1 && std::atoi(argv[1]) > 0) ? std::atoi(argv[1]) : 50000;
    const uint64_t seed = (argc > 2) ? static_cast<uint64_t>(std::atoll(argv[2])) : 42;

    // Strategy + plumbing. Generous OMS limits — the backtest doesn't test risk.
    MarketDataGenerator   gen(seed);
    ITCHParser            parser;
    OMS                   oms(/*max_position=*/2'000'000, /*max_order_value=*/1e9);
    MeanReversionStrategy strat(/*window=*/20, /*threshold_pct=*/0.1, /*order_size=*/100);
    backtest::Backtester  bt;

    // Real matching engine (#75) — fills with slippage instead of "at the signal
    // price". Without it every round-trip is free profit (100% hit, unphysical
    // Sharpe). With slippage the metrics become honest.
    auto book = std::make_unique<BookMatchEngine>(/*level_liq=*/200);

    for (int i = 0; i < N; ++i) {
        const GeneratedMessage gm = gen.generate_random_message();
        const ParsedMessage    pm = parser.parse(gm.data, gm.length);

        const char* stock = nullptr;
        double      price = 0.0;
        if (pm.type == MsgType::ADD_ORDER) { stock = pm.data.add_order.stock; price = pm.data.add_order.price; }
        else if (pm.type == MsgType::TRADE) { stock = pm.data.trade.stock;     price = pm.data.trade.price; }
        else continue;
        if (!stock || price <= 0.0) continue;

        const Signal sig = strat.on_market_data(stock, price, 0);
        if (!sig.valid) continue;

        // Realized-P&L delta around the fill = the result of one closed leg.
        const Position* pre  = oms.get_position(stock);
        const int64_t   pnl0 = pre ? pre->realized_pnl : 0;

        Order* o = oms.submit_order(stock, sig.side, sig.price, sig.quantity);
        bool filled = false;
        if (o) {
            // Match through the book -> a real VWAP with slippage.
            double vwap = sig.price;
            const int32_t matched = book->match(sig.side, sig.price, sig.quantity, vwap);
            if (matched > 0) {
                oms.fill_order(o->order_id, static_cast<uint32_t>(matched), vwap);
                filled = true;
                const Position* post = oms.get_position(stock);
                const int64_t   pnl1 = post ? post->realized_pnl : 0;
                if (pnl1 != pnl0) bt.on_trade(to_float(pnl1 - pnl0));
            }
        }
        bt.on_order(filled);
    }

    bt.print_report("MeanReversion (synthetic feed)");
    std::printf("\n  NOTE: the synthetic feed (LCG) is mean-reverting BY CONSTRUCTION —\n"
                "  prices oscillate around a base, so mean-reversion wins almost always\n"
                "  (hit-rate ~100%%, unphysical Sharpe). This is a softball showing the\n"
                "  HARNESS computes correctly. For real alpha evaluation use\n"
                "  replay/lobster_demo on LOBSTER data, where the edge is NOT guaranteed.\n");

    std::printf("\n%d/%d unit tests passed\n", tests_passed, tests_total);
    return (tests_passed == tests_total) ? 0 : 1;
}
