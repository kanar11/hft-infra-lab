/*
 * backtest_demo — uruchamia strategie przez OMS na syntetycznym strumieniu
 * rynkowym i raportuje metryki wynikowe (Backtester z backtest.hpp).
 *
 * To odpowiedz na "po co handlujemy": dotad lab pokazywal throughput, ale nie
 * czy alpha zarabia. Tu liczymy P&L per zrealizowana noga, hit-rate, Sharpe,
 * max drawdown, fill-rate dla MeanReversion.
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


// Deterministyczne testy rdzenia metryk — niezalezne od strategii.
static void unit_tests() {
    std::printf("=== Backtester unit tests ===\n");
    using backtest::Backtester;

    {   // Equity, hit-rate, profit factor na recznie dobranej sekwencji.
        Backtester bt;
        bt.on_order(true); bt.on_trade(+100.0);
        bt.on_order(true); bt.on_trade(-40.0);
        bt.on_order(true); bt.on_trade(+60.0);
        bt.on_order(false);                       // zlecenie bez fillu
        const auto r = bt.compute();
        ASSERT(r.trades == 3, "bt_trade_count");
        ASSERT(r.wins == 2 && r.losses == 1, "bt_win_loss_split");
        ASSERT(std::fabs(r.total_pnl - 120.0) < 1e-9, "bt_total_pnl");
        ASSERT(std::fabs(r.hit_rate - 2.0/3.0) < 1e-9, "bt_hit_rate");
        ASSERT(std::fabs(r.profit_factor - 160.0/40.0) < 1e-9, "bt_profit_factor");
        ASSERT(std::fabs(r.fill_rate - 3.0/4.0) < 1e-9, "bt_fill_rate");
    }
    {   // Max drawdown: peak 100 → trough 30 → spadek 70.
        Backtester bt;
        bt.on_trade(+100.0);   // equity 100 (peak)
        bt.on_trade(-70.0);    // equity 30  → dd 70
        bt.on_trade(+50.0);    // equity 80
        const auto r = bt.compute();
        ASSERT(std::fabs(r.max_drawdown - 70.0) < 1e-9, "bt_max_drawdown");
    }
    {   // Stale dodatnie pnl → brak strat → profit_factor = INF, dd = 0.
        Backtester bt;
        bt.on_trade(10.0); bt.on_trade(10.0);
        const auto r = bt.compute();
        ASSERT(std::isinf(r.profit_factor), "bt_no_losses_inf_pf");
        ASSERT(r.max_drawdown == 0.0, "bt_no_drawdown");
    }
}


int main(int argc, char* argv[]) {
    unit_tests();

    const int      N    = (argc > 1 && std::atoi(argv[1]) > 0) ? std::atoi(argv[1]) : 50000;
    const uint64_t seed = (argc > 2) ? static_cast<uint64_t>(std::atoll(argv[2])) : 42;

    // Strategia + plumbing. Hojne limity OMS — backtest nie testuje ryzyka.
    MarketDataGenerator   gen(seed);
    ITCHParser            parser;
    OMS                   oms(/*max_position=*/2'000'000, /*max_order_value=*/1e9);
    MeanReversionStrategy strat(/*window=*/20, /*threshold_pct=*/0.1, /*order_size=*/100);
    backtest::Backtester  bt;

    // Realny silnik dopasowan (#75) — fille z poslizgiem zamiast "po cenie
    // sygnalu". Bez tego kazdy round-trip to darmowy zysk (100% hit, Sharpe
    // niefizyczny). Z poslizgiem metryki staja sie uczciwe.
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

        // Realized P&L delta wokol fillu = wynik jednej zrealizowanej nogi.
        const Position* pre  = oms.get_position(stock);
        const int64_t   pnl0 = pre ? pre->realized_pnl : 0;

        Order* o = oms.submit_order(stock, sig.side, sig.price, sig.quantity);
        bool filled = false;
        if (o) {
            // Match przez ksiege → realny VWAP z poslizgiem.
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
    std::printf("\n  UWAGA: syntetyczny feed (LCG) jest mean-reverting Z KONSTRUKCJI —\n"
                "  ceny oscyluja wokol bazy, wiec mean-reversion wygrywa niemal zawsze\n"
                "  (hit-rate ~100%%, Sharpe niefizyczny). To softball pokazujacy ze HARNESS\n"
                "  liczy poprawnie. Realna ewaluacja alphy: replay/lobster_demo na danych\n"
                "  LOBSTER, gdzie edge NIE jest gwarantowany.\n");

    std::printf("\n%d/%d unit tests passed\n", tests_passed, tests_total);
    return (tests_passed == tests_total) ? 0 : 1;
}
