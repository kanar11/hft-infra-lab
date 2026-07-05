/*
 * HFT Infrastructure Lab — Integration Test Suite (C++)
 *
 * Tests all modules from a single binary: ITCH parser, OMS, Risk Manager,
 * Smart Router, Trade Logger, Strategy, FIX, OUCH, Market Simulator.
 *
 * Compile: g++ -O2 -std=c++17 -Wall -Wextra -o tests/test_all tests/test_all.cpp
 * Run:     ./tests/test_all
 */

#include <cstdio>
#include <cstring>
#include <cmath>
#include <chrono>
#include <thread>
#include <atomic>
#include <limits>
#include <vector>
#include <algorithm>
#include <map>
#include <functional>

// All module headers
#include "../itch-parser/itch_parser.hpp"
#include "../itch-parser/itch_book.hpp"
#include "../multicast/gap_recovery.hpp"
#include "../orderbook/orderbook_flat.hpp"
#include "../oms/oms.hpp"
#include "../risk/risk_manager.hpp"
#include "../router/smart_router.hpp"
#include "../logger/trade_logger.hpp"
#include "../logger/lockfree_logger.hpp"
#include "../logger/mmap_logger.hpp"
#include "../strategy/mean_reversion.hpp"
#include "../strategy/momentum.hpp"
#include "../strategy/bollinger.hpp"
#include "../strategy/donchian.hpp"
#include "../strategy/rsi.hpp"
#include "../strategy/ma_crossover.hpp"
#include "../strategy/volatility.hpp"
#include "../strategy/ema.hpp"
#include "../strategy/macd.hpp"
#include "../strategy/stochastic.hpp"
#include "../strategy/wma.hpp"
#include "../strategy/hull_ma.hpp"
#include "../strategy/dema.hpp"
#include "../strategy/tema.hpp"
#include "../strategy/trix.hpp"
#include "../strategy/cci.hpp"
#include "../strategy/bollinger_pctb.hpp"
#include "../strategy/roc.hpp"
#include "../strategy/aroon.hpp"
#include "../strategy/cmo.hpp"
#include "../strategy/zscore.hpp"
#include "../strategy/tsi.hpp"
#include "../strategy/dpo.hpp"
#include "../strategy/kama.hpp"
#include "../strategy/linreg.hpp"
#include "../strategy/rolling_stddev.hpp"
#include "../strategy/fisher.hpp"
#include "../strategy/coppock.hpp"
#include "../strategy/obv.hpp"
#include "../strategy/volume_oscillator.hpp"
#include "../strategy/pvt.hpp"
#include "../strategy/ppo.hpp"
#include "../strategy/force_index.hpp"
#include "../strategy/mfi.hpp"
#include "../strategy/vwma.hpp"
#include "../strategy/nvi_pvi.hpp"
#include "../strategy/close_atr.hpp"
#include "../strategy/keltner.hpp"
#include "../strategy/zlema.hpp"
#include "../strategy/chandelier.hpp"
#include "../strategy/awesome.hpp"
#include "../strategy/accel_decel.hpp"
#include "../strategy/rolling_median.hpp"
#include "../strategy/supertrend.hpp"
#include "../strategy/ultimate.hpp"
#include "../strategy/choppiness.hpp"
#include "../strategy/percent_rank.hpp"
#include "../strategy/cfo.hpp"
#include "../strategy/pgo.hpp"
#include "../strategy/kst.hpp"
#include "../strategy/vidya.hpp"
#include "../strategy/center_of_gravity.hpp"
#include "../strategy/ensemble.hpp"
#include "../backtest/backtest.hpp"
#include "../strategy/trailing_stop.hpp"
#include "../strategy/pov_algo.hpp"
#include "../strategy/signal_throttle.hpp"
#include "../strategy/vwap_tracker.hpp"
#include "../strategy/market_maker.hpp"
#include "../fix-protocol/fix_parser.hpp"
#include "../fix-protocol/fix_session.hpp"
#include "../fix-protocol/fix_order_state.hpp"
#include "../ouch-protocol/ouch_protocol.hpp"
#include "../ouch-protocol/ouch_order_state.hpp"
#include "../ouch-protocol/soupbin_session.hpp"
#include "../common/fill_simulator.hpp"
#include "../lockfree/spsc_queue.hpp"
#include "../lockfree/mpsc_queue.hpp"
#include "../lockfree/mpmc_queue.hpp"
#include "../lockfree/sequencer.hpp"
#include "../lockfree/waitable_mpsc.hpp"
#include "../lockfree/varlen_ring.hpp"
#include "../replay/lobster_reader.hpp"
#include "../simulator/market_sim.hpp"
#include "../backtest/backtest.hpp"

static int tests_passed = 0;
static int tests_failed = 0;
static int tests_total = 0;

#define ASSERT(cond, msg) do { \
    tests_total++; \
    if (!(cond)) { \
        printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
        tests_failed++; \
    } else { \
        tests_passed++; \
    } \
} while(0)

#define SECTION(name) printf("\n--- %s ---\n", name)


// =====================================================
// ITCH Parser Tests
// =====================================================

void test_itch_parser() {
    SECTION("ITCH Parser");
    ITCHParser parser;

    // Test Add Order parsing
    uint8_t add_msg[34];
    add_msg[0] = 'A';
    // timestamp = 0 (bytes 1-8)
    memset(add_msg + 1, 0, 8);
    // order_ref = 1001 (bytes 9-16, big-endian)
    memset(add_msg + 9, 0, 8);
    add_msg[15] = 0x03; add_msg[16] = 0xE9;  // 1001
    // side = 'B'
    add_msg[17] = 'B';
    // shares = 100 (bytes 18-21)
    memset(add_msg + 18, 0, 4);
    add_msg[20] = 0; add_msg[21] = 100;
    // stock = "AAPL    " (bytes 22-29)
    memcpy(add_msg + 22, "AAPL    ", 8);
    // price = 1502500 (150.25 * 10000) (bytes 30-33)
    int32_t px = 1502500;
    add_msg[30] = (px >> 24) & 0xFF;
    add_msg[31] = (px >> 16) & 0xFF;
    add_msg[32] = (px >> 8) & 0xFF;
    add_msg[33] = px & 0xFF;

    ParsedMessage pm = parser.parse(add_msg, 34);
    ASSERT(pm.type == MsgType::ADD_ORDER, "itch_add_order_type");
    ASSERT(pm.data.add_order.side == 'B', "itch_add_order_side");
    ASSERT(pm.data.add_order.shares == 100, "itch_add_order_shares");

    // Test System Event
    uint8_t sys_msg[10];
    sys_msg[0] = 'S';
    memset(sys_msg + 1, 0, 8);
    sys_msg[9] = 'O';
    pm = parser.parse(sys_msg, 10);
    ASSERT(pm.type == MsgType::SYSTEM_EVENT, "itch_system_event_type");
    ASSERT(pm.data.system_event.event_code == 'O', "itch_system_event_code");

    // Test Trade message
    uint8_t trade_msg[42];
    trade_msg[0] = 'P';
    memset(trade_msg + 1, 0, 41);
    trade_msg[17] = 'S';
    trade_msg[21] = 200;  // shares
    // ITCH wire format requires fixed-width 8-byte symbol, not null-terminated.
    // NOLINTNEXTLINE(bugprone-not-null-terminated-result)
    memcpy(trade_msg + 22, "MSFT    ", 8);
    pm = parser.parse(trade_msg, 42);
    ASSERT(pm.type == MsgType::TRADE, "itch_trade_type");
    ASSERT(pm.data.trade.side == 'S', "itch_trade_side");

    // Test Order Executed
    uint8_t exec_msg[29];
    exec_msg[0] = 'E';
    memset(exec_msg + 1, 0, 28);
    exec_msg[20] = 50;  // exec_shares
    pm = parser.parse(exec_msg, 29);
    ASSERT(pm.type == MsgType::ORDER_EXECUTED, "itch_executed_type");

    // Test Delete Order
    uint8_t del_msg[17];
    del_msg[0] = 'D';
    memset(del_msg + 1, 0, 16);
    pm = parser.parse(del_msg, 17);
    ASSERT(pm.type == MsgType::DELETE_ORDER, "itch_delete_type");

    // Test unknown message type
    uint8_t unk_msg[10];
    unk_msg[0] = 'Z';
    pm = parser.parse(unk_msg, 10);
    ASSERT(pm.type == MsgType::UNKNOWN, "itch_unknown_type");

    printf("  ITCH: %d assertions\n", 10);
}


// =====================================================
// OMS Tests
// =====================================================

void test_oms() {
    SECTION("OMS");
    OMS oms(1000, 200000.0);

    // Submit and fill a buy order
    Order* o1 = oms.submit_order("AAPL", Side::BUY, 150.25, 100);
    ASSERT(o1 != nullptr, "oms_submit_buy");
    if (!o1) return;
    ASSERT(o1->order_id == 1, "oms_order_id_1");
    ASSERT(o1->side == Side::BUY, "oms_side_buy");

    oms.fill_order(1, 100, 150.25);
    const Order* filled = oms.get_order(1);
    ASSERT(filled != nullptr, "oms_get_filled");
    if (!filled) return;
    ASSERT(filled->status == OrderStatus::FILLED, "oms_status_filled");
    ASSERT(filled->filled_qty == 100, "oms_filled_qty");

    // Submit and cancel
    Order* o2 = oms.submit_order("TSLA", Side::SELL, 245.00, 50);
    ASSERT(o2 != nullptr, "oms_submit_sell");
    oms.cancel_order(2);
    const Order* cancelled = oms.get_order(2);
    ASSERT(cancelled->status == OrderStatus::CANCELLED, "oms_cancelled");

    // Risk rejection: order too large
    Order* o3 = oms.submit_order("NVDA", Side::BUY, 880.00, 500);
    ASSERT(o3 == nullptr, "oms_risk_reject_value");

    // Position tracking
    const Position* pos = oms.get_position("AAPL");
    ASSERT(pos != nullptr, "oms_position_exists");
    if (!pos) return;
    ASSERT(pos->net_qty == 100, "oms_position_qty");

    // Partial fill
    Order* o4 = oms.submit_order("MSFT", Side::BUY, 410.00, 200);
    ASSERT(o4 != nullptr, "oms_submit_partial");
    if (!o4) return;
    oms.fill_order(o4->order_id, 100, 410.00);
    const Order* partial = oms.get_order(o4->order_id);
    ASSERT(partial->status == OrderStatus::PARTIAL, "oms_partial_status");
    ASSERT(partial->filled_qty == 100, "oms_partial_filled");

    // Fill the rest
    oms.fill_order(o4->order_id, 100, 410.00);
    partial = oms.get_order(o4->order_id);
    ASSERT(partial->status == OrderStatus::FILLED, "oms_full_fill");

    ASSERT(oms.order_count() == 3u, "oms_order_count");

    printf("  OMS: %d assertions\n", 16);
}


// OMS #77 — short-position P&L, flip long↔short, replace_order (amend).
void test_oms_short_and_replace() {
    SECTION("OMS Short P&L + Replace");
    auto close = [](double a, double b) { const double d = a - b; return (d < 0 ? -d : d) < 0.01; };

    // --- Short P&L: sell high, buy back low → profit ---
    {
        OMS oms(10000, 100000000.0);
        oms.fill_order((oms.submit_order("AAPL", Side::SELL, 50.00, 100))->order_id, 100, 50.00);
        const Position* p = oms.get_position("AAPL");
        ASSERT(p && p->net_qty == -100, "short_open_net_neg");
        oms.fill_order((oms.submit_order("AAPL", Side::BUY, 48.00, 100))->order_id, 100, 48.00);
        p = oms.get_position("AAPL");
        ASSERT(p->net_qty == 0, "short_covered_flat");
        ASSERT(close(to_float(p->realized_pnl), 200.0), "short_pnl_profit_200");  // (50-48)*100
    }

    // --- Flip long→short in one fill + finishing the cover ---
    {
        OMS oms(10000, 100000000.0);
        oms.fill_order((oms.submit_order("X", Side::BUY, 10.00, 100))->order_id, 100, 10.00);
        // SELL 150 @ $12: close 100 long (realize $200) and flip to short 50 @ $12
        oms.fill_order((oms.submit_order("X", Side::SELL, 12.00, 150))->order_id, 150, 12.00);
        const Position* p = oms.get_position("X");
        ASSERT(p->net_qty == -50, "flip_to_short_50");
        ASSERT(close(to_float(p->avg_price), 12.00), "flip_avg_is_fill_px");
        ASSERT(close(to_float(p->realized_pnl), 200.0), "flip_realized_200");
        // Cover short 50 @ $11 → +$50 → total $250
        oms.fill_order((oms.submit_order("X", Side::BUY, 11.00, 50))->order_id, 50, 11.00);
        p = oms.get_position("X");
        ASSERT(p->net_qty == 0, "flip_fully_closed");
        ASSERT(close(to_float(p->realized_pnl), 250.0), "flip_total_realized_250");
    }

    // --- replace_order: amend price/quantity ---
    {
        OMS oms(1000, 1000000.0);
        Order* o = oms.submit_order("AAPL", Side::BUY, 100.00, 50);
        ASSERT(o != nullptr, "replace_submit_ok");
        if (!o) return;
        ASSERT(oms.replace_order(o->order_id, 101.00, 80), "replace_applied");
        const Order* r = oms.get_order(o->order_id);
        ASSERT(r->quantity == 80 && close(to_float(r->price), 101.00), "replace_new_px_qty");
    }
    {   // amend above the position limit → rejected, order unchanged
        OMS oms(100, 1000000.0);
        Order* a = oms.submit_order("AAPL", Side::BUY, 10.00, 50);
        ASSERT(!oms.replace_order(a->order_id, 10.00, 200), "replace_rejects_over_limit");
        ASSERT(oms.get_order(a->order_id)->quantity == 50, "replace_unchanged_on_reject");
    }

    {   // #83 commissions: gross vs net. $0.01/share, round-trip 100 shares.
        OMS oms(10000, 100000000.0, /*commission_per_share=*/0.01);
        oms.fill_order(oms.submit_order("AAPL", Side::BUY,  10.00, 100)->order_id, 100, 10.00);
        oms.fill_order(oms.submit_order("AAPL", Side::SELL, 12.00, 100)->order_id, 100, 12.00);
        const Position* p = oms.get_position("AAPL");
        ASSERT(close(to_float(p->realized_pnl), 200.0), "fee_gross_pnl_200");   // (12-10)*100
        ASSERT(close(to_float(p->fees), 2.0), "fee_total_2");                   // 200 shares * $0.01
        ASSERT(close(to_float(p->net_pnl()), 198.0), "fee_net_pnl_198");
        ASSERT(close(to_float(oms.total_fees()), 2.0), "fee_oms_total_2");
    }

    {   // #100 cancel_all / cancel_all_symbol — risk-off masowe anulowanie.
        OMS oms(1000000, 1000000000.0);
        oms.submit_order("AAA", Side::BUY,  10.00, 100);   // SENT
        oms.submit_order("BBB", Side::SELL, 20.00, 50);    // SENT
        Order* f = oms.submit_order("AAA", Side::BUY, 10.00, 100);
        oms.fill_order(f->order_id, 100, 10.00);           // FILLED — not cancellable
        ASSERT(oms.cancel_all() == 2, "cancelall_two_open");
        ASSERT(oms.get_position("AAA")->pending_qty == 0, "cancelall_releases_pending");
        // per-symbol
        OMS o2(1000000, 1000000000.0);
        o2.submit_order("AAA", Side::BUY, 10.00, 100);
        o2.submit_order("BBB", Side::BUY, 10.00, 100);
        ASSERT(o2.cancel_all_symbol("AAA") == 1, "cancelall_symbol_one");
    }

    {   // #172 GTD expiry: purge_expired cancels expired ones, leaves those without expiry.
        OMS oms(100000, 1000000000.0);
        OMSReject why = OMSReject::NONE;
        Order* g = oms.submit_order("AAA", Side::BUY, 10.0, 100, &why, /*expire_ns=*/5000);
        Order* d = oms.submit_order("BBB", Side::BUY, 10.0, 100);   // bez wygasniecia
        ASSERT(oms.purge_expired(6000) == 1, "gtd_purged_one");     // 5000 <= 6000
        ASSERT(oms.get_order(g->order_id)->status == OrderStatus::CANCELLED, "gtd_expired_cancelled");
        ASSERT(oms.get_order(d->order_id)->status == OrderStatus::SENT, "gtd_no_expiry_kept");
    }

    {   // #180 open_order_notional: capital in working orders (remainder).
        OMS oms(100000, 1000000000.0);
        Order* a = oms.submit_order("AAA", Side::BUY, 10.0, 100);   // notional 1000
        Order* b = oms.submit_order("BBB", Side::SELL, 20.0, 50);   // notional 1000
        oms.fill_order(a->order_id, 40, 10.0);                      // remainder 60 -> 600
        ASSERT(close(oms.open_order_notional(), 1600.0), "open_notional_partial");  // 600 + 1000
        oms.cancel_order(b->order_id);
        ASSERT(close(oms.open_order_notional(), 600.0), "open_notional_after_cancel");
    }

    {   // #188 amend_quantity: reduce-only w miejscu.
        OMS oms(100000, 1000000000.0);
        Order* o = oms.submit_order("AAA", Side::BUY, 10.0, 100);
        oms.fill_order(o->order_id, 30, 10.0);                 // filled 30, PARTIAL
        ASSERT(oms.amend_quantity(o->order_id, 50), "amend_reduce_ok");        // 100 -> 50
        ASSERT(oms.get_order(o->order_id)->quantity == 50, "amend_qty_50");
        ASSERT(!oms.amend_quantity(o->order_id, 70), "amend_increase_rejected"); // 70 >= 50
        ASSERT(!oms.amend_quantity(o->order_id, 20), "amend_below_filled_rejected"); // 20 < 30
        ASSERT(!oms.amend_quantity(999999, 10), "amend_unknown_rejected");
        ASSERT(oms.amend_quantity(o->order_id, 30), "amend_to_filled_ok");     // 50 -> 30 == filled
        ASSERT(oms.get_order(o->order_id)->status == OrderStatus::FILLED, "amend_becomes_filled");
    }

    {   // #196 position_count + is_flat (kontrola EOD).
        OMS oms(100000, 1000000000.0);
        ASSERT(oms.is_flat() && oms.open_position_count() == 0, "flat_empty");
        Order* o = oms.submit_order("AAA", Side::BUY, 10.0, 100);
        ASSERT(!oms.is_flat(), "flat_false_working_order");          // working order
        oms.fill_order(o->order_id, 100, 10.0);                      // position 100
        ASSERT(oms.open_position_count() == 1 && !oms.is_flat(), "flat_false_open_position");
        Order* s = oms.submit_order("AAA", Side::SELL, 11.0, 100);
        oms.fill_order(s->order_id, 100, 11.0);                      // net 0
        ASSERT(oms.open_position_count() == 0 && oms.is_flat(), "flat_after_close");
    }

    {   // #290 working_order_count / done_order_count.
        OMS oms(1000000, 1000000000.0);
        Order* a = oms.submit_order("AAA", Side::BUY, 10.0, 100);   // SENT
        Order* b = oms.submit_order("BBB", Side::BUY, 10.0, 100);   // SENT
        oms.fill_order(a->order_id, 100, 10.0);                     // A FILLED
        oms.fill_order(b->order_id, 50, 10.0);                      // B PARTIAL
        ASSERT(oms.working_order_count() == 1 && oms.done_order_count() == 1, "wo_partial_split");
        oms.cancel_order(b->order_id);                             // B CANCELLED
        ASSERT(oms.working_order_count() == 0 && oms.done_order_count() == 2, "wo_after_cancel");
    }

    {   // #204 reset_session_counters (zeros statistics, leaves positions/orders).
        OMS oms(100000, 1000000000.0);
        Order* o = oms.submit_order("AAA", Side::BUY, 10.0, 100);
        oms.fill_order(o->order_id, 50, 10.0);                   // fills 1, PARTIAL
        ASSERT(oms.total_submitted() == 1 && oms.total_fills() == 1, "rsc_counters_before");
        oms.reset_session_counters();
        ASSERT(oms.total_submitted() == 0 && oms.total_fills() == 0
               && oms.total_cancels() == 0 && oms.total_replaces() == 0, "rsc_counters_zeroed");
        ASSERT(oms.order_count() >= 1 && oms.open_position_count() == 1, "rsc_state_kept");
    }

    {   // #212 submit_reject_rate (small max_order_value -> some rejected).
        OMS oms(100000, 500.0);                                 // max_order_value 500
        OMSReject why = OMSReject::NONE;
        oms.submit_order("AAA", Side::BUY, 10.0, 10, &why);     // value 100 <= 500 ok
        oms.submit_order("AAA", Side::BUY, 10.0, 100, &why);    // value 1000 > 500 reject
        oms.submit_order("AAA", Side::BUY, 10.0, 100, &why);    // reject
        ASSERT(oms.total_rejects() == 2, "srr_two_rejects");
        ASSERT(close(oms.submit_reject_rate(), 2.0/3.0), "srr_rate_two_thirds"); // 2 / (1+2)
    }

    {   // #220 gross_position_shares + largest_position.
        OMS oms(1000000, 1000000000.0);
        Order* a = oms.submit_order("AAA", Side::BUY, 10.0, 100);
        oms.fill_order(a->order_id, 100, 10.0);                 // +100
        Order* b = oms.submit_order("BBB", Side::SELL, 20.0, 60);
        oms.fill_order(b->order_id, 60, 20.0);                  // -60 (short)
        ASSERT(oms.gross_position_shares() == 160, "gross_long_plus_abs_short"); // 100 + 60
        ASSERT(oms.largest_position() == 100, "largest_is_AAA");
        // #314 inventory_value — cost basis: 100*10 (long) + 60*20 (short) = 2200.
        ASSERT(close(to_float(oms.inventory_value()), 2200.0), "inventory_value_cost");
        // #484 net_position_shares / net_inventory_value — signed directional.
        // +100 long AAA, -60 short BBB -> net +40 shares, below the gross 160.
        ASSERT(oms.net_position_shares() == 40, "net_position_shares_40");
        ASSERT(oms.net_position_shares() < oms.gross_position_shares(), "net_below_gross");
        // Net $: 100*10 - 60*20 = -200 (short by dollar value despite net long shares).
        ASSERT(close(to_float(oms.net_inventory_value()), -200.0), "net_inventory_value_minus200");
        // #330 largest_position_notional — by $: BBB 60*20=1200 > AAA 100*10=1000, so the
        // dollar leader (BBB) differs from largest_position (AAA, by shares).
        ASSERT(close(to_float(oms.largest_position_notional()), 1200.0), "largest_notional_is_BBB");
        OMS empt(1000000, 1000000000.0);
        ASSERT(empt.inventory_value() == 0, "inventory_value_flat_zero");
        ASSERT(empt.largest_position_notional() == 0, "largest_notional_flat_zero");
        // #484: empty book nets zero on both.
        ASSERT(empt.net_position_shares() == 0 && empt.net_inventory_value() == 0,
               "net_position_flat_zero");
    }

    {   // #228 fill_ratio (filled / ordered shares).
        OMS oms(1000000, 1000000000.0);
        Order* a = oms.submit_order("AAA", Side::BUY, 10.0, 100);   // ordered 100
        oms.fill_order(a->order_id, 60, 10.0);                      // filled 60
        Order* b = oms.submit_order("BBB", Side::BUY, 10.0, 100);   // ordered total 200
        oms.fill_order(b->order_id, 100, 10.0);                     // filled total 160
        ASSERT(oms.total_ordered_shares() == 200 && oms.total_filled_shares() == 160,
               "fillratio_accumulators");
        ASSERT(close(oms.fill_ratio(), 0.8), "fillratio_080");      // 160/200
        // #476 order_fill_rate — per-ORDER completion, diverges from fill_ratio.
        // b is FILLED (100/100), a is only PARTIAL (60/100) -> 1 of 2 orders.
        ASSERT(close(oms.order_fill_rate(), 0.5), "order_fill_rate_half");
        // The share-view (0.8) and order-view (0.5) differ on partial fills.
        ASSERT(oms.order_fill_rate() < oms.fill_ratio(), "order_fill_rate_vs_shares");
        // Completing the partial order lifts it to 100% orders filled.
        oms.fill_order(a->order_id, 40, 10.0);                      // a now FILLED
        ASSERT(close(oms.order_fill_rate(), 1.0), "order_fill_rate_all_filled");
        // A fresh cancelled order never counts toward the fill rate.
        Order* c = oms.submit_order("CCC", Side::BUY, 5.0, 10);
        oms.cancel_order(c->order_id);
        ASSERT(close(oms.order_fill_rate(), 2.0 / 3.0), "order_fill_rate_cancel_dilutes");
    }
    {   // #476 empty OMS -> 0 (no division trap).
        OMS oms(1000000, 1000000000.0);
        ASSERT(oms.order_fill_rate() == 0.0, "order_fill_rate_empty_zero");
    }

    {   // #236 avg_commission_per_share (commission 0.01/share).
        OMS oms(1000000, 1000000000.0, /*commission_per_share=*/0.01);
        Order* a = oms.submit_order("AAA", Side::BUY, 10.0, 100);
        oms.fill_order(a->order_id, 100, 10.0);                     // fee 1.0, filled 100
        ASSERT(close(to_float(oms.total_fees()), 1.0), "avgcomm_total_fee");
        ASSERT(close(oms.avg_commission_per_share(), 0.01), "avgcomm_per_share"); // 1.0/100
        OMS empt(1000000, 1000000000.0);
        ASSERT(empt.avg_commission_per_share() == 0.0, "avgcomm_empty_zero");
    }

    {   // #244 winning_symbols / losing_symbols (P&L attribution).
        OMS oms(1000000, 1000000000.0);
        // A: long 100@10, close @12 -> +200; B: @9 -> -100; C: @10 -> 0
        Order* a1 = oms.submit_order("AAA", Side::BUY, 10.0, 100); oms.fill_order(a1->order_id, 100, 10.0);
        Order* a2 = oms.submit_order("AAA", Side::SELL, 12.0, 100); oms.fill_order(a2->order_id, 100, 12.0);
        Order* b1 = oms.submit_order("BBB", Side::BUY, 10.0, 100); oms.fill_order(b1->order_id, 100, 10.0);
        Order* b2 = oms.submit_order("BBB", Side::SELL, 9.0, 100);  oms.fill_order(b2->order_id, 100, 9.0);
        Order* c1 = oms.submit_order("CCC", Side::BUY, 10.0, 100); oms.fill_order(c1->order_id, 100, 10.0);
        Order* c2 = oms.submit_order("CCC", Side::SELL, 10.0, 100); oms.fill_order(c2->order_id, 100, 10.0);
        ASSERT(oms.winning_symbols() == 1, "winsym_one");   // AAA
        ASSERT(oms.losing_symbols() == 1, "losesym_one");   // BBB (CCC = 0, skipped)
        // #298 symbol_win_rate — 1 winner / (1 winner + 1 loser) = 0.5 (CCC flat excluded).
        ASSERT(close(oms.symbol_win_rate(), 0.5), "symwinrate_half");
        OMS empt(1000000, 1000000000.0);
        ASSERT(empt.symbol_win_rate() == 0.0, "symwinrate_empty_zero");
        // #339 gross_profit / gross_loss / profit_factor (P&L-weighted attribution).
        ASSERT(close(to_float(oms.gross_profit()), 200.0), "oms_gross_profit_200");  // AAA
        ASSERT(close(to_float(oms.gross_loss()),   100.0), "oms_gross_loss_100");    // BBB
        ASSERT(close(oms.profit_factor(), 2.0), "oms_profit_factor_2");              // 200/100
        ASSERT(empt.profit_factor() == 0.0, "oms_pf_empty_zero");
        // #492 best_realized_symbol / worst_realized_symbol — the names.
        // AAA is the +200 winner, BBB the -100 loser (CCC flat is skipped).
        char rsym[9];
        ASSERT(close(oms.best_realized_symbol(rsym), 200.0)
               && std::strcmp(rsym, "AAA") == 0, "oms_best_realized_AAA");
        ASSERT(close(oms.worst_realized_symbol(rsym), -100.0)
               && std::strcmp(rsym, "BBB") == 0, "oms_worst_realized_BBB");
        // Empty book: no realized P&L -> empty name, 0.
        rsym[0] = 'X';
        ASSERT(empt.best_realized_symbol(rsym) == 0.0 && rsym[0] == '\0', "oms_best_realized_empty");
        // #500 profit_concentration / loss_concentration — HHI-lite of P&L.
        // AAA is the ONLY winner (+200) -> profit fully concentrated at 1.0;
        // BBB the only loser (-100) -> loss concentration 1.0.
        ASSERT(close(oms.profit_concentration(), 1.0), "oms_profit_conc_single_1");
        ASSERT(close(oms.loss_concentration(), 1.0), "oms_loss_conc_single_1");
        ASSERT(empt.profit_concentration() == 0.0 && empt.loss_concentration() == 0.0,
               "oms_conc_empty_zero");
        // #500: two winners spread the profit -> concentration below 1.
        // AAA +200 (100@10 -> @12), DDD +100 (100@10 -> @11): gross 300,
        // best 200 -> 200/300 = 2/3.
        OMS oms2(1000000, 1000000000.0);
        Order* pc1 = oms2.submit_order("AAA", Side::BUY, 10.0, 100); oms2.fill_order(pc1->order_id, 100, 10.0);
        Order* pc2 = oms2.submit_order("AAA", Side::SELL, 12.0, 100); oms2.fill_order(pc2->order_id, 100, 12.0);
        Order* pc3 = oms2.submit_order("DDD", Side::BUY, 10.0, 100); oms2.fill_order(pc3->order_id, 100, 10.0);
        Order* pc4 = oms2.submit_order("DDD", Side::SELL, 11.0, 100); oms2.fill_order(pc4->order_id, 100, 11.0);
        ASSERT(close(oms2.profit_concentration(), 2.0 / 3.0), "oms_profit_conc_two_thirds");
        // No losers at all -> loss_concentration is 0 (no denominator).
        ASSERT(oms2.loss_concentration() == 0.0, "oms_loss_conc_no_losers_zero");
        // #347 avg_win_per_symbol / avg_loss_per_symbol — dollars won/lost PER NAME.
        ASSERT(close(oms.avg_win_per_symbol(), 200.0), "oms_avgwin_200");    // 200/1 winner (AAA)
        ASSERT(close(oms.avg_loss_per_symbol(), 100.0), "oms_avgloss_100"); // 100/1 loser (BBB)
        // D: long 100@10, closed @9.5 -> -50. Now 2 losers (BBB -100, DDD -50), gross_loss 150.
        Order* d1 = oms.submit_order("DDD", Side::BUY, 10.0, 100); oms.fill_order(d1->order_id, 100, 10.0);
        Order* d2 = oms.submit_order("DDD", Side::SELL, 9.5, 100); oms.fill_order(d2->order_id, 100, 9.5);
        ASSERT(oms.losing_symbols() == 2 && close(to_float(oms.gross_loss()), 150.0),
               "oms_avgloss_ddd_added");
        ASSERT(close(oms.avg_loss_per_symbol(), 75.0), "oms_avgloss_75");   // 150/2
        ASSERT(close(oms.avg_win_per_symbol(), 200.0), "oms_avgwin_unchanged"); // still 200/1
        ASSERT(empt.avg_win_per_symbol() == 0.0 && empt.avg_loss_per_symbol() == 0.0,
               "oms_avgwinloss_empty_zero");
        // #355 largest_win / largest_loss — the single biggest name, vs the #347 mean.
        // AAA is the only winner (+200); BBB (-100) beats DDD (-50) as the biggest loser.
        ASSERT(close(to_float(oms.largest_win()), 200.0), "oms_largest_win_200");
        ASSERT(close(to_float(oms.largest_loss()), 100.0), "oms_largest_loss_100");
        ASSERT(empt.largest_win() == 0 && empt.largest_loss() == 0, "oms_largest_winloss_empty_zero");
        // #363 expectancy_per_symbol — frequency×magnitude edge. AAA +200, BBB -100,
        // DDD -50: net 50 over 3 decided symbols -> 16.666...
        ASSERT(close(oms.expectancy_per_symbol(), 50.0 / 3.0), "oms_expectancy_16_67");
        // cross-check the composite form: win_rate*avg_win - loss_rate*avg_loss.
        ASSERT(close(oms.expectancy_per_symbol(),
                     oms.symbol_win_rate() * oms.avg_win_per_symbol()
                     - (1.0 - oms.symbol_win_rate()) * oms.avg_loss_per_symbol()),
               "oms_expectancy_composite_matches");
        ASSERT(empt.expectancy_per_symbol() == 0.0, "oms_expectancy_empty_zero");
        // no losing symbols + profit -> +inf (same convention as the Backtester)
        OMS pf_allwin(1000000, 1000000000.0);
        Order* pfw1 = pf_allwin.submit_order("WIN", Side::BUY, 10.0, 100);
        pf_allwin.fill_order(pfw1->order_id, 100, 10.0);
        Order* pfw2 = pf_allwin.submit_order("WIN", Side::SELL, 11.0, 100);
        pf_allwin.fill_order(pfw2->order_id, 100, 11.0);
        const double pf = pf_allwin.profit_factor();
        ASSERT(std::isinf(pf) && pf > 0.0, "oms_pf_no_loss_inf");
    }

    {   // #251 pending_buy_shares / pending_sell_shares (working orders per side).
        OMS oms(1000000, 1000000000.0);
        Order* a = oms.submit_order("AAA", Side::BUY, 10.0, 100);   // pending +100
        oms.submit_order("BBB", Side::SELL, 20.0, 60);              // pending -60
        ASSERT(oms.pending_buy_shares() == 100 && oms.pending_sell_shares() == 60,
               "pending_split_before");
        oms.fill_order(a->order_id, 100, 10.0);                     // AAA pending -> 0
        ASSERT(oms.pending_buy_shares() == 0 && oms.pending_sell_shares() == 60,
               "pending_split_after_fill");
    }

    {   // #258 cancel_rate (cancels / submitted).
        OMS oms(1000000, 1000000000.0);
        Order* a = oms.submit_order("AAA", Side::BUY, 10.0, 100);
        oms.submit_order("BBB", Side::BUY, 10.0, 100);              // submitted 2
        oms.cancel_order(a->order_id);                             // cancels 1
        ASSERT(close(oms.cancel_rate(), 0.5), "cancel_rate_half"); // 1/2
    }

    {   // #282 replace_rate (replaces / submitted).
        OMS oms(1000000, 1000000000.0);
        Order* a = oms.submit_order("AAA", Side::BUY, 10.0, 100);  // submitted 1
        oms.replace_order(a->order_id, 11.0, 80);                  // replaces 1
        oms.submit_order("BBB", Side::BUY, 10.0, 100);             // submitted 2
        ASSERT(close(oms.replace_rate(), 0.5), "replace_rate_half"); // 1/2
    }

    {   // #266 total_traded_notional (cumulative $ of all fills).
        OMS oms(1000000, 1000000000.0);
        Order* a = oms.submit_order("AAA", Side::BUY, 10.0, 100);
        oms.fill_order(a->order_id, 100, 10.0);                    // 100 * 10 = 1000
        Order* b = oms.submit_order("BBB", Side::SELL, 20.0, 50);
        oms.fill_order(b->order_id, 50, 20.0);                     // 50 * 20 = 1000
        ASSERT(close(oms.total_traded_notional(), 2000.0), "traded_notional_sum");
        // #306 avg_trade_price — blended VWAP = 2000 notional / 150 shares.
        ASSERT(close(oms.avg_trade_price(), 2000.0 / 150.0), "avg_trade_price_blended");
        OMS empt(1000000, 1000000000.0);
        ASSERT(empt.avg_trade_price() == 0.0, "avg_trade_price_empty_zero");
    }

    {   // #322 total_submitted_notional / avg_submitted_notional
        OMS oms322(1000000, 1000000000.0);
        // submit 100 @ $10 = $1000 and 50 @ $20 = $1000 => total = $2000, avg = $1000
        Order* s1 = oms322.submit_order("AAA", Side::BUY, 10.0, 100);
        Order* s2 = oms322.submit_order("BBB", Side::SELL, 20.0, 50);
        (void)s1; (void)s2;
        ASSERT(close(oms322.total_submitted_notional(), 2000.0), "oms_sub_notional_sum");
        ASSERT(close(oms322.avg_submitted_notional(), 1000.0), "oms_avg_sub_notional");
        // third order: 200 @ $5 = $1000 => total = $3000, avg = $1000
        Order* s3 = oms322.submit_order("CCC", Side::BUY, 5.0, 200);
        (void)s3;
        ASSERT(close(oms322.total_submitted_notional(), 3000.0), "oms_sub_notional_3");
        ASSERT(close(oms322.avg_submitted_notional(), 1000.0), "oms_avg_sub_notional_3");
        // reset clears the counter
        oms322.reset_session_counters();
        ASSERT(oms322.total_submitted_notional() == 0.0, "oms_sub_notional_reset");
        ASSERT(oms322.avg_submitted_notional() == 0.0, "oms_avg_sub_notional_reset");
        // empty OMS returns 0
        OMS oms322e(1000000, 1000000000.0);
        ASSERT(oms322e.avg_submitted_notional() == 0.0, "oms_avg_sub_notional_empty");
    }

    {   // #508 notional_fill_ratio — $-weighted fill rate, diverges from order_fill_rate.
        OMS nfr(1000000, 1000000000.0);
        ASSERT(nfr.notional_fill_ratio() == 0.0, "nfr_empty_zero");
        Order* small = nfr.submit_order("AAA", Side::BUY, 10.0, 10);   // $100 submitted
        nfr.fill_order(small->order_id, 10, 10.0);                     // $100 traded -> FILLED
        Order* big = nfr.submit_order("BBB", Side::BUY, 20.0, 100);    // $2000 submitted
        nfr.fill_order(big->order_id, 10, 20.0);                       // $200 traded -> partial
        // submitted $2100, traded $300 -> 0.142857 of intended dollars
        ASSERT(close(nfr.notional_fill_ratio(), 300.0 / 2100.0), "nfr_dollar_weighted");
        // per-ORDER view is 1 of 2 fully filled = 0.5, far above the $-weighted truth:
        // the big order is barely worked, so the counts flatter the execution.
        ASSERT(nfr.order_fill_rate() > nfr.notional_fill_ratio(), "nfr_below_order_rate");
        nfr.reset_session_counters();
        ASSERT(nfr.notional_fill_ratio() == 0.0, "nfr_reset_zero");
    }

    {   // #274 avg_fill_size (shares per fill).
        OMS oms(1000000, 1000000000.0);
        Order* a = oms.submit_order("AAA", Side::BUY, 10.0, 100);
        oms.fill_order(a->order_id, 40, 10.0);                     // fill 1: 40
        oms.fill_order(a->order_id, 60, 10.0);                     // fill 2: 60
        ASSERT(oms.total_fills() == 2 && oms.total_filled_shares() == 100, "avgfill_accum");
        ASSERT(close(oms.avg_fill_size(), 50.0), "avgfill_50");     // 100 / 2
    }

    {   // #371 avg_submitted_size (shares per submit) — entry-side vs avg_fill_size.
        OMS oms(1000000, 1000000000.0);
        ASSERT(oms.avg_submitted_size() == 0.0, "avgsub_empty_zero");
        oms.submit_order("AAA", Side::BUY, 10.0, 300);
        oms.submit_order("BBB", Side::BUY, 10.0, 100);             // 2 submits, 400 shares
        ASSERT(oms.total_submitted() == 2 && oms.total_ordered_shares() == 400, "avgsub_accum");
        ASSERT(close(oms.avg_submitted_size(), 200.0), "avgsub_200");   // 400 / 2
    }

    {   // #380 avg/max_time_to_fill_ns — submit→completion latency TCA.
        // Timestamps come from a real monotonic clock, so assert invariants
        // (zero/positive/ordering), never exact values.
        OMS oms(1000000, 1000000000.0);
        ASSERT(oms.avg_time_to_fill_ns() == 0 && oms.max_time_to_fill_ns() == 0,
               "ttf_empty_zero");
        ASSERT(oms.min_time_to_fill_ns() == 0, "ttf_min_empty_zero");   // #516
        Order* ta = oms.submit_order("AAA", Side::BUY, 10.0, 100);
        oms.fill_order(ta->order_id, 60, 10.0);            // PARTIAL — not complete yet
        ASSERT(oms.avg_time_to_fill_ns() == 0, "ttf_partial_not_counted");
        ASSERT(oms.min_time_to_fill_ns() == 0, "ttf_min_partial_not_counted");   // #516
        oms.fill_order(ta->order_id, 40, 10.0);            // FILLED — now measured
        ASSERT(oms.avg_time_to_fill_ns() > 0, "ttf_filled_positive");
        ASSERT(oms.min_time_to_fill_ns() > 0, "ttf_min_positive");   // #516
        ASSERT(oms.max_time_to_fill_ns() >= oms.avg_time_to_fill_ns(), "ttf_max_ge_avg");
        // #516: min/avg/max ordering — with a single completed order all three coincide.
        ASSERT(oms.min_time_to_fill_ns() <= oms.avg_time_to_fill_ns()
               && oms.avg_time_to_fill_ns() <= oms.max_time_to_fill_ns(), "ttf_min_le_avg_le_max");
        Order* tb = oms.submit_order("BBB", Side::SELL, 20.0, 50);
        oms.fill_order(tb->order_id, 50, 20.0);            // second completed order
        ASSERT(oms.avg_time_to_fill_ns() > 0 && oms.max_time_to_fill_ns() >= oms.avg_time_to_fill_ns(),
               "ttf_two_orders_invariant");
        ASSERT(oms.min_time_to_fill_ns() <= oms.max_time_to_fill_ns(), "ttf_min_le_max_two");   // #516
        // Cancelled working orders never enter the fill-latency stats.
        Order* tc = oms.submit_order("CCC", Side::BUY, 5.0, 10);
        const int64_t ttf_before = oms.avg_time_to_fill_ns();
        const int64_t ttf_min_before = oms.min_time_to_fill_ns();
        oms.cancel_order(tc->order_id);
        ASSERT(oms.avg_time_to_fill_ns() == ttf_before, "ttf_cancel_not_counted");
        ASSERT(oms.min_time_to_fill_ns() == ttf_min_before, "ttf_min_cancel_not_counted");   // #516
    }

    {   // #388 oldest_working_order_age_ns / oldest_working_order_id —
        // stale-order detection. sent_ns is forced to synthetic values via
        // the public Order fields so the assertions are exact.
        OMS oms(1000000, 1000000000.0);
        ASSERT(oms.oldest_working_order_age_ns(123456) == 0
               && oms.oldest_working_order_id() == 0, "owo_empty_zero");
        Order* owa = oms.submit_order("AAA", Side::BUY, 10.0, 100);
        Order* owb = oms.submit_order("BBB", Side::SELL, 20.0, 50);
        owa->sent_ns = 1000;
        owb->sent_ns = 2000;
        ASSERT(oms.oldest_working_order_id() == owa->order_id, "owo_oldest_is_a");
        ASSERT(oms.oldest_working_order_age_ns(5000) == 4000, "owo_age_exact");
        // A partial fill keeps the order WORKING — still the oldest.
        oms.fill_order(owa->order_id, 40, 10.0);
        ASSERT(oms.oldest_working_order_id() == owa->order_id, "owo_partial_still_working");
        // Completing A moves the oldest to B.
        oms.fill_order(owa->order_id, 60, 10.0);
        ASSERT(oms.oldest_working_order_id() == owb->order_id, "owo_moves_to_b");
        ASSERT(oms.oldest_working_order_age_ns(5000) == 3000, "owo_age_b");
        // now at/before the sent timestamp -> clamped to 0.
        ASSERT(oms.oldest_working_order_age_ns(2000) == 0, "owo_now_before_sent_zero");
        // Cancelling the last working order empties the watch.
        oms.cancel_order(owb->order_id);
        ASSERT(oms.oldest_working_order_age_ns(9999999) == 0
               && oms.oldest_working_order_id() == 0, "owo_cancel_empties");
    }

    {   // #412 avg_working_order_age_ns — whole-book staleness vs #388's worst.
        OMS oms(1000000, 1000000000.0);
        ASSERT(oms.avg_working_order_age_ns(5000) == 0, "awa_empty_zero");
        Order* awa = oms.submit_order("AAA", Side::BUY, 10.0, 100);
        Order* awb = oms.submit_order("BBB", Side::SELL, 20.0, 50);
        Order* awc = oms.submit_order("CCC", Side::BUY, 5.0, 10);
        awa->sent_ns = 1000;
        awb->sent_ns = 3000;
        awc->sent_ns = 5000;
        // Ages at now=6000: 5000/3000/1000 -> mean 3000; oldest (#388) is 5000.
        ASSERT(oms.avg_working_order_age_ns(6000) == 3000, "awa_mean_exact");
        ASSERT(oms.oldest_working_order_age_ns(6000) == 5000, "awa_oldest_differs");
        // One forgotten order vs a stale book: completing the fresh two
        // leaves only the old one -> mean jumps to the oldest.
        oms.fill_order(awb->order_id, 50, 20.0);
        oms.cancel_order(awc->order_id);
        ASSERT(oms.avg_working_order_age_ns(6000) == 5000, "awa_only_forgotten_left");
        // A stamp in the future clamps that order's age to 0 in the mean.
        awa->sent_ns = 7000;
        ASSERT(oms.avg_working_order_age_ns(6000) == 0, "awa_future_stamp_clamped");
    }

    {   // #420 round_trips — flat-to-flat position cycles (MILESTONE 420).
        OMS oms(1000000, 1000000000.0);
        ASSERT(oms.round_trips() == 0, "rtp_empty_zero");
        Order* rta = oms.submit_order("AAA", Side::BUY, 10.0, 100);
        oms.fill_order(rta->order_id, 100, 10.0);            // open from flat
        ASSERT(oms.round_trips() == 0, "rtp_open_not_a_trip");
        Order* rtb = oms.submit_order("AAA", Side::SELL, 10.0, 60);
        oms.fill_order(rtb->order_id, 60, 10.5);             // partial reduction
        ASSERT(oms.round_trips() == 0, "rtp_partial_close_no_trip");
        Order* rtc = oms.submit_order("AAA", Side::SELL, 10.0, 40);
        oms.fill_order(rtc->order_id, 40, 10.5);             // lands on flat
        ASSERT(oms.round_trips() == 1, "rtp_flat_completes");
        // A flip through zero closes the cycle too (long -> short, one fill).
        Order* rtd = oms.submit_order("BBB", Side::BUY, 20.0, 100);
        oms.fill_order(rtd->order_id, 100, 20.0);
        Order* rte = oms.submit_order("BBB", Side::SELL, 20.0, 150);
        oms.fill_order(rte->order_id, 150, 20.0);            // net -50: flipped
        ASSERT(oms.round_trips() == 2, "rtp_flip_counts");
        // Closing the short leg is the next completed cycle.
        Order* rtf = oms.submit_order("BBB", Side::BUY, 20.0, 50);
        oms.fill_order(rtf->order_id, 50, 19.5);
        ASSERT(oms.round_trips() == 3, "rtp_short_close_counts");

        // #428 avg_pnl_per_round_trip — per-trade expectancy of the book.
        // Realized: AAA long 100@10 sold 100@10.5 = +50; BBB long 100@20
        // closed @20 = 0; short 50@20 covered @19.5 = +25. Total +75 / 3.
        ASSERT(close(oms.avg_pnl_per_round_trip(), 25.0), "artp_75_over_3");

        // #436 symbol_round_trips — WHICH names recycle capital.
        // AAA closed once, BBB flipped once and closed once = 2 cycles.
        ASSERT(oms.symbol_round_trips("AAA") == 1, "srt_aaa_one");
        ASSERT(oms.symbol_round_trips("BBB") == 2, "srt_bbb_two");
        ASSERT(oms.symbol_round_trips("AAA") + oms.symbol_round_trips("BBB")
               == oms.round_trips(), "srt_sums_to_global");
        ASSERT(oms.symbol_round_trips("GHOST") == 0, "srt_unknown_zero");

        oms.reset_session_counters();
        ASSERT(oms.round_trips() == 0, "rtp_session_reset");
        // Denominator gone -> back to 0 (positions keep their realized P&L).
        ASSERT(oms.avg_pnl_per_round_trip() == 0.0, "artp_reset_denominator");
        // #436 lifetime note pinned: the per-name counter lives on the
        // Position and SURVIVES the session-counter reset, like realized_pnl.
        ASSERT(oms.symbol_round_trips("BBB") == 2, "srt_survives_session_reset");
    }

    {   // #444 order_fill_count / max_order_fill_count — per-order fragmentation.
        OMS oms(1000000, 1000000000.0);
        ASSERT(oms.max_order_fill_count() == 0, "ofc_empty_zero");
        Order* fca = oms.submit_order("AAA", Side::BUY, 10.0, 100);
        Order* fcb = oms.submit_order("BBB", Side::BUY, 20.0, 100);
        // AAA gets shredded into 3 odd lots; BBB fills in one print.
        oms.fill_order(fca->order_id, 30, 10.0);
        oms.fill_order(fca->order_id, 30, 10.0);
        oms.fill_order(fca->order_id, 40, 10.0);
        oms.fill_order(fcb->order_id, 100, 20.0);
        ASSERT(oms.order_fill_count(fca->order_id) == 3, "ofc_shredded_three");
        ASSERT(oms.order_fill_count(fcb->order_id) == 1, "ofc_clean_one");
        ASSERT(oms.max_order_fill_count() == 3, "ofc_max_names_worst");
        ASSERT(oms.order_fill_count(999999) == 0, "ofc_unknown_zero");
        // A fully-clamped over-fill (0 applied) does not count a slice.
        oms.fill_order(fca->order_id, 50, 10.0);   // AAA already complete
        ASSERT(oms.order_fill_count(fca->order_id) == 3, "ofc_clamped_overfill_ignored");
        // Consistency: slices across orders sum to the global fill counter.
        ASSERT(oms.order_fill_count(fca->order_id) + oms.order_fill_count(fcb->order_id)
               == oms.total_fills(), "ofc_sums_to_total_fills");
    }

    {   // #452 avg_time_to_cancel_ns — quote lifetime (forced stamps, exact).
        OMS oms(1000000, 1000000000.0);
        ASSERT(oms.avg_time_to_cancel_ns() == 0, "ttc_empty_zero");
        ASSERT(oms.min_time_to_cancel_ns() == 0, "ttc_min_empty_zero");   // #524
        Order* tca_ = oms.submit_order("AAA", Side::BUY, 10.0, 100);
        Order* tcb_ = oms.submit_order("BBB", Side::SELL, 20.0, 50);
        Order* tcc_ = oms.submit_order("CCC", Side::BUY, 5.0, 10);
        oms.cancel_order(tca_->order_id);
        oms.cancel_order(tcb_->order_id);
        // Force the stamps for exact math: lifetimes 1000 and 3000 -> mean 2000.
        tca_->sent_ns = 1000; tca_->cancelled_ns = 2000;
        tcb_->sent_ns = 1000; tcb_->cancelled_ns = 4000;
        ASSERT(oms.avg_time_to_cancel_ns() == 2000, "ttc_mean_exact");
        // A still-working order contributes nothing; a filled one neither.
        oms.fill_order(tcc_->order_id, 10, 5.0);
        ASSERT(oms.avg_time_to_cancel_ns() == 2000, "ttc_fill_not_counted");
        // The two lifetime reads split the terminal population: fills go to
        // #380's clock, cancels to this one.
        ASSERT(oms.avg_time_to_fill_ns() > 0, "ttc_fill_side_separate");
        // #460 max_time_to_cancel_ns — the tail of the cancel lifetime.
        ASSERT(oms.max_time_to_cancel_ns() == 3000, "ttc_max_tail");
        // #524 min_time_to_cancel_ns — the reflex pull (shortest of 1000 and 3000).
        ASSERT(oms.min_time_to_cancel_ns() == 1000, "ttc_min_tail");
        ASSERT(oms.min_time_to_cancel_ns() <= oms.avg_time_to_cancel_ns()
               && oms.avg_time_to_cancel_ns() <= oms.max_time_to_cancel_ns(), "ttc_min_le_avg_le_max");
    }

    {   // #460 realized_pnl_per_share — the per-share edge (MILESTONE 460).
        OMS oms(1000000, 1000000000.0);
        ASSERT(oms.realized_pnl_per_share() == 0.0, "rps_empty_zero");
        // Buy 100 @ 10.00, sell 100 @ 10.50 -> +50 realized over 200 filled
        // shares of turnover -> 0.25 $/share.
        Order* rpa = oms.submit_order("AAA", Side::BUY, 10.0, 100);
        oms.fill_order(rpa->order_id, 100, 10.0);
        Order* rpb = oms.submit_order("AAA", Side::SELL, 10.5, 100);
        oms.fill_order(rpb->order_id, 100, 10.5);
        ASSERT(close(to_float(oms.total_realized_pnl()), 50.0), "rps_realized_50");
        ASSERT(oms.total_filled_shares() == 200, "rps_turnover_200");
        ASSERT(close(oms.realized_pnl_per_share(), 0.25), "rps_quarter_per_share");
        // A losing round trip pulls the per-share edge negative.
        Order* rpc = oms.submit_order("BBB", Side::BUY, 20.0, 100);
        oms.fill_order(rpc->order_id, 100, 20.0);
        Order* rpd = oms.submit_order("BBB", Side::SELL, 19.0, 100);
        oms.fill_order(rpd->order_id, 100, 19.0);      // -100 over +50 -> -50 / 400
        ASSERT(close(oms.realized_pnl_per_share(), -50.0 / 400.0), "rps_negative_after_loss");
    }

    {   // #468 net_pnl_per_share — the post-fee edge (the honest number).
        OMS oms(1000000, 1000000000.0, /*commission_per_share=*/0.01);
        ASSERT(oms.net_pnl_per_share() == 0.0, "nps_empty_zero");
        // Buy 100 @ 10.00, sell 100 @ 10.50 -> +50 gross, 200 shares, fees
        // 200*0.01 = 2.00 -> net 48. gross 0.25/sh, net 0.24/sh.
        Order* nsa = oms.submit_order("AAA", Side::BUY, 10.0, 100);
        oms.fill_order(nsa->order_id, 100, 10.0);
        Order* nsb = oms.submit_order("AAA", Side::SELL, 10.5, 100);
        oms.fill_order(nsb->order_id, 100, 10.5);
        ASSERT(close(oms.realized_pnl_per_share(), 0.25), "nps_gross_quarter");
        ASSERT(close(oms.net_pnl_per_share(), 0.24), "nps_net_after_fees");
        // Identity: net == gross - avg commission per share.
        ASSERT(close(oms.net_pnl_per_share(),
                     oms.realized_pnl_per_share() - oms.avg_commission_per_share()),
               "nps_identity_gross_minus_comm");
    }

    {   // #468: a thin gross edge below the per-share fee reads NEGATIVE net.
        OMS oms(1000000, 1000000000.0, /*commission_per_share=*/0.01);
        Order* tna = oms.submit_order("BBB", Side::BUY, 10.0, 100);
        oms.fill_order(tna->order_id, 100, 10.0);
        Order* tnb = oms.submit_order("BBB", Side::SELL, 10.005, 100);
        oms.fill_order(tnb->order_id, 100, 10.005);   // +0.5 gross over 200 = 0.0025/sh
        ASSERT(oms.realized_pnl_per_share() > 0.0, "nps_thin_gross_positive");
        ASSERT(oms.net_pnl_per_share() < 0.0, "nps_fees_eat_the_edge");
    }

    {   // #396 price improvement vs limit — the price-quality TCA axis.
        OMS oms(1000000, 1000000000.0);
        ASSERT(oms.total_price_improvement() == 0.0
               && oms.avg_price_improvement_per_share() == 0.0, "pimp_empty_zero");
        Order* pia = oms.submit_order("AAA", Side::BUY, 10.00, 100);
        oms.fill_order(pia->order_id, 100, 9.98);           // 0.02 better x 100 = +2.00
        ASSERT(close(oms.total_price_improvement(), 2.0), "pimp_buy_below_limit");
        Order* pib = oms.submit_order("BBB", Side::SELL, 20.00, 50);
        oms.fill_order(pib->order_id, 50, 20.10);           // 0.10 better x 50 = +5.00
        ASSERT(close(oms.total_price_improvement(), 7.0), "pimp_sell_above_limit");
        // A fill exactly at the limit contributes nothing.
        Order* pic = oms.submit_order("CCC", Side::BUY, 5.00, 40);
        oms.fill_order(pic->order_id, 40, 5.00);
        ASSERT(close(oms.total_price_improvement(), 7.0), "pimp_at_limit_zero");
        // Filled PAST the limit drags the total negative (SELL below limit).
        Order* pid_ = oms.submit_order("DDD", Side::SELL, 30.00, 100);
        oms.fill_order(pid_->order_id, 100, 29.90);         // -0.10 x 100 = -10.00
        ASSERT(close(oms.total_price_improvement(), -3.0), "pimp_slippage_negative");
        // Per executed share: -3.00 / (100+50+40+100) shares.
        ASSERT(close(oms.avg_price_improvement_per_share(), -3.0 / 290.0), "pimp_avg_per_share");
        oms.reset_session_counters();
        ASSERT(oms.total_price_improvement() == 0.0, "pimp_session_reset");
    }

    {   // #404 open_order_notional_symbol — per-name slice of #180.
        OMS oms(1000000, 1000000000.0);
        ASSERT(oms.open_order_notional_symbol("AAA") == 0.0, "oons_empty_zero");
        Order* na = oms.submit_order("AAA", Side::BUY, 10.0, 100);   // $1000 working
        oms.submit_order("AAA", Side::SELL, 12.0, 50);               // $600 working
        oms.submit_order("BBB", Side::BUY, 20.0, 200);               // $4000 working
        ASSERT(close(oms.open_order_notional_symbol("AAA"), 1600.0), "oons_aaa_sum");
        ASSERT(close(oms.open_order_notional_symbol("BBB"), 4000.0), "oons_bbb");
        // The per-symbol slices sum to the global #180 view.
        ASSERT(close(oms.open_order_notional_symbol("AAA")
                     + oms.open_order_notional_symbol("BBB"),
                     oms.open_order_notional()), "oons_slices_sum_to_global");
        // A partial fill counts only the unfilled remainder.
        oms.fill_order(na->order_id, 40, 10.0);                      // 60 left -> $600
        ASSERT(close(oms.open_order_notional_symbol("AAA"), 1200.0), "oons_partial_remainder");
        // Terminal orders drop out; other names untouched.
        oms.cancel_all_symbol("AAA");
        ASSERT(oms.open_order_notional_symbol("AAA") == 0.0, "oons_cancel_zeroes");
        ASSERT(close(oms.open_order_notional_symbol("BBB"), 4000.0), "oons_bbb_untouched");
        ASSERT(oms.open_order_notional_symbol("GHOST") == 0.0, "oons_unknown_zero");
    }

    {   // #166 runtime commission change.
        OMS oms(100000, 1000000000.0, /*commission_per_share=*/0.005);
        ASSERT(close(oms.commission_per_share(), 0.005), "comm_initial");
        oms.set_commission(0.01);
        ASSERT(close(oms.commission_per_share(), 0.01), "comm_updated");
        Order* o = oms.submit_order("AAA", Side::BUY, 10.0, 100);
        oms.fill_order(o->order_id, 100, 10.0);          // fee 100 * 0.01 = 1.0
        ASSERT(close(to_float(oms.total_fees()), 1.0), "comm_new_rate_applied");
    }

    {   // #151 lifecycle operation counters (fills/cancels/replaces).
        OMS oms(100000, 1000000000.0);
        Order* a = oms.submit_order("AAA", Side::BUY, 10.0, 100);
        oms.fill_order(a->order_id, 50, 10.0);            // fill 1
        oms.fill_order(a->order_id, 50, 10.0);            // fill 2
        Order* b = oms.submit_order("BBB", Side::BUY, 10.0, 100);
        oms.replace_order(b->order_id, 11.0, 80);         // replace 1
        oms.cancel_order(b->order_id);                    // cancel 1
        ASSERT(oms.total_fills() == 2, "ops_fills_2");
        ASSERT(oms.total_replaces() == 1, "ops_replaces_1");
        ASSERT(oms.total_cancels() == 1, "ops_cancels_1");
        ASSERT(oms.total_submitted() == 2, "ops_submitted_2");   // #160 (AAA, BBB)
    }

    {   // #141 avg_fill_price — an order filled at two prices.
        OMS oms(100000, 1000000000.0);
        Order* o = oms.submit_order("AAPL", Side::BUY, 100.00, 100);
        oms.fill_order(o->order_id, 40, 100.00);                  // 40 @ 100.00
        oms.fill_order(o->order_id, 60, 101.00);                  // 60 @ 101.00
        const Order* r = oms.get_order(o->order_id);
        // avg = (40*100 + 60*101)/100 = 100.60
        ASSERT(close(to_float(r->avg_fill_price()), 100.60), "avg_fill_price_blended");
    }

    {   // #128 count_by_status — order-status observability.
        OMS oms(1000000, 1000000000.0);
        oms.submit_order("AAA", Side::BUY, 10.0, 100);                      // SENT
        Order* f = oms.submit_order("BBB", Side::BUY, 10.0, 100);
        oms.fill_order(f->order_id, 100, 10.0);                            // FILLED
        Order* p = oms.submit_order("CCC", Side::BUY, 10.0, 100);
        oms.fill_order(p->order_id, 40, 10.0);                             // PARTIAL
        ASSERT(oms.count_by_status(OrderStatus::SENT) == 1, "cbs_sent_1");
        ASSERT(oms.count_by_status(OrderStatus::FILLED) == 1, "cbs_filled_1");
        ASSERT(oms.count_by_status(OrderStatus::PARTIAL) == 1, "cbs_partial_1");
    }

    {   // #120 portfolio P&L aggregates (realized/net over all positions).
        OMS oms(100000, 100000000.0, /*commission_per_share=*/0.01);
        // AAPL: buy 100@50, sell 100@52 -> realized +200, fees 2
        oms.fill_order(oms.submit_order("AAPL", Side::BUY,  50.00, 100)->order_id, 100, 50.00);
        oms.fill_order(oms.submit_order("AAPL", Side::SELL, 52.00, 100)->order_id, 100, 52.00);
        // MSFT: buy 50@10, sell 50@9 -> realized -50, fees 1
        oms.fill_order(oms.submit_order("MSFT", Side::BUY,  10.00, 50)->order_id, 50, 10.00);
        oms.fill_order(oms.submit_order("MSFT", Side::SELL,  9.00, 50)->order_id, 50,  9.00);
        ASSERT(close(to_float(oms.total_realized_pnl()), 150.0), "agg_realized_150");  // 200-50
        ASSERT(close(to_float(oms.total_net_pnl()), 147.0), "agg_net_147");            // 150 - 3 fees
    }

    {   // #96 unrealized P&L (mark-to-market) for long and short.
        OMS oms(10000, 100000000.0);
        oms.fill_order(oms.submit_order("AAPL", Side::BUY, 50.00, 100)->order_id, 100, 50.00);
        const Position* lp = oms.get_position("AAPL");
        ASSERT(close(to_float(lp->unrealized_pnl(to_fixed(52.00))), 200.0), "mtm_long_up");   // (52-50)*100
        ASSERT(close(to_float(lp->unrealized_pnl(to_fixed(48.00))), -200.0), "mtm_long_down");
        OMS oms2(10000, 100000000.0);
        oms2.fill_order(oms2.submit_order("AAPL", Side::SELL, 50.00, 100)->order_id, 100, 50.00);
        const Position* sp = oms2.get_position("AAPL");
        ASSERT(close(to_float(sp->unrealized_pnl(to_fixed(48.00))), 200.0), "mtm_short_profit"); // short profits when it drops
        ASSERT(close(to_float(sp->total_pnl(to_fixed(48.00))), 200.0), "mtm_total_short");       // realized 0 + unrl 200
    }

    {   // #88 reject reasons — the caller distinguishes WHY it was rejected.
        OMS oms(/*max_pos=*/100, /*max_val=*/1000.0);
        OMSReject why = OMSReject::NONE;
        ASSERT(oms.submit_order("AAPL", Side::BUY, -1.0, 10, &why) == nullptr
               && why == OMSReject::INVALID_INPUT, "rej_invalid_input");
        ASSERT(oms.submit_order("AAPL", Side::BUY, 100.0, 50, &why) == nullptr
               && why == OMSReject::ORDER_VALUE, "rej_order_value");        // 5000>1000
        ASSERT(oms.submit_order("AAPL", Side::BUY, 1.0, 200, &why) == nullptr
               && why == OMSReject::POSITION_LIMIT, "rej_position_limit");  // qty 200>100
        Order* ok = oms.submit_order("AAPL", Side::BUY, 1.0, 10, &why);
        ASSERT(ok && why == OMSReject::NONE && oms.last_reject() == OMSReject::NONE,
               "rej_none_on_success");
        // #136 reject statistics per reason (above: 1x each type).
        ASSERT(oms.reject_count(OMSReject::INVALID_INPUT) == 1, "rejcnt_invalid");
        ASSERT(oms.reject_count(OMSReject::ORDER_VALUE) == 1, "rejcnt_value");
        ASSERT(oms.reject_count(OMSReject::POSITION_LIMIT) == 1, "rejcnt_position");
        ASSERT(oms.reject_count(OMSReject::NONE) == 0, "rejcnt_none_zero");
    }
}


// =====================================================
// Risk Manager Tests
// =====================================================

void test_risk() {
    SECTION("Risk Manager");
    RiskLimits limits;
    limits.max_position_per_symbol = 500;
    limits.max_order_value = 100000;
    limits.max_orders_per_second = 1000;
    RiskManager risk(limits);

    // Accept normal order
    auto ok = risk.check_order("AAPL", Side::BUY, 150.25, 100);
    ASSERT(ok.action == RiskAction::ALLOW, "risk_accept_normal");

    // Reject overvalued order
    auto reject_val = risk.check_order("NVDA", Side::BUY, 900.0, 200);
    ASSERT(reject_val.action != RiskAction::ALLOW, "risk_reject_value");

    // Kill switch
    ASSERT(!risk.is_kill_switch_active(), "risk_not_killed");
    risk.activate_kill_switch();
    ASSERT(risk.is_kill_switch_active(), "risk_killed");

    // All orders rejected after kill switch
    auto post_kill = risk.check_order("AAPL", Side::BUY, 10.0, 1);
    ASSERT(post_kill.action != RiskAction::ALLOW, "risk_reject_after_kill");

    // Reset
    risk.deactivate_kill_switch();
    ASSERT(!risk.is_kill_switch_active(), "risk_reset");
    auto post_reset = risk.check_order("AAPL", Side::BUY, 10.0, 1);
    ASSERT(post_reset.action == RiskAction::ALLOW, "risk_accept_after_reset");

    printf("  Risk: %d assertions\n", 8);
}


// =====================================================
// Smart Router Tests
// =====================================================

void test_router() {
    SECTION("Smart Router");
    SmartOrderRouter router(RoutingStrategy::BEST_PRICE, 500);

    Venue nyse, nasdaq, bats;
    strncpy(nyse.name, "NYSE", 15);     nyse.latency_ns = 500;   nyse.fee_per_share = 0.003;
    strncpy(nasdaq.name, "NASDAQ", 15); nasdaq.latency_ns = 350;  nasdaq.fee_per_share = 0.002;
    strncpy(bats.name, "BATS", 15);     bats.latency_ns = 250;   bats.fee_per_share = 0.001;
    router.add_venue(nyse);
    router.add_venue(nasdaq);
    router.add_venue(bats);

    // Update quotes — BATS has best bid
    router.update_quote("NYSE",   150.20, 150.30, 200, 200);
    router.update_quote("NASDAQ", 150.22, 150.28, 300, 300);
    router.update_quote("BATS",   150.25, 150.26, 100, 100);

    // Best price routing for BUY should pick lowest ask
    RouteDecision rd = router.route_order("BUY", 100);
    ASSERT(rd.valid, "router_buy_valid");
    ASSERT(strcmp(rd.venue, "BATS") == 0, "router_buy_best_price_bats");

    // Best price routing for SELL should pick highest bid
    rd = router.route_order("SELL", 100);
    ASSERT(rd.valid, "router_sell_valid");
    ASSERT(strcmp(rd.venue, "BATS") == 0, "router_sell_best_price_bats");

    // Lowest latency routing
    SmartOrderRouter lat_router(RoutingStrategy::LOWEST_LATENCY, 500);
    lat_router.add_venue(nyse);
    lat_router.add_venue(nasdaq);
    lat_router.add_venue(bats);
    lat_router.update_quote("NYSE",   150.20, 150.30, 200, 200);
    lat_router.update_quote("NASDAQ", 150.22, 150.28, 300, 300);
    lat_router.update_quote("BATS",   150.25, 150.26, 100, 100);

    rd = lat_router.route_order("BUY", 100);
    ASSERT(rd.valid, "router_latency_valid");
    ASSERT(strcmp(rd.venue, "BATS") == 0, "router_lowest_latency_bats");

    // No quotes — should return invalid
    SmartOrderRouter empty_router;
    rd = empty_router.route_order("BUY", 100);
    ASSERT(!rd.valid, "router_no_venues_invalid");
    // #125 reject reason: no venue vs no liquidity
    ASSERT(rd.reject_reason == RouteReject::NO_VENUES, "router_reject_no_venues");
    SmartOrderRouter dry(RoutingStrategy::BEST_PRICE);
    dry.add_venue(Venue("A", 100, 0.0));         // venue exists, but without a quote
    const RouteDecision rdl = dry.route_order("BUY", 100);
    ASSERT(!rdl.valid && rdl.reject_reason == RouteReject::NO_LIQUIDITY,
           "router_reject_no_liquidity");
    rd = router.route_order("BUY", 100);          // udana trasa
    ASSERT(rd.valid && rd.reject_reason == RouteReject::NONE, "router_reject_none_on_ok");

    printf("  Router: %d assertions\n", 7);
}


// =====================================================
// Trade Logger Tests
// =====================================================

void test_logger() {
    SECTION("Trade Logger");
    TradeLogger logger;

    // Log events
    logger.log(EventType::SYSTEM_START);
    logger.log(EventType::ORDER_SUBMIT, 1, "AAPL", "BUY", 100, 150.25);
    logger.log(EventType::RISK_ACCEPT, 1, "AAPL");
    logger.log(EventType::ORDER_FILL, 1, "AAPL", "BUY", 100, 150.25, "venue=NASDAQ");

    ASSERT(logger.total_events() == 4, "logger_total_events");
    ASSERT(logger.sequence() == 4, "logger_sequence");
    ASSERT(logger.get_counter(EventType::ORDER_SUBMIT) == 1, "logger_submit_count");
    ASSERT(logger.get_counter(EventType::ORDER_FILL) == 1, "logger_fill_count");

    // Filter by order
    TradeEvent trail[16];
    int n = logger.get_order_trail(1, trail, 16);
    ASSERT(n == 3, "logger_trail_count");
    ASSERT(trail[0].event_type == EventType::ORDER_SUBMIT, "logger_trail_first");
    ASSERT(trail[2].event_type == EventType::ORDER_FILL, "logger_trail_last");

    // Unique counts
    ASSERT(logger.unique_orders() == 1, "logger_unique_orders");
    ASSERT(logger.unique_symbols() == 1, "logger_unique_symbols");

    // Time span
    ASSERT(logger.time_span_ms() >= 0.0, "logger_time_span");

    // Ring buffer mode
    TradeLogger ring_logger(true, 5);
    for (int i = 0; i < 10; ++i) {
        ring_logger.log(EventType::ORDER_SUBMIT, i + 1, "TEST", "BUY", 100, 1.0);
    }
    ASSERT(ring_logger.total_events() == 5, "logger_ring_buffer_count");
    ASSERT(ring_logger.total_logged() == 10, "logger_ring_total_logged");
    ASSERT(ring_logger.buffer_full(), "logger_ring_full");

    // Latency stats
    auto lstats = logger.get_latency_stats();
    ASSERT(lstats.count == 3, "logger_latency_count");
    ASSERT(lstats.min_ns >= 0, "logger_latency_min");
    ASSERT(lstats.p50_ns >= 0, "logger_latency_p50");

    // Empty logger
    TradeLogger empty;
    ASSERT(empty.total_events() == 0, "logger_empty");
    ASSERT(empty.time_span_ms() == 0.0, "logger_empty_span");

    printf("  Logger: %d assertions\n", 20);
}


// =====================================================
// Strategy Tests
// =====================================================

void test_strategy() {
    SECTION("Strategy");
    MeanReversionStrategy strategy(5, 1.0, 100);  // window=5, threshold=1%

    // Feed prices to build up the SMA window
    for (int i = 0; i < 5; ++i) {
        Signal s = strategy.on_market_data("AAPL", 150.0, 0);
        ASSERT(!s.valid, "strategy_building_window");
    }

    // Price at mean — should be HOLD
    Signal s = strategy.on_market_data("AAPL", 150.0, 0);
    // May or may not trigger depending on threshold

    // Price significantly below mean — 145 is 3.3% below SMA=150, threshold=1% → must trigger BUY
    s = strategy.on_market_data("AAPL", 145.0, 0);
    ASSERT(s.valid, "strategy_buy_signal_valid");
    ASSERT(s.side == Side::BUY, "strategy_buy_signal");

    // Reset: feed 5 prices at 150 so SMA≈150, then spike to 160 (6.7% above) → must trigger SELL
    for (int i = 0; i < 5; ++i) {
        strategy.on_market_data("AAPL", 150.0, 0);
    }
    s = strategy.on_market_data("AAPL", 160.0, 0);
    ASSERT(s.valid, "strategy_sell_signal_valid");
    ASSERT(s.side == Side::SELL, "strategy_sell_signal");

    // Different stocks tracked independently
    strategy.on_market_data("TSLA", 245.0, 0);
    strategy.on_market_data("MSFT", 410.0, 0);
    // Should not crash or mix up

    printf("  Strategy: assertions passed\n");
}


void test_strategy_edge_cases() {
    SECTION("Strategy Edge Cases");

    // window=0 and negative window must clamp to ≥1 — no divide-by-zero in add()/sma()
    MeanReversionStrategy s0(0,  1.0, 100);  s0.on_market_data("AAPL", 150.0, 0);
    MeanReversionStrategy sn(-5, 1.0, 100);  sn.on_market_data("AAPL", 150.0, 0);
    ASSERT(true, "strategy_window_clamp_no_crash");  // reaching here = no UB

    // NaN / Inf / non-positive prices must be rejected (HOLD), not propagated
    MeanReversionStrategy s(5, 1.0, 100);
    for (int i = 0; i < 5; ++i) s.on_market_data("AAPL", 150.0, 0);  // build SMA=150
    ASSERT(!s.on_market_data("AAPL", std::nan(""), 0).valid,                          "strategy_nan_holds");
    ASSERT(!s.on_market_data("AAPL", std::numeric_limits<double>::infinity(), 0).valid, "strategy_inf_holds");
    ASSERT(!s.on_market_data("AAPL", -10.0, 0).valid,        "strategy_negative_holds");
    ASSERT(!s.on_market_data("AAPL", 0.0, 0).valid,          "strategy_zero_holds");

    // Sub-threshold deviation must HOLD (only deviation strictly > threshold triggers).
    // After many 150s the SMA stays ~150; 151.0 ≈ 0.66% < 1% threshold → HOLD.
    Signal sub = s.on_market_data("AAPL", 151.0, 0);
    ASSERT(!sub.valid, "strategy_sub_threshold_holds");

    printf("  Strategy edge: 6 assertions\n");
}


// =====================================================
// Market Maker — quote / fill / inventory skew / max-inventory suppression
// =====================================================

void test_market_maker() {
    SECTION("Market Maker");

    // --- Neutral state: symmetric quotes around mid ---
    mm::MMConfig cfg;
    cfg.quote_size          = 10;
    cfg.half_spread_ticks   = 5;
    cfg.max_inventory       = 100;
    cfg.risk_aversion_ticks = 0.0;  // no skew for the symmetry test

    mm::MarketMaker maker(cfg, "AAPL");
    mm::Quote q = maker.quote(/*best_bid=*/15000, /*best_ask=*/15010);

    ASSERT(q.bid_size == 10,               "mm_initial_bid_size");
    ASSERT(q.ask_size == 10,               "mm_initial_ask_size");
    ASSERT(q.ask_price - q.bid_price == 10,"mm_spread_two_halfspreads");
    const int32_t mid = (15000 + 15010) / 2;
    ASSERT(q.bid_price == mid - 5,         "mm_bid_at_mid_minus_half");
    ASSERT(q.ask_price == mid + 5,         "mm_ask_at_mid_plus_half");

    // --- Fills: BUY raises position, SELL lowers ---
    maker.apply_fill(Side::BUY, 30, q.bid_price);
    ASSERT(maker.position() == 30, "mm_buy_raises_position");
    maker.apply_fill(Side::SELL, 10, q.ask_price);
    ASSERT(maker.position() == 20, "mm_sell_lowers_position");
    ASSERT(maker.fills_received() == 2, "mm_fills_counted");

    // --- Inventory skew: long position pushes BOTH quotes down ---
    mm::MMConfig skew_cfg = cfg;
    skew_cfg.risk_aversion_ticks = 1.0;   // 1 tick of skew per share
    mm::MarketMaker sk(skew_cfg, "AAPL");
    sk.apply_fill(Side::BUY, 5, 15000);   // position = +5 → skew = -5 ticks
    mm::Quote skq = sk.quote(15000, 15010);
    // neutral would be bid=15000, ask=15010; with skew -5 → bid=14995, ask=15005
    ASSERT(skq.bid_price == 14995, "mm_long_skews_bid_down");
    ASSERT(skq.ask_price == 15005, "mm_long_skews_ask_down");

    // --- Max-inventory hit: BID suppressed when at long limit ---
    mm::MMConfig cap_cfg = cfg;
    cap_cfg.risk_aversion_ticks = 0.0;
    mm::MarketMaker capped(cap_cfg, "AAPL");
    capped.apply_fill(Side::BUY, 100, 15000);  // position == +100 == max_inventory
    mm::Quote cq = capped.quote(15000, 15010);
    ASSERT(cq.bid_size == 0,  "mm_no_bid_at_long_cap");
    ASSERT(cq.ask_size == 10, "mm_still_quotes_ask_at_long_cap");

    // --- Symmetric: short cap suppresses ASK ---
    mm::MarketMaker shorted(cap_cfg, "AAPL");
    shorted.apply_fill(Side::SELL, 100, 15010);  // position == -100 == -max_inventory
    mm::Quote sq = shorted.quote(15000, 15010);
    ASSERT(sq.ask_size == 0,  "mm_no_ask_at_short_cap");
    ASSERT(sq.bid_size == 10, "mm_still_quotes_bid_at_short_cap");

    // --- P&L sanity: buy 10 @ 14995, sell 10 @ 15005 = +10 ticks × 10 shares = $1.00 ---
    mm::MarketMaker pnl(cfg, "AAPL");
    pnl.apply_fill(Side::BUY,  10, 14995);
    pnl.apply_fill(Side::SELL, 10, 15005);
    const double earned = pnl.pnl(/*mid_ticks=*/15000);
    ASSERT(earned == 1.0, "mm_pnl_round_trip_one_dollar");
}


// =====================================================
// FIX Protocol Tests
// =====================================================

void test_fix() {
    SECTION("FIX Protocol");

    // Parse a New Order Single (tag 35=D)
    const char* nos = "8=FIX.4.2|35=D|49=CLIENT|56=EXCHANGE|11=ORD001|55=AAPL|54=1|38=100|44=150.25|40=2|";
    FIXMessage msg1;
    int64_t parse_ns = msg1.parse(nos);
    ASSERT(parse_ns >= 0, "fix_parse_time");
    ASSERT(strcmp(msg1.get_msg_type(), "D") == 0, "fix_msg_type_D");
    ASSERT(strcmp(msg1.get_symbol(), "AAPL") == 0, "fix_symbol_aapl");
    ASSERT(strcmp(msg1.get_side(), "BUY") == 0, "fix_side_buy");
    ASSERT(msg1.get_quantity() == 100, "fix_qty_100");
    ASSERT(fabs(msg1.get_price() - 150.25) < 0.01, "fix_price");
    ASSERT(msg1.field_count() > 0, "fix_field_count");

    // Parse a Sell order
    const char* sell = "8=FIX.4.2|35=D|49=CLIENT|56=EXCHANGE|11=ORD002|55=TSLA|54=2|38=50|44=245.00|40=1|";
    FIXMessage msg2;
    msg2.parse(sell);
    ASSERT(strcmp(msg2.get_side(), "SELL") == 0, "fix_side_sell");
    ASSERT(strcmp(msg2.get_symbol(), "TSLA") == 0, "fix_symbol_tsla");
    ASSERT(msg2.get_quantity() == 50, "fix_qty_50");

    // Parse a Heartbeat
    const char* hb = "8=FIX.4.2|35=0|49=CLIENT|56=EXCHANGE|";
    FIXMessage msg3;
    msg3.parse(hb);
    ASSERT(strcmp(msg3.get_msg_type(), "0") == 0, "fix_heartbeat_type");

    // Empty message
    FIXMessage msg4;
    msg4.parse("");
    ASSERT(msg4.field_count() == 0, "fix_empty_msg");

    printf("  FIX: %d assertions\n", 12);
}


// =====================================================
// OUCH Protocol Tests
// =====================================================

void test_ouch() {
    SECTION("OUCH Protocol");

    // Encode enter order (static method)
    uint8_t buf[64];
    int len = OUCHMessage::enter_order(buf, "TOK001", 'B', 100, "AAPL", 150.25);
    ASSERT(len == 33, "ouch_encode_enter");
    ASSERT(buf[0] == 'O', "ouch_enter_type");

    // Encode cancel
    len = OUCHMessage::cancel_order(buf, "TOK001");
    ASSERT(len == 19, "ouch_encode_cancel");
    ASSERT(buf[0] == 'X', "ouch_cancel_type");

    // Encode replace
    len = OUCHMessage::replace_order(buf, "TOK001", "TOK002", 200, 151.00);
    ASSERT(len == 37, "ouch_encode_replace");
    ASSERT(buf[0] == 'U', "ouch_replace_type");

    // Parse accepted response
    uint8_t accepted[48];
    memset(accepted, 0, sizeof(accepted));
    accepted[0] = 'A';
    // OUCH wire format: 14-byte fixed-width token, space-padded, not null-terminated.
    // NOLINTNEXTLINE(bugprone-not-null-terminated-result)
    memcpy(accepted + 1, "TOK001        ", 14);
    OUCHResponse parsed = OUCHMessage::parse_response(accepted, 41);
    ASSERT(strcmp(parsed.type, "ACCEPTED") == 0, "ouch_parse_accepted");
    ASSERT(strcmp(parsed.token, "TOK001") == 0, "ouch_accepted_token");

    printf("  OUCH: %d assertions\n", 8);
}


void test_fill_simulator() {
    SECTION("FillSimulator");
    // Default config — partial 20%, slippage mean 1 tick, reject 2%.
    common::FillSimulator sim({}, /*seed*/42);

    // 1000 attempts → non-zero reject rate, partial rate and slippage.
    int rejected = 0, partial = 0, exact = 0;
    int64_t total_slippage = 0;
    for (int i = 0; i < 1000; ++i) {
        auto r = sim.simulate(Side::BUY, 100, 22381, /*displayed*/500,
                              common::Urgency::MARKETABLE);
        if (r.rejected)                 ++rejected;
        else if (r.fill_qty < 100)      ++partial;
        else                             ++exact;
        total_slippage += r.slippage_ticks;
    }
    ASSERT(rejected > 5  && rejected < 80, "fillsim_reject_in_range");   // ~20 ± noise
    ASSERT(partial  > 100,                  "fillsim_partials_happen");
    ASSERT(exact    > 400,                  "fillsim_mostly_full_fills");
    ASSERT(total_slippage > 0,              "fillsim_slippage_nonzero");

    // Determinizm — ten sam seed → ta sama sekwencja.
    common::FillSimulator s1({}, 7);
    common::FillSimulator s2({}, 7);
    auto a = s1.simulate(Side::BUY, 100, 10000, 1000);
    auto b = s2.simulate(Side::BUY, 100, 10000, 1000);
    ASSERT(a.fill_qty         == b.fill_qty &&
           a.fill_price_ticks == b.fill_price_ticks &&
           a.rejected         == b.rejected,         "fillsim_deterministic_seed");

    // Aggressive urgency → larger slippage than passive (statistically).
    common::FillSimulator agg({}, 11);
    common::FillSimulator pas({}, 11);
    int64_t agg_slip = 0, pas_slip = 0;
    for (int i = 0; i < 200; ++i) {
        auto ra = agg.simulate(Side::BUY, 100, 10000, 1000, common::Urgency::AGGRESSIVE);
        auto rp = pas.simulate(Side::BUY, 100, 10000, 1000, common::Urgency::PASSIVE);
        if (!ra.rejected) agg_slip += ra.slippage_ticks;
        if (!rp.rejected) pas_slip += rp.slippage_ticks;
    }
    ASSERT(agg_slip > pas_slip, "fillsim_urgency_aggressive_more_slippage");

    printf("  FillSimulator: %d assertions\n", 6);
}


// =====================================================
// Market Simulator Integration Tests
// =====================================================

// =====================================================
// SPSC Queue — concurrent stress test
// =====================================================

void test_spsc_queue() {
    SECTION("SPSC Queue (concurrent)");

    lockfree::SPSCQueue<int, 4096> q{};
    constexpr int N = 100'000;
    std::atomic<bool> stop{false};
    int last_seq = -1;
    bool gap = false, repeat = false;

    // Consumer drains until producer signals stop and queue is empty
    std::thread consumer([&]() {
        int v = 0;
        // cppcheck-suppress uninitvar  // q is the SPSCQueue declared above (template ctor not modeled)
        while (!stop.load(std::memory_order_relaxed) || !q.empty()) {
            if (q.pop(v)) {
                if (v <  last_seq + 1) repeat = true;  // out-of-order or duplicate
                if (v >  last_seq + 1) gap    = true;  // missing element
                last_seq = v;
            }
        }
    });

    // Producer pushes 0..N-1 in order, spinning if queue is full
    for (int i = 0; i < N; ++i) while (!q.push(i)) {}
    stop.store(true, std::memory_order_relaxed);
    consumer.join();

    ASSERT(last_seq == N - 1, "spsc_all_received");
    ASSERT(!gap,             "spsc_no_gaps");
    ASSERT(!repeat,          "spsc_no_repeats_or_reorders");
    ASSERT(q.empty(),        "spsc_drained");

    printf("  SPSC: 4 assertions (%d msgs)\n", N);
}


// =====================================================
// MPSC Queue — 4 producers × 25k → 1 consumer, per-producer FIFO
// =====================================================

void test_mpsc_queue() {
    SECTION("MPSC Queue (4 producers)");
    lockfree::MPSCQueue<uint64_t, 4096> q;

    constexpr int PRODS = 4;
    constexpr int PER   = 25'000;
    auto encode = [](int pid, int seq) {
        return (static_cast<uint64_t>(pid) << 32) | static_cast<uint32_t>(seq);
    };

    std::atomic<int> done{0};
    int  last_seen[PRODS] = {-1, -1, -1, -1};
    int  received = 0;
    bool fifo_ok  = true;
    bool no_dup   = true;

    std::thread consumer([&]() {
        uint64_t v;
        while (done.load(std::memory_order_acquire) < PRODS || !q.empty()) {
            if (q.pop(v)) {
                const int pid = static_cast<int>(v >> 32);
                const int seq = static_cast<int>(v & 0xFFFFFFFFu);
                if (seq == last_seen[pid]) no_dup  = false;
                else if (seq != last_seen[pid] + 1) fifo_ok = false;
                last_seen[pid] = seq;
                ++received;
            }
        }
    });

    std::vector<std::thread> producers;
    producers.reserve(PRODS);
    for (int p = 0; p < PRODS; ++p) {
        producers.emplace_back([&, p]() {
            for (int s = 0; s < PER; ++s) while (!q.push(encode(p, s))) {}
            done.fetch_add(1, std::memory_order_release);
        });
    }
    for (auto& t : producers) t.join();
    consumer.join();

    ASSERT(received == PRODS * PER, "mpsc_total_count");
    ASSERT(fifo_ok,                 "mpsc_per_producer_fifo");
    ASSERT(no_dup,                  "mpsc_no_duplicates");
    ASSERT(q.empty(),               "mpsc_drained");

    printf("  MPSC: 4 assertions (%d producers × %d msgs)\n", PRODS, PER);
}


// =====================================================
// MPMC Queue — 4 producers × 25k → 4 consumers, no-loss + no-dup
// =====================================================

void test_mpmc_queue() {
    SECTION("MPMC Queue (4×4)");
    lockfree::MPMCQueue<uint64_t, 4096> q;

    constexpr int PRODS = 4, CONS = 4, PER = 25'000;
    auto encode = [](int pid, int seq) {
        return (static_cast<uint64_t>(pid) << 32) | static_cast<uint32_t>(seq);
    };

    std::atomic<int> done{0};
    std::vector<std::vector<uint64_t>> received(CONS);

    std::vector<std::thread> consumers;
    consumers.reserve(CONS);
    for (int c = 0; c < CONS; ++c) {
        consumers.emplace_back([&, c]() {
            received[c].reserve(static_cast<std::size_t>(PRODS) * PER / CONS * 2);
            uint64_t v;
            while (done.load(std::memory_order_acquire) < PRODS || !q.empty()) {
                if (q.pop(v)) received[c].push_back(v);
            }
        });
    }
    std::vector<std::thread> producers;
    producers.reserve(PRODS);
    for (int p = 0; p < PRODS; ++p) {
        producers.emplace_back([&, p]() {
            for (int s = 0; s < PER; ++s) while (!q.push(encode(p, s))) {}
            done.fetch_add(1, std::memory_order_release);
        });
    }
    for (auto& t : producers) t.join();
    for (auto& t : consumers) t.join();

    std::vector<uint64_t> all;
    for (auto& r : received) all.insert(all.end(), r.begin(), r.end());
    std::sort(all.begin(), all.end());
    const bool no_dup = std::adjacent_find(all.begin(), all.end()) == all.end();

    ASSERT((int)all.size() == PRODS * PER, "mpmc_total_count");
    ASSERT(no_dup,                          "mpmc_no_duplicates");
    ASSERT(all.front() == encode(0, 0),     "mpmc_first_present");
    ASSERT(all.back()  == encode(PRODS - 1, PER - 1), "mpmc_last_present");

    printf("  MPMC: 4 assertions (%d×%d × %d msgs)\n", PRODS, CONS, PER);
}


// =====================================================
// Sequencer — 1 producer → 3 consumers, fan-out, in-order reads
// =====================================================

void test_sequencer() {
    SECTION("Sequencer (1P × 3C fan-out)");
    lockfree::Sequencer<int64_t, 1024> s;
    constexpr int N = 20'000, CONS = 3;

    std::vector<std::atomic<int64_t>> cseq(CONS);
    for (auto& a : cseq) a.store(-1, std::memory_order_relaxed);
    std::atomic<bool> producer_done{false};
    std::atomic<bool> in_order{true};

    auto min_cseq = [&]() {
        int64_t mn = cseq[0].load(std::memory_order_acquire);
        for (int c = 1; c < CONS; ++c) {
            const int64_t v = cseq[c].load(std::memory_order_acquire);
            if (v < mn) mn = v;
        }
        return mn;
    };

    std::vector<std::thread> consumers;
    consumers.reserve(CONS);
    for (int c = 0; c < CONS; ++c) {
        consumers.emplace_back([&, c]() {
            int64_t next = 0;
            for (;;) {
                const int64_t hi = s.available();
                while (next <= hi) {
                    if (s.read(next) != next * 7) in_order.store(false);
                    cseq[c].store(next, std::memory_order_release);
                    ++next;
                }
                if (producer_done.load(std::memory_order_acquire) && next > s.available())
                    break;
            }
        });
    }
    std::thread gater([&]() {
        while (!producer_done.load(std::memory_order_acquire) || min_cseq() < N - 1) {
            const int64_t mn = min_cseq();
            if (mn >= 0) s.mark_consumed(mn);
        }
    });
    std::thread producer([&]() {
        for (int i = 0; i < N; ++i) {
            int64_t seq;
            while ((seq = s.try_claim()) == -1) {
                const int64_t mn = min_cseq();
                if (mn >= 0) s.mark_consumed(mn);
            }
            s.slot(seq) = seq * 7;
            s.publish(seq);
        }
        producer_done.store(true, std::memory_order_release);
    });

    producer.join();
    for (auto& t : consumers) t.join();
    gater.join();

    ASSERT(in_order.load(), "seq_each_consumer_in_order");
    for (int c = 0; c < CONS; ++c)
        ASSERT(cseq[c].load() == N - 1, "seq_consumer_finished");

    printf("  Sequencer: 4 assertions (%d events × %d consumers)\n", N, CONS);
}


// =====================================================
// WaitableMPSC — producer wakes parked consumer via pop_wait(timeout)
// =====================================================

void test_waitable_mpsc() {
    SECTION("Waitable MPSC (blocking pop)");
    using namespace std::chrono_literals;

    // Fast path: push then pop_wait returns immediately.
    {
        lockfree::WaitableMPSCQueue<int, 8> q;
        ASSERT(q.push(42), "wmpsc_push");
        int v = 0;
        const auto start = std::chrono::steady_clock::now();
        ASSERT(q.pop_wait(v, 100ms), "wmpsc_fast_pop");
        ASSERT(v == 42, "wmpsc_fast_value");
        ASSERT(std::chrono::steady_clock::now() - start < 50ms, "wmpsc_fast_no_block");
    }

    // Wakeup: parked consumer wakes within deadline when producer pushes.
    {
        lockfree::WaitableMPSCQueue<int, 8> q;
        std::atomic<bool> got{false};
        std::atomic<int>  got_value{0};
        std::thread consumer([&]() {
            int v;
            if (q.pop_wait(v, 500ms)) {
                got_value.store(v, std::memory_order_relaxed);
                got.store(true, std::memory_order_release);
            }
        });
        std::this_thread::sleep_for(10ms);
        q.push(7);
        consumer.join();
        ASSERT(got.load(),           "wmpsc_wakeup");
        ASSERT(got_value.load() == 7,"wmpsc_wakeup_value");
    }

    // Timeout: pop_wait on empty queue returns false after the deadline.
    {
        lockfree::WaitableMPSCQueue<int, 8> q;
        int v = 0;
        const auto start = std::chrono::steady_clock::now();
        const bool got = q.pop_wait(v, 20ms);
        const auto dt = std::chrono::steady_clock::now() - start;
        ASSERT(!got,           "wmpsc_timeout_returns_false");
        ASSERT(dt >= 15ms,     "wmpsc_actually_waited");
    }

    printf("  WaitableMPSC: 8 assertions\n");
}


// =====================================================
// VarlenRingBuffer — variable-length messages with length prefix
// =====================================================

void test_varlen_ring() {
    SECTION("VarlenRingBuffer (variable-size messages)");

    lockfree::VarlenRingBuffer<1024> ring;

    // Empty fresh
    ASSERT(ring.empty(), "varlen_fresh_empty");

    // Write three messages of different sizes
    const char* m1 = "hello";              // 5 bytes
    const char* m2 = "world!";             // 6 bytes
    const char* m3 = "third message here"; // 18 bytes
    ASSERT(ring.write(m1, 5),  "varlen_write_1");
    ASSERT(ring.write(m2, 6),  "varlen_write_2");
    ASSERT(ring.write(m3, 18), "varlen_write_3");
    ASSERT(!ring.empty(),      "varlen_not_empty_after_writes");

    // Read back in order
    char buf[64] = {};
    const std::uint32_t BUF_CAP = static_cast<std::uint32_t>(sizeof(buf));
    std::uint32_t n;

    n = ring.read(buf, BUF_CAP);
    ASSERT(n == 5,                       "varlen_read_1_len");
    ASSERT(std::memcmp(buf, m1, 5) == 0, "varlen_read_1_content");

    n = ring.read(buf, BUF_CAP);
    ASSERT(n == 6,                       "varlen_read_2_len");
    ASSERT(std::memcmp(buf, m2, 6) == 0, "varlen_read_2_content");

    n = ring.read(buf, BUF_CAP);
    ASSERT(n == 18u,                      "varlen_read_3_len");
    ASSERT(std::memcmp(buf, m3, 18) == 0, "varlen_read_3_content");

    ASSERT(ring.empty(), "varlen_empty_after_drain");
    ASSERT(ring.read(buf, BUF_CAP) == 0u, "varlen_read_empty_returns_0");

    // Reject zero-len and over-capacity
    ASSERT(!ring.write(m1, 0u),    "varlen_reject_zero_len");
    ASSERT(!ring.write(m1, 2048u), "varlen_reject_too_big");

    // Buffer-too-small on read returns 0 without consuming
    ASSERT(ring.write(m3, 18u),  "varlen_write_for_short_read_test");
    char small[4];
    const std::uint32_t SMALL_CAP = static_cast<std::uint32_t>(sizeof(small));
    ASSERT(ring.read(small, SMALL_CAP) == 0u, "varlen_short_read_no_consume");
    ASSERT(ring.read(buf,   BUF_CAP)   == 18u,"varlen_subsequent_read_ok");
}


// =====================================================
// Logger async flush — exercise start/stop and binary file output
// =====================================================

void test_logger_async_flush() {
    SECTION("Logger async flush");
    const char* path = "/tmp/hft_logger_async_test.bin";
    std::remove(path);  // ensure clean slate

    {
        TradeLogger logger;
        ASSERT(logger.start_async_flush(path), "async_started");
        for (int i = 0; i < 50; ++i)
            logger.log(EventType::ORDER_SUBMIT, i + 1, "AAPL", "BUY", 100, 150.25);
        logger.stop_async_flush();  // blocks until drained
    }

    FILE* f = std::fopen(path, "rb");
    ASSERT(f != nullptr, "async_file_exists");
    if (f) {
        std::fseek(f, 0, SEEK_END);
        long size = std::ftell(f);
        std::fclose(f);
        // header (64) + 50 events * 128 = 64 + 6400 = 6464
        ASSERT(size == 64 + 50 * 128, "async_file_correct_size");
    }
    std::remove(path);
}


// =====================================================
// LockfreeTradeLogger — same audit semantics, SPSCQueue under the hood
// =====================================================

void test_lockfree_logger() {
    SECTION("LockfreeTradeLogger (SPSCQueue-backed)");
    const char* path = "/tmp/hft_lockfree_logger_test.bin";
    std::remove(path);

    // Small CAPACITY (must be power of 2) to keep the binary lean; still
    // 100x larger than the 50 events we push.
    {
        lockfree_logger::LockfreeTradeLogger</*CAPACITY=*/8192> logger;
        ASSERT(logger.start_async_flush(path), "lf_logger_started");
        for (int i = 0; i < 50; ++i) {
            const std::uint64_t seq =
                logger.log(EventType::ORDER_SUBMIT, i + 1, "AAPL", "BUY", 100, 150.25);
            ASSERT(seq == static_cast<std::uint64_t>(i + 1), "lf_logger_seq_returned");
        }
        ASSERT(logger.total_logged() == 50, "lf_logger_count");
        ASSERT(logger.get_counter(EventType::ORDER_SUBMIT) == 50, "lf_logger_per_type");
        logger.stop_async_flush();
    }

    // File size = header(64) + 50 * sizeof(TradeEvent)(128) = 6464 bytes.
    FILE* f = std::fopen(path, "rb");
    ASSERT(f != nullptr, "lf_logger_file_exists");
    if (f) {
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        std::fclose(f);
        ASSERT(size == 64 + 50 * 128, "lf_logger_file_correct_size");
    }
    std::remove(path);
}


// =====================================================
// MmapTradeLogger — mmap-backed audit ring, no async flush thread
// =====================================================

void test_mmap_logger() {
    SECTION("MmapTradeLogger");
    const char* path = "/tmp/hft_mmap_logger_test.bin";
    std::remove(path);

    {
        mmap_logger::MmapTradeLogger lg;
        ASSERT(lg.open_file(path, /*capacity=*/100), "mmap_open");
        ASSERT(lg.is_open(),                          "mmap_is_open");
        for (int i = 0; i < 50; ++i) {
            const std::uint64_t seq =
                lg.log(EventType::ORDER_SUBMIT, i + 1, "AAPL", "BUY", 100, 150.25);
            ASSERT(seq == static_cast<std::uint64_t>(i + 1), "mmap_seq_returned");
        }
        ASSERT(lg.total_logged() == 50, "mmap_count");
        ASSERT(lg.get_counter(EventType::ORDER_SUBMIT) == 50, "mmap_per_type_counter");
        ASSERT(lg.flush_sync(), "mmap_flush_sync_ok");
        // close() happens in destructor
    }

    // File size = header(64) + capacity(100) * sizeof(TradeEvent)(128) = 12864
    FILE* f = std::fopen(path, "rb");
    ASSERT(f != nullptr, "mmap_file_exists");
    if (f) {
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        std::fclose(f);
        ASSERT(size == 64 + 100 * 128, "mmap_file_correct_size");
    }

    // Capacity-full path: log() returns 0 once we hit capacity.
    {
        mmap_logger::MmapTradeLogger small;
        ASSERT(small.open_file("/tmp/hft_mmap_small.bin", /*capacity=*/3), "mmap_small_open");
        ASSERT(small.log(EventType::ORDER_SUBMIT, 1) == 1, "mmap_small_1");
        ASSERT(small.log(EventType::ORDER_SUBMIT, 2) == 2, "mmap_small_2");
        ASSERT(small.log(EventType::ORDER_SUBMIT, 3) == 3, "mmap_small_3");
        ASSERT(small.log(EventType::ORDER_SUBMIT, 4) == 0, "mmap_full_returns_0");
    }
    std::remove(path);
    std::remove("/tmp/hft_mmap_small.bin");
}


// =====================================================
// Risk rate limiter — bursts above max_orders_per_second get rejected
// =====================================================

void test_risk_rate_limiter() {
    SECTION("Risk rate limiter");
    RiskLimits l;
    l.max_orders_per_second = 5;
    RiskManager rm(l);

    for (int i = 0; i < 5; ++i) {
        auto r = rm.check_order("AAPL", Side::BUY, 1.0, 1);
        ASSERT(r.action == RiskAction::ALLOW, "rate_within_burst");
    }
    auto over = rm.check_order("AAPL", Side::BUY, 1.0, 1);
    ASSERT(over.action == RiskAction::REJECT, "rate_burst_rejected");
    ASSERT(strstr(over.reason, "Rate") != nullptr, "rate_reason_correct");
}


// =====================================================
// Risk drawdown with peak=0 — must skip the check, never reject
// =====================================================

void test_risk_drawdown_no_peak() {
    SECTION("Risk drawdown peak=0");
    RiskLimits l;
    l.max_drawdown_pct = 5.0;
    RiskManager rm(l);

    // No P&L history yet — drawdown check skips when peak <= 0
    auto r1 = rm.check_order("AAPL", Side::BUY, 1.0, 1);
    ASSERT(r1.action == RiskAction::ALLOW, "drawdown_zero_peak_allow");

    // Negative P&L without ever hitting a peak — drawdown still skips
    rm.update_pnl(-100.0);
    auto r2 = rm.check_order("AAPL", Side::BUY, 1.0, 1);
    ASSERT(r2.action == RiskAction::ALLOW, "drawdown_no_peak_no_kill");
    ASSERT(!rm.is_kill_switch_active(), "drawdown_no_peak_not_killed");
}


// =====================================================
// FIX + OUCH + ITCH interleaved — three protocols on one wire-of-life
// =====================================================

void test_protocols_interleaved() {
    SECTION("FIX + OUCH + ITCH interleaved");

    FIXMessage fix;
    ITCHParser itch;
    uint8_t buf41[41] = {'A'};   // OUCH Accepted
    uint8_t buf31[31] = {'E'};   // OUCH Executed
    uint8_t itch_a[34] = {'A'};  // ITCH Add Order
    uint8_t itch_d[17] = {'D'};  // ITCH Delete Order

    // Round 1
    fix.parse("8=FIX.4.2|35=D|55=AAPL|54=1|44=150.25|38=100");
    ASSERT(fix.field_count() > 0,                                   "ileave_fix_1");
    ASSERT(strcmp(OUCHMessage::parse_response(buf41, 41).type,
                  "ACCEPTED") == 0,                                 "ileave_ouch_1");
    ASSERT(itch.parse(itch_a, 34).type == MsgType::ADD_ORDER,       "ileave_itch_1");

    // Round 2 — different messages, same parsers
    fix.parse("8=FIX.4.2|35=8|55=TSLA|54=2");
    ASSERT(fix.field_count() == 4,                                  "ileave_fix_2");
    ASSERT(strcmp(OUCHMessage::parse_response(buf31, 31).type,
                  "EXECUTED") == 0,                                 "ileave_ouch_2");
    ASSERT(itch.parse(itch_d, 17).type == MsgType::DELETE_ORDER,    "ileave_itch_2");
}


// =====================================================
// LOBSTER replay — parse the bundled sample CSV
// =====================================================

void test_lobster_reader() {
    SECTION("LOBSTER reader (sample CSV)");

    lobster::LobsterReader r("replay/sample_aapl.csv");
    ASSERT(r.is_open(), "lobster_file_opens");
    if (!r.is_open()) return;

    int submits = 0, executes = 0, cancels = 0, deletes = 0;
    lobster::LobsterMessage m;
    while (r.next(m)) {
        switch (m.event_type) {
            case lobster::EventType::SUBMIT:           ++submits;  break;
            case lobster::EventType::EXECUTE_VISIBLE:  ++executes; break;
            case lobster::EventType::CANCEL_PARTIAL:   ++cancels;  break;
            case lobster::EventType::DELETE:           ++deletes;  break;
            default: break;
        }
    }
    ASSERT(r.rows_bad() == 0,         "lobster_no_malformed_rows");
    ASSERT(r.rows_read() == 20,       "lobster_all_rows_parsed");
    ASSERT(submits  > 0,              "lobster_has_submits");
    ASSERT(executes > 0,              "lobster_has_executes");
    ASSERT(cancels + deletes > 0,     "lobster_has_cancels_or_deletes");
}


void test_simulator() {
    SECTION("Market Simulator");

    // Test generator produces valid messages
    MarketDataGenerator gen(42);
    ITCHParser parser;

    auto msg = gen.generate_add_order();
    ASSERT(msg.length > 0, "sim_gen_add_order");
    auto pm = parser.parse(msg.data, msg.length);
    ASSERT(pm.type == MsgType::ADD_ORDER, "sim_parse_add_order");

    msg = gen.generate_trade();
    pm = parser.parse(msg.data, msg.length);
    ASSERT(pm.type == MsgType::TRADE, "sim_parse_trade");

    msg = gen.generate_system_event('O');
    pm = parser.parse(msg.data, msg.length);
    ASSERT(pm.type == MsgType::SYSTEM_EVENT, "sim_parse_system");

    // Test RNG determinism
    FastRNG rng1(999), rng2(999);
    bool match = true;
    for (int i = 0; i < 1000; ++i) {
        if (rng1.next() != rng2.next()) { match = false; break; }
    }
    ASSERT(match, "sim_rng_deterministic");

    // Test full pipeline — direct mode
    PipelineStats stats = run_pipeline(200, false, false, 42);
    ASSERT(stats.messages_generated > 200, "sim_pipeline_generated");
    ASSERT(stats.messages_parsed == stats.messages_generated, "sim_pipeline_parsed_all");
    ASSERT(stats.orders_submitted > 0, "sim_pipeline_submitted");
    ASSERT(stats.orders_filled == stats.orders_submitted, "sim_pipeline_filled");
    ASSERT(stats.total_ms > 0, "sim_pipeline_time");

    // Test pipeline — with strategy
    stats = run_pipeline(500, true, false, 42);
    ASSERT(stats.messages_parsed > 0, "sim_strategy_parsed");

    // Test pipeline — with router
    stats = run_pipeline(200, false, true, 42);
    ASSERT(stats.orders_submitted > 0, "sim_router_submitted");

    // Test pipeline — full (strategy + router)
    stats = run_pipeline(500, true, true, 42);
    ASSERT(stats.messages_parsed > 0, "sim_full_pipeline");

    // Test pipeline with realistic fill latency — orders queue then drain
    stats = run_pipeline(200, false, false, 42, nullptr, /*fill_latency_iters=*/5);
    ASSERT(stats.orders_submitted > 0, "sim_latency_submitted");
    ASSERT(stats.orders_filled == stats.orders_submitted, "sim_latency_all_drained");
    ASSERT(stats.max_in_flight_orders > 0, "sim_latency_in_flight_peak");

    // Async pipeline — producer + consumer split across threads via SPSCQueue
    PipelineStats async_stats = run_pipeline_threaded(5'000, 42);
    ASSERT(async_stats.messages_generated == 5'000, "sim_threaded_count_gen");
    ASSERT(async_stats.messages_parsed    == 5'000, "sim_threaded_count_parsed");
    ASSERT(async_stats.add_orders + async_stats.executes +
           async_stats.trades + async_stats.cancels > 0,
           "sim_threaded_has_data_messages");

    printf("  Simulator: %d assertions\n", 19);
}


// =====================================================
// Cross-Module Integration Test
// =====================================================

void test_integration() {
    SECTION("Cross-Module Integration");

    // Full pipeline: generate ITCH → parse → risk check → route → OMS → log
    MarketDataGenerator gen(123);
    ITCHParser parser;
    RiskLimits rlimits;
    rlimits.max_position_per_symbol = 1000;
    rlimits.max_order_value = 500000;
    RiskManager risk(rlimits);
    SmartOrderRouter router(RoutingStrategy::BEST_PRICE, 500);
    OMS oms(1000, 500000.0);
    TradeLogger logger;

    Venue nyse;
    strncpy(nyse.name, "NYSE", 15);
    nyse.latency_ns = 500;
    nyse.fee_per_share = 0.003;
    router.add_venue(nyse);
    router.update_quote("NYSE", 150.00, 150.10, 500, 500);

    logger.log(EventType::SYSTEM_START);

    int submitted = 0, rejected = 0, filled = 0;

    for (int i = 0; i < 50; ++i) {
        auto raw = gen.generate_add_order();
        auto pm = parser.parse(raw.data, raw.length);

        if (pm.type != MsgType::ADD_ORDER) continue;

        const char* side_str = pm.data.add_order.side == 'B' ? "BUY" : "SELL";
        double price = pm.data.add_order.price;
        int32_t shares = static_cast<int32_t>(pm.data.add_order.shares);
        const char* stock = pm.data.add_order.stock;

        // Log submit
        logger.log(EventType::ORDER_SUBMIT, i + 1, stock, side_str, shares, price);

        // Risk check
        Side side = (pm.data.add_order.side == 'B') ? Side::BUY : Side::SELL;
        if (risk.check_order(stock, side_from_str(side_str), price, shares).action != RiskAction::ALLOW) {
            logger.log(EventType::RISK_REJECT, i + 1, stock, side_str, shares, price);
            rejected++;
            continue;
        }
        logger.log(EventType::RISK_ACCEPT, i + 1, stock);

        // Route
        RouteDecision rd = router.route_order(side_str, shares, RoutingStrategy::BEST_PRICE);
        double fill_price = rd.valid ? rd.price : price;

        // Submit to OMS
        Order* order = oms.submit_order(stock, side, fill_price, shares);
        if (!order) {
            rejected++;
            continue;
        }
        submitted++;

        // Fill
        oms.fill_order(order->order_id, shares, fill_price);
        logger.log(EventType::ORDER_FILL, i + 1, stock, side_str, shares, fill_price);
        filled++;
    }

    logger.log(EventType::SYSTEM_STOP);

    ASSERT(logger.total_events() > 10, "integ_logged_events");
    ASSERT(submitted > 0, "integ_submitted");
    ASSERT(filled == submitted, "integ_all_filled");
    ASSERT(submitted + rejected > 0, "integ_orders_attempted");
    ASSERT(logger.get_counter(EventType::SYSTEM_START) == 1, "integ_system_start");
    ASSERT(logger.get_counter(EventType::SYSTEM_STOP) == 1, "integ_system_stop");
    ASSERT(logger.get_counter(EventType::ORDER_SUBMIT) > 0, "integ_submits_logged");
    ASSERT(logger.get_counter(EventType::ORDER_FILL) > 0, "integ_fills_logged");
    ASSERT(oms.order_count() > 0u, "integ_oms_orders");
    ASSERT(oms.position_count() > 0u, "integ_positions");

    printf("  Integration: %d assertions\n", 10);
}


// ITCH→Book #82 — rekonstrukcja ksiegi L3 ze strumienia ITCH.
void test_itch_book() {
    SECTION("ITCH Book Reconstructor (#82)");
    auto close = [](double a, double b) { const double d = a - b; return (d < 0 ? -d : d) < 1e-6; };
    itch::ITCHOrderBook book;

    book.on_add(1, 'B', 150.00, 100);
    book.on_add(2, 'B', 149.99, 200);
    book.on_add(3, 'S', 150.05, 150);
    book.on_add(4, 'S', 150.10, 300);
    ASSERT(close(book.best_bid(), 150.00), "itchbook_best_bid");
    ASSERT(close(book.best_ask(), 150.05), "itchbook_best_ask");
    ASSERT(book.resting_orders() == 4, "itchbook_resting_4");
    ASSERT(book.total_bid_qty() == 300, "itchbook_total_bid_qty");

    book.on_execute(1, 60);                       // 150.00: 100 → 40
    ASSERT(book.qty_at('B', 150.00) == 40, "itchbook_execute_reduces");
    book.on_cancel(2, 40);                        // 149.99: 200 → 160
    ASSERT(book.qty_at('B', 149.99) == 160, "itchbook_cancel_reduces");
    book.on_delete(3);                            // ask 150.05 znika
    ASSERT(close(book.best_ask(), 150.10), "itchbook_delete_lifts_ask");

    book.on_replace(1, 5, 150.01, 80);            // order1(40) → ref5 @150.01 x80
    ASSERT(book.qty_at('B', 150.00) == 0, "itchbook_replace_clears_old");
    ASSERT(close(book.best_bid(), 150.01), "itchbook_replace_new_best_bid");

    book.on_execute(999, 10);                     // unknown ref → orphan
    ASSERT(book.orphans() == 1, "itchbook_orphan_counted");

    // #350 cancel_to_add_ratio: 4 adds, 1 cancel above -> 0.25.
    ASSERT(book.adds() == 4 && book.cancels() == 1, "itchbook_adds_cancels_counted");
    ASSERT(close(book.cancel_to_add_ratio(), 0.25), "itchbook_cancel_add_ratio");
    itch::ITCHOrderBook fresh_ctr;
    ASSERT(fresh_ctr.cancel_to_add_ratio() == 0.0, "itchbook_cancel_add_ratio_no_adds");

    // #358 execute_to_add_ratio: 4 adds, 2 executes (on_execute(1,60) + orphan
    // on_execute(999,10) above, which counts regardless) -> 0.5.
    ASSERT(book.executes() == 2, "itchbook_executes_counted");
    ASSERT(close(book.execute_to_add_ratio(), 0.5), "itchbook_execute_add_ratio");
    ASSERT(fresh_ctr.execute_to_add_ratio() == 0.0, "itchbook_execute_add_ratio_no_adds");

    // #87 microstructure: mid + top-of-book imbalance.
    itch::ITCHOrderBook mb;
    mb.on_add(10, 'B', 100.00, 300);              // best bid 300
    mb.on_add(11, 'S', 100.02, 100);              // best ask 100
    ASSERT(close(mb.mid_price(), 100.01), "itchbook_mid_price");
    ASSERT(mb.best_bid_qty() == 300 && mb.best_ask_qty() == 100, "itchbook_tob_qty");
    ASSERT(close(mb.imbalance(), 0.5), "itchbook_imbalance");   // (300-100)/400
    // microprice: (100.02*300 + 100.00*100)/400 = 100.015 (>mid 100.01, bid-heavy)
    ASSERT(close(mb.microprice(), 100.015), "itchbook_microprice");
    // #239 microprice_skew = microprice - mid = 0.005 (>0: presja wzrostowa, bid-heavy)
    ASSERT(close(mb.microprice_skew(), 0.005), "itchbook_microprice_skew");
    itch::ITCHOrderBook bal;
    bal.on_add(1, 'B', 100.00, 100); bal.on_add(2, 'S', 100.02, 100);   // zbalansowany
    ASSERT(close(bal.microprice_skew(), 0.0), "itchbook_microprice_skew_balanced");
    // #254 imbalance_signal (mb: imbalance 0.5 bid-heavy; bal: 0.0)
    ASSERT(mb.imbalance_signal(0.3) == 1, "itchbook_imb_signal_up");      // 0.5 > 0.3
    ASSERT(mb.imbalance_signal(0.6) == 0, "itchbook_imb_signal_neutral"); // 0.5 < 0.6
    ASSERT(bal.imbalance_signal(0.1) == 0, "itchbook_imb_signal_balanced");
    itch::ITCHOrderBook ah;
    ah.on_add(1, 'B', 100.00, 100); ah.on_add(2, 'S', 100.02, 300);     // ask-heavy -0.5
    ASSERT(ah.imbalance_signal(0.3) == -1, "itchbook_imb_signal_down");

    // #95 pre-trade impact: walk the book → VWAP of a market order.
    itch::ITCHOrderBook fb;
    fb.on_add(1, 'S', 100.00, 100);
    fb.on_add(2, 'S', 100.01, 200);
    fb.on_add(3, 'S', 100.02, 300);
    double v = 0.0;
    int64_t f = fb.expected_fill('B', 250, v);                  // 100@100.00 + 150@100.01
    ASSERT(f == 250, "itchbook_fill_qty_250");
    ASSERT(close(v, 100.006), "itchbook_fill_vwap");            // (100*100.00+150*100.01)/250
    int64_t f2 = fb.expected_fill('B', 1000, v);                // only 600 liquidity
    ASSERT(f2 == 600, "itchbook_fill_partial_600");

    // #123 top_levels: top N levels per side.
    itch::ITCHOrderBook tb;
    tb.on_add(1, 'B', 100.00, 100);
    tb.on_add(2, 'B',  99.99, 200);
    tb.on_add(3, 'B',  99.98, 300);
    double px[2]; int64_t q[2];
    const int nl = tb.top_levels('B', 2, px, q);
    ASSERT(nl == 2, "itchbook_top_levels_count");
    ASSERT(close(px[0], 100.00) && q[0] == 100, "itchbook_top_lvl0_best");
    ASSERT(close(px[1],  99.99) && q[1] == 200, "itchbook_top_lvl1_next");

    // #131 spread_bps + clear
    itch::ITCHOrderBook sb;
    sb.on_add(1, 'B', 100.00, 100);
    sb.on_add(2, 'S', 100.02, 100);                       // mid 100.01, spread 0.02
    ASSERT(close(sb.spread_bps(), 1.99980), "itchbook_spread_bps");

    // #148 wielopoziomowa nierownowaga (depth_imbalance)
    itch::ITCHOrderBook di;
    di.on_add(1, 'B', 100.00, 100); di.on_add(2, 'B', 99.99, 200);  // bids 100,200
    di.on_add(3, 'S', 100.02, 50);  di.on_add(4, 'S', 100.03, 50);  // asks 50,50
    ASSERT(close(di.depth_imbalance(1), 1.0/3.0), "itchbook_depth_imb_1");  // (100-50)/150
    ASSERT(close(di.depth_imbalance(2), 0.5), "itchbook_depth_imb_2");      // (300-100)/400

    // #155 vwap_depth: VWAP of the top-N levels.
    itch::ITCHOrderBook vd;
    vd.on_add(1, 'S', 100.00, 100);
    vd.on_add(2, 'S', 100.02, 300);
    // (100.00*100 + 100.02*300)/400 = 100.015
    ASSERT(close(vd.vwap_depth('S', 2), 100.015), "itchbook_vwap_depth");
    ASSERT(close(vd.vwap_depth('S', 1), 100.00), "itchbook_vwap_depth_top1");

    // #285 depth_weighted_mid (averages both sides' depth-VWAP).
    itch::ITCHOrderBook dm;
    dm.on_add(1, 'B', 100.00, 100); dm.on_add(2, 'B', 99.98, 200);   // bids
    dm.on_add(3, 'S', 100.02, 100); dm.on_add(4, 'S', 100.04, 200);  // asks
    // vwap_bid(2)=29996/300=99.98667, vwap_ask(2)=30010/300=100.03333 -> mid 100.01
    ASSERT(close(dm.depth_weighted_mid(2), 100.01), "itchbook_dwm_depth2");
    // n=1: touch only -> (100.00 + 100.02)/2 = 100.01
    ASSERT(close(dm.depth_weighted_mid(1), 100.01), "itchbook_dwm_touch");
    itch::ITCHOrderBook dmo;
    dmo.on_add(1, 'B', 100.00, 100);                                 // one-sided
    ASSERT(dmo.depth_weighted_mid(1) == 0.0, "itchbook_dwm_onesided");

    // #164 liquidity_within (N ticks from best).
    itch::ITCHOrderBook lw;
    lw.on_add(1, 'S', 100.00, 100); lw.on_add(2, 'S', 100.01, 200); lw.on_add(3, 'S', 100.05, 300);
    ASSERT(lw.liquidity_within('S', 1) == 300, "itchbook_liq_within_1");  // 10000+10001
    ASSERT(lw.liquidity_within('S', 5) == 600, "itchbook_liq_within_5");  // + 10005

    // #269 liquidity_imbalance_within (ticks-based imbalance).
    itch::ITCHOrderBook li;
    li.on_add(1, 'B', 100.00, 300); li.on_add(2, 'B', 99.99, 100);   // bids
    li.on_add(3, 'S', 100.02, 100); li.on_add(4, 'S', 100.03, 200);  // asks
    // within 0 ticks (touch): bid 300, ask 100 -> (300-100)/400 = 0.5
    ASSERT(close(li.liquidity_imbalance_within(0), 0.5), "itchbook_liqimb_touch");
    // within 1 tick: bid 400, ask 300 -> (400-300)/700 = 0.142857
    ASSERT(close(li.liquidity_imbalance_within(1), 100.0/700.0), "itchbook_liqimb_1tick");

    // #174 total_shares + level_count (book size and depth).
    // #223 fillable_shares (up to the limit price).
    // lw aski: 100.00(100), 100.01(200), 100.05(300)
    ASSERT(lw.fillable_shares('B', 100.01) == 300, "itchbook_fillable_buy_mid");  // <=100.01
    ASSERT(lw.fillable_shares('B', 100.05) == 600, "itchbook_fillable_buy_all");
    ASSERT(lw.fillable_shares('B', 99.99) == 0, "itchbook_fillable_buy_none");    // < best ask
    itch::ITCHOrderBook fs;                                                       // separate book for bids
    fs.on_add(10, 'B', 99.99, 150); fs.on_add(11, 'B', 99.98, 250);
    ASSERT(fs.fillable_shares('S', 99.98) == 400, "itchbook_fillable_sell_both"); // >=99.98
    ASSERT(fs.fillable_shares('S', 99.99) == 150, "itchbook_fillable_sell_top");  // >=99.99

    // #247 price_to_fill (the worst level to sweep).
    itch::ITCHOrderBook pf;
    pf.on_add(1, 'S', 100.00, 100); pf.on_add(2, 'S', 100.02, 200); pf.on_add(3, 'S', 100.05, 300);
    ASSERT(close(pf.price_to_fill('B', 100), 100.00), "itchbook_ptf_buy_top");   // 100 @ best
    ASSERT(close(pf.price_to_fill('B', 250), 100.02), "itchbook_ptf_buy_mid");   // 100+150 -> 2nd level
    ASSERT(pf.price_to_fill('B', 700) == 0.0, "itchbook_ptf_insufficient");      // > 600 available
    pf.on_add(4, 'B', 99.99, 150); pf.on_add(5, 'B', 99.98, 250);
    ASSERT(close(pf.price_to_fill('S', 200), 99.98), "itchbook_ptf_sell");       // 150+50 -> 99.98

    // #342 levels_to_fill (how many price levels a sweep must touch).
    // Same pf book: asks 100.00(100), 100.02(200), 100.05(300); bids 99.99(150), 99.98(250).
    ASSERT(pf.levels_to_fill('B', 0)   == 0,  "itchbook_ltf_zero_shares");
    ASSERT(pf.levels_to_fill('B', 100) == 1,  "itchbook_ltf_buy_top_only");   // 100 <= level 1
    ASSERT(pf.levels_to_fill('B', 250) == 2,  "itchbook_ltf_buy_two_levels"); // 100+150 -> level 2
    ASSERT(pf.levels_to_fill('B', 700) == -1, "itchbook_ltf_buy_insufficient"); // > 600 available
    ASSERT(pf.levels_to_fill('S', 200) == 2,  "itchbook_ltf_sell_two_levels"); // 150+50 -> level 2
    ASSERT(pf.levels_to_fill('S', 500) == -1, "itchbook_ltf_sell_insufficient"); // > 400 available

    // #366 spread_at_size — round-trip spread paid to sweep `shares` both sides.
    // pf: asks 100.00(100),100.02(200); bids 99.99(150),99.98(250).
    // size 100: buy@100.00, sell@99.99 -> 0.01 (equals the touch spread).
    ASSERT(close(pf.spread_at_size(100), 0.01), "itchbook_spread_at_size_touch");
    // size 200: buy@100.02 (100+200), sell@99.98 (150+250) -> 0.04 (widened by depth).
    ASSERT(close(pf.spread_at_size(200), 0.04), "itchbook_spread_at_size_deep");
    // size 700 > 600 ask depth: buy side can't fill -> 0.
    ASSERT(pf.spread_at_size(700) == 0.0, "itchbook_spread_at_size_insufficient");

    ASSERT(lw.total_shares('S') == 600, "itchbook_total_shares_ask");     // 100+200+300
    ASSERT(lw.level_count('S') == 3, "itchbook_level_count_ask");
    ASSERT(lw.total_shares('B') == 0 && lw.level_count('B') == 0, "itchbook_empty_bid_side");

    // #374 resting_order_count / avg_resting_order_size — per-side order granularity.
    // lw asks: 3 orders (100, 200, 300 shares) -> count 3, mean 200.
    ASSERT(lw.resting_order_count('S') == 3, "itchbook_roc_ask_3");
    ASSERT(close(lw.avg_resting_order_size('S'), 200.0), "itchbook_aros_ask_200");
    ASSERT(lw.resting_order_count('B') == 0 && lw.avg_resting_order_size('B') == 0.0,
           "itchbook_roc_empty_bid_zero");
    // partial execute shrinks the order but keeps it resting: 100 -> 40 on order 1.
    lw.on_execute(1, 60);
    ASSERT(lw.resting_order_count('S') == 3, "itchbook_roc_partial_keeps_order");
    ASSERT(close(lw.avg_resting_order_size('S'), 540.0 / 3.0), "itchbook_aros_after_partial");
    // full delete removes order 2 (200 shares): count 2, mean (40+300)/2 = 170.
    lw.on_delete(2);
    ASSERT(lw.resting_order_count('S') == 2, "itchbook_roc_after_delete");
    ASSERT(close(lw.avg_resting_order_size('S'), 170.0), "itchbook_aros_after_delete");

    // #191 total_notional ($ depth).
    itch::ITCHOrderBook tn;
    tn.on_add(1, 'S', 100.00, 100);   // 10000.00
    tn.on_add(2, 'S', 100.02, 200);   // 20004.00
    ASSERT(close(tn.total_notional('S'), 30004.0), "itchbook_total_notional_ask");
    ASSERT(tn.total_notional('B') == 0.0, "itchbook_total_notional_empty_bid");

    // #199 slippage_bps (execution cost vs mid).
    itch::ITCHOrderBook sl;
    sl.on_add(1, 'B', 99.98, 100);                          // best bid
    sl.on_add(2, 'S', 100.00, 100); sl.on_add(3, 'S', 100.02, 200);  // asks
    // mid = 99.99; BUY 100 -> vwap 100.00 -> (100.00-99.99)/99.99*10000 ~ 1.0001 bps
    ASSERT(close(sl.slippage_bps('B', 100), 1.00010001), "itchbook_slippage_buy_100");
    // a larger order eats a deeper level -> larger slippage
    ASSERT(sl.slippage_bps('B', 200) > sl.slippage_bps('B', 100), "itchbook_slippage_monotone");
    // SELL 50 hits bid 99.98 -> (99.99-99.98)/99.99*10000 ~ 1.0001 bps
    ASSERT(close(sl.slippage_bps('S', 50), 1.00010001), "itchbook_slippage_sell_50");
    itch::ITCHOrderBook nlq;                                // one-sided book -> 0
    nlq.on_add(1, 'B', 99.98, 100);
    ASSERT(nlq.slippage_bps('B', 100) == 0.0, "itchbook_slippage_no_ask_liq");

    // #231 round_trip_cost_bps (buy+sell).
    // sl: bid 99.98 / ask 100.00, mid 99.99; buy 100 slip ~1bps, sell 50 slip ~1bps
    ASSERT(close(sl.round_trip_cost_bps(100), 2.0 * (0.01/99.99*10000.0)),
           "itchbook_round_trip_cost");
    ASSERT(nlq.round_trip_cost_bps(100) == 0.0, "itchbook_round_trip_onesided");

    // #207 spread_ticks (total number of $0.01 ticks).
    itch::ITCHOrderBook st;
    st.on_add(1, 'B', 100.00, 100); st.on_add(2, 'S', 100.02, 100);  // 2 ticki
    ASSERT(st.spread_ticks() == 2, "itchbook_spread_ticks_2");
    itch::ITCHOrderBook st1;
    st1.on_add(1, 'B', 50.00, 100); st1.on_add(2, 'S', 50.01, 100);  // 1 tick (touch)
    ASSERT(st1.spread_ticks() == 1, "itchbook_spread_ticks_1");
    ASSERT(nlq.spread_ticks() == 0, "itchbook_spread_ticks_onesided");

    // #261 is_marketable (would a limit cross now?).
    itch::ITCHOrderBook im;
    im.on_add(1, 'B', 99.98, 100); im.on_add(2, 'S', 100.02, 100);   // bid 99.98 / ask 100.02
    ASSERT(im.is_marketable('B', 100.02), "itchbook_mkt_buy_at_ask");   // limit >= ask
    ASSERT(im.is_marketable('B', 100.05), "itchbook_mkt_buy_above");
    ASSERT(!im.is_marketable('B', 100.00), "itchbook_mkt_buy_below");   // < ask -> rests
    ASSERT(im.is_marketable('S', 99.98), "itchbook_mkt_sell_at_bid");   // limit <= bid
    ASSERT(!im.is_marketable('S', 99.99), "itchbook_mkt_sell_above");   // > bid -> rests
    ASSERT(!nlq.is_marketable('B', 100.0), "itchbook_mkt_no_ask");      // one-sided

    // #277 nth_level_price / nth_level_qty (random level access).
    itch::ITCHOrderBook nla;
    nla.on_add(1, 'S', 100.00, 100); nla.on_add(2, 'S', 100.02, 200); nla.on_add(3, 'S', 100.05, 300);
    nla.on_add(4, 'B', 99.99, 150);  nla.on_add(5, 'B', 99.98, 250);
    ASSERT(close(nla.nth_level_price('S', 0), 100.00), "itchbook_nth_ask0");   // best ask
    ASSERT(close(nla.nth_level_price('S', 2), 100.05), "itchbook_nth_ask2");
    ASSERT(nla.nth_level_qty('S', 1) == 200, "itchbook_nth_ask_qty");
    ASSERT(close(nla.nth_level_price('B', 0), 99.99), "itchbook_nth_bid0");    // best bid
    ASSERT(nla.nth_level_qty('B', 1) == 250, "itchbook_nth_bid_qty");
    ASSERT(nla.nth_level_price('S', 5) == 0.0 && nla.nth_level_qty('B', 9) == 0,
           "itchbook_nth_out_of_range");

    // #293 cumulative_qty (top-N displayed size across levels).
    ASSERT(nla.cumulative_qty('S', 2) == 300, "itchbook_cumqty_ask2");   // 100 + 200
    ASSERT(nla.cumulative_qty('S', 10) == 600, "itchbook_cumqty_ask_all"); // all (extra 0)
    ASSERT(nla.cumulative_qty('B', 1) == 150, "itchbook_cumqty_bid1");   // best bid only
    ASSERT(nla.cumulative_qty('B', 5) == 400, "itchbook_cumqty_bid_all"); // 150 + 250

    // #301 fill_shortfall (unfilled remainder after sweeping depth).
    ASSERT(nla.fill_shortfall('B', 250) == 0, "itchbook_shortfall_buy_covered");  // <= 600 asks
    ASSERT(nla.fill_shortfall('B', 700) == 100, "itchbook_shortfall_buy_short");  // 700 - 600
    ASSERT(nla.fill_shortfall('S', 300) == 0, "itchbook_shortfall_sell_covered"); // <= 400 bids
    ASSERT(nla.fill_shortfall('S', 500) == 100, "itchbook_shortfall_sell_short"); // 500 - 400

    // #309 depth_notional ($ resting across top-N levels).
    ASSERT(close(nla.depth_notional('S', 2), 30004.0), "itchbook_depthnot_ask2"); // 100.00*100 + 100.02*200
    ASSERT(close(nla.depth_notional('S', 10), 60019.0), "itchbook_depthnot_ask_all"); // + 100.05*300
    ASSERT(close(nla.depth_notional('B', 1), 14998.5), "itchbook_depthnot_bid1");  // 99.99*150
    ASSERT(close(nla.depth_notional('B', 5), 39993.5), "itchbook_depthnot_bid_all"); // + 99.98*250

    // #317 queue_ahead (shares resting at a price level).
    ASSERT(nla.queue_ahead('S', 100.02) == 200, "itchbook_queue_ask_level");
    ASSERT(nla.queue_ahead('B', 99.98) == 250, "itchbook_queue_bid_level");
    ASSERT(nla.queue_ahead('B', 99.99) == 150, "itchbook_queue_best_bid");
    ASSERT(nla.queue_ahead('S', 101.00) == 0, "itchbook_queue_empty_level");  // no level there

    // #325 largest_level_gap (widest tick gap between adjacent levels in top-N).
    ASSERT(nla.largest_level_gap('S', 10) == 3, "itchbook_gap_ask_all");    // .00->.02=2, .02->.05=3
    ASSERT(nla.largest_level_gap('S', 2)  == 2, "itchbook_gap_ask_top2");   // only the .00->.02 step
    ASSERT(nla.largest_level_gap('B', 5)  == 1, "itchbook_gap_bid_contig"); // 99.99->99.98 = 1 tick
    ASSERT(nla.largest_level_gap('S', 1)  == 0, "itchbook_gap_single_level"); // no adjacent pair
    ASSERT(nla.largest_level_gap('B', 0)  == 0, "itchbook_gap_n_zero");

    // #334 book_slope (liquidity gradient: shares of depth per $0.01 tick).
    itch::ITCHOrderBook slp;
    slp.on_add(1, 'B', 100.00, 100); slp.on_add(2, 'B', 99.99, 200); slp.on_add(3, 'B', 99.98, 300);
    slp.on_add(4, 'S', 100.01, 50);  slp.on_add(5, 'S', 100.02, 50);  slp.on_add(6, 'S', 100.03, 50);
    // bids: cum 600 over a 2-tick span (100.00 -> 99.98) = 300 sh/tick
    ASSERT(std::fabs(slp.book_slope('B', 3) - 300.0) < 1e-9, "itchbook_slope_bid");
    // asks: cum 150 over a 2-tick span (100.01 -> 100.03) = 75 sh/tick
    ASSERT(std::fabs(slp.book_slope('S', 3) - 75.0)  < 1e-9, "itchbook_slope_ask");
    // imbalance: (300-75)/(300+75) = 0.6 (bid side steeper)
    ASSERT(std::fabs(slp.book_slope_imbalance(3) - 0.6) < 1e-9, "itchbook_slope_imb");
    ASSERT(slp.book_slope('B', 1) == 0.0, "itchbook_slope_n1_zero");      // need >= 2 levels
    itch::ITCHOrderBook slp1;
    slp1.on_add(1, 'B', 100.00, 100);                                     // single level -> span 0
    ASSERT(slp1.book_slope('B', 5) == 0.0, "itchbook_slope_single_zero");
    ASSERT(slp1.book_slope_imbalance(5) == 0.0, "itchbook_slope_imb_onesided");

    // #215 notional_imbalance (wazony wartoscia, rozny od depth_imbalance).
    itch::ITCHOrderBook ni;
    ni.on_add(1, 'B', 50.00, 200);   // bid $: 50*200 = 10000, 200 shares.
    ni.on_add(2, 'S', 100.00, 150);  // ask $: 100*150 = 15000, 150 shares.
    // notional: (10000-15000)/25000 = -0.2 ; depth (shares): (200-150)/350 = +0.1428
    ASSERT(close(ni.notional_imbalance(1), -0.2), "itchbook_notional_imb");
    ASSERT(ni.depth_imbalance(1) > 0.0, "itchbook_depth_imb_differs_sign");  // opposite sign

    // #183 locked / crossed book.
    itch::ITCHOrderBook nb;
    nb.on_add(1, 'B', 100.00, 100); nb.on_add(2, 'S', 100.02, 100);   // normalny spread
    ASSERT(!nb.is_locked() && !nb.is_crossed(), "itchbook_normal_not_locked_crossed");
    itch::ITCHOrderBook lk;
    lk.on_add(1, 'B', 100.00, 100); lk.on_add(2, 'S', 100.00, 100);   // locked
    ASSERT(lk.is_locked() && !lk.is_crossed(), "itchbook_locked");
    itch::ITCHOrderBook cr;
    cr.on_add(1, 'B', 100.05, 100); cr.on_add(2, 'S', 100.00, 100);   // crossed
    ASSERT(cr.is_crossed() && !cr.is_locked(), "itchbook_crossed");

    // #383 audit_book — cross-check of the per-order map vs level aggregates.
    itch::ITCHOrderBook iaud;
    ASSERT(iaud.audit_book() == 0, "itchbook_audit_empty_ok");
    iaud.on_add(1, 'B', 100.00, 100);
    iaud.on_add(2, 'B', 100.00, 50);      // same level, second order
    iaud.on_add(3, 'B', 99.98, 200);
    iaud.on_add(4, 'S', 100.05, 120);
    ASSERT(iaud.audit_book() == 0, "itchbook_audit_after_adds_ok");
    iaud.on_execute(1, 40);               // partial: order 1 -> 60, level -> 110
    iaud.on_cancel(3, 50);                // partial cancel: 200 -> 150
    iaud.on_replace(4, 5, 100.07, 80);    // reprice the ask
    iaud.on_delete(2);                    // remove the co-resting order
    ASSERT(iaud.audit_book() == 0, "itchbook_audit_full_lifecycle_ok");

    // A corrupted feed REUSING a live ref: on_add overwrites orders_[ref] but
    // double-counts the level aggregate — exactly what the audit must catch.
    itch::ITCHOrderBook iadup;
    iadup.on_add(42, 'B', 10.00, 100);
    iadup.on_add(42, 'B', 10.01, 50);     // duplicate ref, bid side
    ASSERT(iadup.audit_book() == 3, "itchbook_audit_catches_bid_dup_ref");
    itch::ITCHOrderBook iasd;
    iasd.on_add(7, 'S', 20.00, 100);
    iasd.on_add(7, 'S', 20.10, 10);       // duplicate ref, ask side
    ASSERT(iasd.audit_book() == 4, "itchbook_audit_catches_ask_dup_ref");
    // clear() wipes both structures together -> consistent again.
    iadup.clear();
    ASSERT(iadup.audit_book() == 0, "itchbook_audit_ok_after_clear");

    // #391 largest_resting_order — the block/"wall" detector per side.
    itch::ITCHOrderBook ilro;
    double ilro_px = -1.0;
    ASSERT(ilro.largest_resting_order('B') == 0, "itchbook_lro_empty_zero");
    ASSERT(ilro.largest_resting_order('B', &ilro_px) == 0 && ilro_px == -1.0,
           "itchbook_lro_empty_price_untouched");
    ilro.on_add(1, 'B', 100.00, 300);
    ilro.on_add(2, 'B', 99.95, 5000);     // the bid wall
    ilro.on_add(3, 'B', 99.90, 200);
    ilro.on_add(4, 'S', 100.10, 800);     // ask side has its own max
    ASSERT(ilro.largest_resting_order('B', &ilro_px) == 5000, "itchbook_lro_bid_wall");
    ASSERT(std::fabs(ilro_px - 99.95) < 1e-9, "itchbook_lro_bid_wall_price");
    ASSERT(ilro.largest_resting_order('S') == 800, "itchbook_lro_ask_side_separate");
    // A partial execute shrinks the wall below the runner-up -> new winner.
    ilro.on_execute(2, 4800);             // wall 5000 -> 200
    ASSERT(ilro.largest_resting_order('B', &ilro_px) == 300, "itchbook_lro_new_winner");
    ASSERT(std::fabs(ilro_px - 100.00) < 1e-9, "itchbook_lro_new_winner_price");
    // Deleting the winner hands the crown to the next largest.
    ilro.on_delete(1);
    ASSERT(ilro.largest_resting_order('B') == 200, "itchbook_lro_after_delete");

    // #423 largest_level — the aggregate wall vs #391's single-order wall.
    itch::ITCHOrderBook ilvl;
    double ilvl_px = -1.0;
    ASSERT(ilvl.largest_level('B') == 0, "itchbook_lvl_empty_zero");
    ASSERT(ilvl.largest_level('B', &ilvl_px) == 0 && ilvl_px == -1.0,
           "itchbook_lvl_empty_price_untouched");
    // Retail accumulation: 3 small clips stack a level past one big order.
    ilvl.on_add(1, 'B', 10.00, 400);              // single institutional clip
    ilvl.on_add(2, 'B', 9.95, 200);
    ilvl.on_add(3, 'B', 9.95, 200);
    ilvl.on_add(4, 'B', 9.95, 200);               // 9.95 aggregates to 600
    ASSERT(ilvl.largest_level('B', &ilvl_px) == 600, "itchbook_lvl_aggregate_wins");
    ASSERT(std::fabs(ilvl_px - 9.95) < 1e-9, "itchbook_lvl_price");
    // ...while the single-order wall (#391) still points at the 400 clip.
    ASSERT(ilvl.largest_resting_order('B', &ilvl_px) == 400
           && std::fabs(ilvl_px - 10.00) < 1e-9, "itchbook_lvl_vs_lro_divergence");
    // Sides are independent.
    ilvl.on_add(5, 'S', 10.10, 900);
    ASSERT(ilvl.largest_level('S') == 900, "itchbook_lvl_ask_side");
    // Chipping away at the wall hands the crown back to the 10.00 level.
    ilvl.on_execute(2, 200); ilvl.on_delete(3);   // 9.95: 600 -> 200
    ASSERT(ilvl.largest_level('B', &ilvl_px) == 400
           && std::fabs(ilvl_px - 10.00) < 1e-9, "itchbook_lvl_crown_moves");

    // #455 participant_imbalance — heads, not shares (crowd vs whale).
    itch::ITCHOrderBook ipim;
    ASSERT(ipim.participant_imbalance() == 0.0, "itchbook_pim_empty_zero");
    // Three small retail bids vs one institutional block ask of equal size:
    // the SHARE imbalance is flat, the HEAD count leans hard to the bid.
    ipim.on_add(1, 'B', 10.00, 100);
    ipim.on_add(2, 'B', 9.99, 100);
    ipim.on_add(3, 'B', 9.98, 100);
    ipim.on_add(4, 'S', 10.05, 300);
    ASSERT(std::fabs(ipim.participant_imbalance() - 0.5) < 1e-9, "itchbook_pim_crowd_leans_bid");
    ASSERT(std::fabs(ipim.depth_imbalance(5)) < 1e-9, "itchbook_pim_shares_flat_divergence");
    // Balanced heads -> 0.
    ipim.on_add(5, 'S', 10.06, 50);
    ipim.on_add(6, 'S', 10.07, 50);
    ASSERT(ipim.participant_imbalance() == 0.0, "itchbook_pim_balanced");
    // A full execute removes a participant and tips the count.
    ipim.on_execute(1, 100);
    ASSERT(std::fabs(ipim.participant_imbalance() + 0.2) < 1e-9, "itchbook_pim_exec_tips");

    // #447 order_count_at — queue length in PARTICIPANTS at a price.
    itch::ITCHOrderBook ioca;
    ASSERT(ioca.order_count_at('B', 10.00) == 0, "itchbook_oca_empty");
    ioca.on_add(1, 'B', 10.00, 100);
    ioca.on_add(2, 'B', 10.00, 200);
    ioca.on_add(3, 'B', 10.00, 700);
    ioca.on_add(4, 'B', 9.99, 500);               // different level
    ioca.on_add(5, 'S', 10.00, 300);              // same price, other side
    ASSERT(ioca.order_count_at('B', 10.00) == 3, "itchbook_oca_three_in_queue");
    ASSERT(ioca.order_count_at('B', 9.99) == 1, "itchbook_oca_other_level");
    ASSERT(ioca.order_count_at('S', 10.00) == 1, "itchbook_oca_side_separate");
    // Same shares, different queues: 9.99's single 500-lot vs three orders
    // totalling 1000 at 10.00 — qty_at alone cannot tell them apart.
    ASSERT(ioca.qty_at('B', 10.00) == 1000 && ioca.qty_at('B', 9.99) == 500,
           "itchbook_oca_qty_context");
    // A partial execute keeps the participant; a full one removes it.
    ioca.on_execute(1, 60);
    ASSERT(ioca.order_count_at('B', 10.00) == 3, "itchbook_oca_partial_keeps");
    ioca.on_execute(1, 40);
    ASSERT(ioca.order_count_at('B', 10.00) == 2, "itchbook_oca_full_exec_leaves");
    ioca.on_delete(2);
    ASSERT(ioca.order_count_at('B', 10.00) == 1, "itchbook_oca_delete_leaves");

    // #439 depth_concentration — the book's shape as a mass fraction.
    itch::ITCHOrderBook idcn;
    ASSERT(idcn.depth_concentration('B', 3) == 0.0, "itchbook_dcn_empty_zero");
    // Top-heavy: 800 of 1000 shares at the touch.
    idcn.on_add(1, 'B', 10.00, 800);
    idcn.on_add(2, 'B', 9.99, 100);
    idcn.on_add(3, 'B', 9.98, 100);
    ASSERT(std::fabs(idcn.depth_concentration('B', 1) - 0.8) < 1e-9, "itchbook_dcn_top_heavy");
    // Widening the window converges to 1 (all mass inside).
    ASSERT(std::fabs(idcn.depth_concentration('B', 3) - 1.0) < 1e-9, "itchbook_dcn_full_window");
    ASSERT(std::fabs(idcn.depth_concentration('B', 99) - 1.0) < 1e-9, "itchbook_dcn_overshoot_capped");
    // A deep, even ask side reads low at the touch.
    idcn.on_add(4, 'S', 10.10, 100);
    idcn.on_add(5, 'S', 10.11, 100);
    idcn.on_add(6, 'S', 10.12, 100);
    idcn.on_add(7, 'S', 10.13, 100);
    ASSERT(std::fabs(idcn.depth_concentration('S', 1) - 0.25) < 1e-9, "itchbook_dcn_even_book");
    // Sweeping the heavy touch hollows the side: the remaining book is even.
    idcn.on_execute(1, 800);
    ASSERT(std::fabs(idcn.depth_concentration('B', 1) - 0.5) < 1e-9, "itchbook_dcn_after_sweep");

    // #431 avg_reprice_ticks — quote-chasing intensity (how FAR, not how often).
    itch::ITCHOrderBook irpc;
    ASSERT(irpc.avg_reprice_ticks() == 0.0, "itchbook_rpc_empty_zero");
    ASSERT(irpc.max_reprice_ticks() == 0, "itchbook_rpc_max_empty_zero");   // #519
    irpc.on_add(1, 'B', 10.00, 100);
    irpc.on_replace(1, 2, 10.05, 100);            // +5 ticks chase
    ASSERT(std::fabs(irpc.avg_reprice_ticks() - 5.0) < 1e-9, "itchbook_rpc_five_ticks");
    ASSERT(irpc.max_reprice_ticks() == 5, "itchbook_rpc_max_5");   // #519
    // A size-only amendment (same price) dilutes the average toward zero.
    irpc.on_replace(2, 3, 10.05, 50);
    ASSERT(std::fabs(irpc.avg_reprice_ticks() - 2.5) < 1e-9, "itchbook_rpc_size_only_dilutes");
    // Direction does not matter — a retreat is the same distance.
    irpc.on_replace(3, 4, 10.02, 50);             // -3 ticks
    ASSERT(std::fabs(irpc.avg_reprice_ticks() - 8.0 / 3.0) < 1e-9, "itchbook_rpc_retreat_counts");
    // #519: the mean has fallen to 8/3, but the peak still remembers the +5 jump.
    ASSERT(irpc.max_reprice_ticks() == 5, "itchbook_rpc_max_holds_above_mean");
    // An orphaned replace has no old price — excluded from the average.
    irpc.on_replace(999, 5, 20.00, 10);
    ASSERT(std::fabs(irpc.avg_reprice_ticks() - 8.0 / 3.0) < 1e-9, "itchbook_rpc_orphan_excluded");
    irpc.clear();
    ASSERT(irpc.avg_reprice_ticks() == 0.0, "itchbook_rpc_clear");
    ASSERT(irpc.max_reprice_ticks() == 0, "itchbook_rpc_max_clear");   // #519

    // #519: a bigger later reprice raises the high-water mark; a smaller one never lowers it.
    itch::ITCHOrderBook irpm;
    irpm.on_add(1, 'B', 10.00, 100);
    irpm.on_replace(1, 2, 10.03, 100);            // +3 ticks
    ASSERT(irpm.max_reprice_ticks() == 3, "itchbook_rpc_max_3");
    irpm.on_replace(2, 3, 10.15, 100);            // +12 ticks -> new peak
    ASSERT(irpm.max_reprice_ticks() == 12, "itchbook_rpc_max_grows_12");
    irpm.on_replace(3, 4, 10.16, 100);            // +1 tick -> peak unchanged
    ASSERT(irpm.max_reprice_ticks() == 12, "itchbook_rpc_max_not_lowered");

    // #399 orphan_rate / ref_event_count — feed-health ratio behind orphans().
    itch::ITCHOrderBook iorf;
    ASSERT(iorf.orphan_rate() == 0.0 && iorf.ref_event_count() == 0, "itchbook_orate_empty");
    iorf.on_add(1, 'B', 10.00, 100);
    iorf.on_execute(1, 40);
    iorf.on_cancel(1, 10);
    ASSERT(iorf.ref_event_count() == 2 && iorf.orphan_rate() == 0.0, "itchbook_orate_clean");
    // Each orphaned event kind raises the numerator AND the denominator.
    iorf.on_execute(999, 10);                       // unknown ref
    ASSERT(std::fabs(iorf.orphan_rate() - 1.0/3.0) < 1e-9, "itchbook_orate_one_third");
    iorf.on_delete(998);                            // unknown ref
    iorf.on_replace(997, 996, 11.0, 50);            // unknown ref
    ASSERT(iorf.orphans() == 3 && iorf.ref_event_count() == 5, "itchbook_orate_counts");
    ASSERT(std::fabs(iorf.orphan_rate() - 3.0/5.0) < 1e-9, "itchbook_orate_three_fifths");
    // Adds never orphan (new ref) and do not enter the denominator.
    iorf.on_add(2, 'S', 12.00, 100);
    ASSERT(iorf.ref_event_count() == 5, "itchbook_orate_adds_excluded");
    iorf.clear();
    ASSERT(iorf.orphan_rate() == 0.0 && iorf.ref_event_count() == 0, "itchbook_orate_clear");

    // #407 executed_shares / executed_vwap — the tape from the L3 feed.
    itch::ITCHOrderBook itap;
    ASSERT(itap.executed_shares() == 0 && itap.executed_vwap() == 0.0, "itchbook_tape_empty");
    itap.on_add(1, 'B', 10.00, 100);
    itap.on_add(2, 'B', 10.10, 50);
    itap.on_execute(1, 40);                        // 40 @ 10.00
    itap.on_execute(2, 50);                        // 50 @ 10.10 (fully)
    ASSERT(itap.executed_shares() == 90, "itchbook_tape_shares");
    // VWAP = (40*10.00 + 50*10.10) / 90 = 905/90.
    ASSERT(std::fabs(itap.executed_vwap() - 905.0 / 90.0) < 1e-9, "itchbook_tape_vwap");
    // Cancels/deletes remove liquidity without trading — tape untouched.
    itap.on_cancel(1, 30);
    itap.on_add(3, 'S', 11.00, 200);
    itap.on_delete(3);
    ASSERT(itap.executed_shares() == 90, "itchbook_tape_ignores_cancel_delete");
    // Over-execute counts only the truly resting part (order 1 has 30 left).
    itap.on_execute(1, 999);
    ASSERT(itap.executed_shares() == 120, "itchbook_tape_overexec_clamped");
    // An orphan execute (unknown ref) leaves the tape untouched.
    itap.on_execute(777, 50);
    ASSERT(itap.executed_shares() == 120, "itchbook_tape_orphan_ignored");
    // #463 trade_prints / avg_trade_size — three real prints (40, 50, 30) of
    // 120 shares -> avg 40; the orphan execute did NOT count a print.
    ASSERT(itap.trade_prints() == 3, "itchbook_prints_three");
    ASSERT(std::fabs(itap.avg_trade_size() - 40.0) < 1e-9, "itchbook_avg_trade_40");
    // #503 largest_trade_size: prints 40/50/30 -> the biggest single is 50.
    ASSERT(itap.largest_trade_size() == 50, "itchbook_largest_trade_50");
    itap.clear();
    ASSERT(itap.executed_shares() == 0 && itap.executed_vwap() == 0.0, "itchbook_tape_clear");
    ASSERT(itap.trade_prints() == 0 && itap.avg_trade_size() == 0.0, "itchbook_prints_clear");
    ASSERT(itap.largest_trade_size() == 0, "itchbook_largest_trade_clear");

    // #503: block dominance -> one 1000-share block amid 100-share slices
    // gives a largest far above the average.
    itch::ITCHOrderBook ilg;
    ASSERT(ilg.largest_trade_size() == 0, "itchbook_largest_empty");
    ilg.on_add(1, 'S', 20.00, 2000);
    ilg.on_execute(1, 100); ilg.on_execute(1, 100);
    ilg.on_execute(1, 1000);                       // the block
    ilg.on_execute(1, 100);
    ASSERT(ilg.largest_trade_size() == 1000, "itchbook_largest_is_block");
    ASSERT(ilg.largest_trade_size() > static_cast<uint32_t>(ilg.avg_trade_size() * 2),
           "itchbook_block_dominates_avg");

    // #463: a block tape vs a sliced tape reads very differently in avg size.
    itch::ITCHOrderBook iblk;
    ASSERT(iblk.avg_trade_size() == 0.0, "itchbook_avg_trade_empty");
    iblk.on_add(1, 'S', 20.00, 1000);
    iblk.on_execute(1, 1000);                      // one block print of 1000
    ASSERT(iblk.trade_prints() == 1 && std::fabs(iblk.avg_trade_size() - 1000.0) < 1e-9,
           "itchbook_avg_trade_block");
    itch::ITCHOrderBook islc;
    islc.on_add(1, 'S', 20.00, 1000);
    for (int k = 0; k < 10; ++k) islc.on_execute(1, 100);   // ten sliced prints
    ASSERT(islc.trade_prints() == 10 && std::fabs(islc.avg_trade_size() - 100.0) < 1e-9,
           "itchbook_avg_trade_sliced");

    // #471 last_trade_price / last_trade_is_buy — the tape's last print + tick.
    itch::ITCHOrderBook iltp;
    ASSERT(iltp.last_trade_price() == 0.0, "itchbook_ltp_empty_zero");
    iltp.on_add(1, 'B', 10.00, 500);              // resting bid
    iltp.on_add(2, 'S', 10.05, 500);              // resting ask
    iltp.on_execute(2, 100);                      // ask lifted -> buyer-initiated @10.05
    ASSERT(std::fabs(iltp.last_trade_price() - 10.05) < 1e-9, "itchbook_ltp_lift_price");
    ASSERT(iltp.last_trade_is_buy(), "itchbook_ltp_lift_is_buy");
    iltp.on_execute(1, 100);                      // bid hit -> seller-initiated @10.00
    ASSERT(std::fabs(iltp.last_trade_price() - 10.00) < 1e-9, "itchbook_ltp_hit_price");
    ASSERT(!iltp.last_trade_is_buy(), "itchbook_ltp_hit_is_sell");
    // Cancels/deletes do NOT print — the last trade is unchanged.
    iltp.on_cancel(1, 50);
    iltp.on_add(3, 'S', 11.00, 100); iltp.on_delete(3);
    ASSERT(std::fabs(iltp.last_trade_price() - 10.00) < 1e-9, "itchbook_ltp_cancel_no_print");
    // An orphan execute does not move the tape either.
    iltp.on_execute(999, 10);
    ASSERT(std::fabs(iltp.last_trade_price() - 10.00) < 1e-9, "itchbook_ltp_orphan_no_print");
    iltp.clear();
    ASSERT(iltp.last_trade_price() == 0.0 && !iltp.last_trade_is_buy(), "itchbook_ltp_clear");

    // #479 tick_direction — the SSR uptick rule with zero-plus carry.
    itch::ITCHOrderBook itk;
    ASSERT(itk.tick_direction() == 0 && !itk.is_uptick(), "itchbook_tick_empty_zero");
    itk.on_add(1, 'S', 10.00, 100);
    itk.on_add(2, 'S', 10.02, 100);
    itk.on_add(3, 'S', 10.05, 100);
    itk.on_add(4, 'B', 9.98, 100);
    itk.on_add(5, 'B', 9.98, 100);
    itk.on_execute(1, 100);                       // first print @10.00 -> no prior tick
    ASSERT(itk.tick_direction() == 0, "itchbook_tick_first_print_zero");
    itk.on_execute(2, 100);                       // @10.02 > 10.00 -> uptick
    ASSERT(itk.tick_direction() == 1 && itk.is_uptick(), "itchbook_tick_uptick");
    itk.on_execute(3, 100);                       // @10.05 > 10.02 -> still up
    ASSERT(itk.tick_direction() == 1, "itchbook_tick_still_up");
    itk.on_execute(4, 100);                       // @9.98 < 10.05 -> downtick
    ASSERT(itk.tick_direction() == -1 && !itk.is_uptick(), "itchbook_tick_downtick");
    itk.on_execute(5, 100);                       // @9.98 == 9.98 -> carry -1 (zero-minus)
    ASSERT(itk.tick_direction() == -1, "itchbook_tick_zero_minus_carry");
    // The tick test is price-based, distinct from the aggressor (#471):
    // the last print hit a bid (seller-initiated) AND was a downtick.
    ASSERT(!itk.last_trade_is_buy() && itk.tick_direction() == -1, "itchbook_tick_vs_aggressor");
    itk.clear();
    ASSERT(itk.tick_direction() == 0, "itchbook_tick_clear");

    // #487 aggressor_run — the live streak of same-side prints.
    itch::ITCHOrderBook iar;
    ASSERT(iar.aggressor_run() == 0, "itchbook_run_empty_zero");
    iar.on_add(1, 'S', 10.05, 100);
    iar.on_add(2, 'S', 10.06, 100);
    iar.on_add(3, 'S', 10.07, 100);
    iar.on_add(4, 'B', 9.98, 100);
    iar.on_add(5, 'B', 9.97, 100);
    iar.on_execute(1, 100);                       // lift ask -> buy, run +1
    ASSERT(iar.aggressor_run() == 1, "itchbook_run_first_buy");
    iar.on_execute(2, 100);                       // buy, run +2
    iar.on_execute(3, 100);                       // buy, run +3
    ASSERT(iar.aggressor_run() == 3, "itchbook_run_three_buys");
    iar.on_execute(4, 100);                       // hit bid -> sell, run flips to -1
    ASSERT(iar.aggressor_run() == -1, "itchbook_run_flip_to_sell");
    iar.on_execute(5, 100);                       // sell, run -2
    ASSERT(iar.aggressor_run() == -2, "itchbook_run_two_sells");
    // Cancels/deletes/orphans do not print -> the run is unchanged.
    iar.on_add(6, 'S', 11.00, 100);
    iar.on_cancel(6, 50); iar.on_delete(6);
    iar.on_execute(999, 10);
    ASSERT(iar.aggressor_run() == -2, "itchbook_run_only_prints_move_it");
    // #511 longest_buy_run / longest_sell_run — sweep-length high-water marks.
    // iar ran 3 buys then 2 sells, so the peaks are 3 and 2.
    ASSERT(iar.longest_buy_run() == 3 && iar.longest_sell_run() == 2, "itchbook_sweep_highwater");
    iar.clear();
    ASSERT(iar.aggressor_run() == 0, "itchbook_run_clear");
    ASSERT(iar.longest_buy_run() == 0 && iar.longest_sell_run() == 0, "itchbook_sweep_highwater_clear");

    // #511: a later, SHORTER run must not lower the high-water mark.
    itch::ITCHOrderBook isw;
    isw.on_add(1, 'S', 10.0, 100); isw.on_add(2, 'S', 10.0, 100); isw.on_add(3, 'S', 10.0, 100);
    isw.on_add(4, 'B', 9.0, 100);  isw.on_add(5, 'S', 10.0, 100);
    isw.on_execute(1, 100); isw.on_execute(2, 100); isw.on_execute(3, 100);   // 3 buys in a row
    ASSERT(isw.longest_buy_run() == 3, "itchbook_sweep_buy_3");
    isw.on_execute(4, 100);   // hit bid -> 1 sell, flips the live run
    isw.on_execute(5, 100);   // lift ask -> new BUY run of just +1
    ASSERT(isw.aggressor_run() == 1 && isw.longest_buy_run() == 3, "itchbook_sweep_survives_shorter");
    ASSERT(isw.longest_sell_run() == 1, "itchbook_sweep_sell_1");

    // #415 executed_against_bid/ask + tape_imbalance — exact aggressor flow.
    itch::ITCHOrderBook iagr;
    ASSERT(iagr.tape_imbalance() == 0.0, "itchbook_agr_empty_zero");
    iagr.on_add(1, 'B', 10.00, 500);              // resting bid
    iagr.on_add(2, 'S', 10.05, 500);              // resting ask
    iagr.on_execute(2, 300);                      // ask lifted = buyer-initiated
    ASSERT(iagr.executed_against_ask() == 300 && iagr.executed_against_bid() == 0,
           "itchbook_agr_lift_counts_ask");
    ASSERT(std::fabs(iagr.tape_imbalance() - 1.0) < 1e-9, "itchbook_agr_pure_buying");
    // #495 cumulative_delta: 300 bought, 0 sold -> +300 (the LEVEL, not ratio).
    ASSERT(iagr.cumulative_delta() == 300, "itchbook_cvd_plus_300");
    iagr.on_execute(1, 100);                      // bid hit = seller-initiated
    // (300 - 100) / 400 = +0.5 — net buying pressure.
    ASSERT(std::fabs(iagr.tape_imbalance() - 0.5) < 1e-9, "itchbook_agr_net_half");
    // #495: CVD 300 - 100 = +200.
    ASSERT(iagr.cumulative_delta() == 200, "itchbook_cvd_plus_200");
    // The split always sums to the #407 tape total.
    ASSERT(iagr.executed_against_bid() + iagr.executed_against_ask()
           == iagr.executed_shares(), "itchbook_agr_sums_to_tape");
    iagr.on_execute(1, 200);                      // heavy selling flips the sign
    ASSERT(iagr.tape_imbalance() == 0.0, "itchbook_agr_balanced_at_300_300");
    // #495: balanced tape -> CVD exactly 0 (where the ratio is also 0).
    ASSERT(iagr.cumulative_delta() == 0, "itchbook_cvd_balanced_zero");
    iagr.on_execute(1, 100);                      // 300 lifted vs 400 hit
    ASSERT(iagr.tape_imbalance() < 0.0, "itchbook_agr_net_selling_negative");
    // #495: CVD 300 - 400 = -100 (net selling in shares).
    ASSERT(iagr.cumulative_delta() == -100, "itchbook_cvd_net_selling");
    iagr.clear();
    ASSERT(iagr.executed_against_bid() == 0 && iagr.tape_imbalance() == 0.0,
           "itchbook_agr_clear");
    ASSERT(iagr.cumulative_delta() == 0, "itchbook_cvd_clear");

    sb.clear();
    ASSERT(sb.resting_orders() == 0 && sb.best_bid() == 0.0, "itchbook_clear_resets");
}

// Multicast gap-recovery #82 — detekcja luk + retransmisja + rekoncyliacja.
void test_multicast_gap_recovery() {
    SECTION("Multicast Gap Recovery (#82)");
    multicast::GapRecovery gr;
    gr.observe(1); gr.observe(2);
    gr.observe(5);                                // gap: missing 3,4
    ASSERT(gr.has_gaps() && gr.missing_count() == 2, "gaprec_two_missing");
    uint64_t lo = 0, hi = 0;
    ASSERT(gr.next_request(lo, hi) && lo == 3 && hi == 4, "gaprec_request_range");

    ASSERT(gr.on_retransmit(3), "gaprec_recover_3");
    ASSERT(gr.on_retransmit(4), "gaprec_recover_4");
    ASSERT(!gr.has_gaps(), "gaprec_fully_recovered");
    ASSERT(gr.recovered == 2, "gaprec_recovered_count");
    ASSERT(!gr.on_retransmit(99), "gaprec_unknown_retransmit_false");

    gr.observe(8);                               // gap: missing 6,7
    gr.observe(6);                               // a late primary fills 6
    ASSERT(gr.missing_count() == 1 && gr.recovered == 3, "gaprec_late_primary_recovers");

    // #149 lista zakresow luk (ciagle przedzialy).
    multicast::GapRecovery mr2;
    mr2.observe(1); mr2.observe(5);                      // missing 2,3,4 -> [2,4]
    mr2.observe(10);                                     // missing 6,7,8,9 -> [6,9]
    const auto rngs = mr2.missing_ranges();
    ASSERT(rngs.size() == 2, "gaprec_two_ranges");
    ASSERT(rngs[0].first == 2 && rngs[0].second == 4, "gaprec_range_2_4");
    ASSERT(rngs[1].first == 6 && rngs[1].second == 9, "gaprec_range_6_9");

    // #156 recovery_completeness.
    multicast::GapRecovery rc2;
    ASSERT(std::fabs(rc2.recovery_completeness() - 1.0) < 1e-9, "gaprec_complete_when_empty");
    rc2.observe(1); rc2.observe(4);                     // missing 2,3
    ASSERT(std::fabs(rc2.recovery_completeness() - 0.0) < 1e-9, "gaprec_complete_0");
    rc2.on_retransmit(2);                               // recovered 1, missing 1
    ASSERT(std::fabs(rc2.recovery_completeness() - 0.5) < 1e-9, "gaprec_complete_half");
    rc2.on_retransmit(3);                               // recovered 2, missing 0
    ASSERT(std::fabs(rc2.recovery_completeness() - 1.0) < 1e-9, "gaprec_complete_full");

    // #329 avg_gap_burst — consecutive-missing run per gap event.
    multicast::GapRecovery agb;
    ASSERT(agb.avg_gap_burst() == 0.0, "gaprec_burst_none");
    agb.observe(1); agb.observe(2); agb.observe(5);     // one gap event, run of 2 (3,4)
    ASSERT(agb.gap_events == 1 && std::fabs(agb.avg_gap_burst() - 2.0) < 1e-9, "gaprec_burst_2");
    agb.observe(6);                                     // contiguous, no new gap
    ASSERT(std::fabs(agb.avg_gap_burst() - 2.0) < 1e-9, "gaprec_burst_still_2");
    agb.observe(8);                                     // second gap event, run of 1 (7)
    // total lost = 3 (3,4,7) over 2 gap events -> 1.5
    ASSERT(agb.gap_events == 2 && std::fabs(agb.avg_gap_burst() - 1.5) < 1e-9, "gaprec_burst_avg_1p5");
    // #475 gap_event_rate: 2 gap events over 5 observed packets -> 0.4.
    // Distinct from avg_gap_burst (SIZE per event): this is FREQUENCY per
    // packet. observe count = 5 (1,2,5,6,8).
    ASSERT(agb.primary_packets == 5 && std::fabs(agb.gap_event_rate() - 0.4) < 1e-9,
           "gaprec_gap_event_rate");
    // recovering does not change the lifetime burst average (recovered+missing invariant)
    agb.on_retransmit(3); agb.on_retransmit(4); agb.on_retransmit(7);
    ASSERT(std::fabs(agb.avg_gap_burst() - 1.5) < 1e-9, "gaprec_burst_recover_invariant");
    // #475: recovery touches neither gap_events nor primary_packets.
    ASSERT(std::fabs(agb.gap_event_rate() - 0.4) < 1e-9, "gaprec_gap_event_rate_stable");
    // A clean in-order feed never opens a gap -> rate 0.
    multicast::GapRecovery gcln;
    gcln.observe(1); gcln.observe(2); gcln.observe(3); gcln.observe(4);
    ASSERT(gcln.gap_event_rate() == 0.0, "gaprec_gap_event_rate_clean_zero");
    multicast::GapRecovery gempty;
    ASSERT(gempty.gap_event_rate() == 0.0, "gaprec_gap_event_rate_empty_zero");

    // #338 outstanding_range_count / largest_outstanding_run — live fragmentation.
    multicast::GapRecovery gror;
    ASSERT(gror.outstanding_range_count() == 0 && gror.largest_outstanding_run() == 0,
           "gaprec_runs_empty");
    gror.observe(1);
    gror.observe(5);     // miss 2,3,4  -> one run of 3
    gror.observe(10);    // miss 6,7,8,9 -> a second run of 4
    ASSERT(gror.outstanding_range_count() == 2, "gaprec_two_runs");
    ASSERT(gror.largest_outstanding_run() == 4, "gaprec_largest_run_4");
    gror.on_retransmit(7);  // splits 6,7,8,9 into 6 | 8,9 -> three runs, largest now 3 (2,3,4)
    ASSERT(gror.outstanding_range_count() == 3, "gaprec_three_runs_after_fill");
    ASSERT(gror.largest_outstanding_run() == 3, "gaprec_largest_run_now_3");
    gror.on_retransmit(2); gror.on_retransmit(3); gror.on_retransmit(4);
    gror.on_retransmit(6); gror.on_retransmit(8); gror.on_retransmit(9);
    ASSERT(!gror.has_gaps() && gror.largest_outstanding_run() == 0, "gaprec_runs_cleared");

    // #379 oldest_missing / head_of_line_lag — depth of the oldest stall.
    multicast::GapRecovery ghl;
    ASSERT(ghl.oldest_missing() == 0 && ghl.head_of_line_lag() == 0, "gaprec_hol_empty");
    ghl.observe(1);
    ghl.observe(5);      // miss 2,3,4; expected=6 -> oldest hole 2, lag 6-2=4
    ASSERT(ghl.oldest_missing() == 2, "gaprec_hol_oldest_2");
    ASSERT(ghl.head_of_line_lag() == 4, "gaprec_hol_lag_4");
    ghl.observe(10);     // miss 6..9 too; the OLDEST hole (2) still rules the lag
    ASSERT(ghl.oldest_missing() == 2 && ghl.head_of_line_lag() == 9, "gaprec_hol_live_edge_grows");
    ghl.on_retransmit(2);   // head hole filled -> next oldest is 3
    ASSERT(ghl.oldest_missing() == 3 && ghl.head_of_line_lag() == 8, "gaprec_hol_advances");
    ghl.on_retransmit(3); ghl.on_retransmit(4);
    ASSERT(ghl.oldest_missing() == 6 && ghl.head_of_line_lag() == 5, "gaprec_hol_skips_to_next_range");
    ghl.on_retransmit(6); ghl.on_retransmit(7); ghl.on_retransmit(8); ghl.on_retransmit(9);
    ASSERT(ghl.head_of_line_lag() == 0, "gaprec_hol_contiguous_zero");
    // snapshot_resync abandons the holes -> lag back to 0.
    multicast::GapRecovery ghs;
    ghs.observe(1); ghs.observe(50);
    ASSERT(ghs.head_of_line_lag() == 49, "gaprec_hol_deep_stall");
    ghs.snapshot_resync(60);
    ASSERT(ghs.oldest_missing() == 0 && ghs.head_of_line_lag() == 0, "gaprec_hol_snapshot_clears");

    // #110 ReorderBuffer — always delivers in order, holds "future" ones.
    multicast::ReorderBuffer<int> rb;
    rb.push(1, 10);                              // expected=1 -> dostarcz, expected->2
    ASSERT(rb.out.size() == 1 && rb.out[0] == 10, "reorder_inorder_deliver");
    rb.push(3, 30);                              // gap at 2 -> buffer
    rb.push(4, 40);                              // buffer it
    ASSERT(rb.out.size() == 1 && rb.buffered() == 2, "reorder_holds_future");
    rb.push(2, 20);                              // fills the gap -> drain 2,3,4
    ASSERT(rb.buffered() == 0, "reorder_drained");

    // #411 max_buffered_depth — the burst high-water mark for buffer sizing.
    // rb held {3,4} plus the gap-filling 2 in transit -> deepest was 3.
    ASSERT(rb.max_buffered_depth() == 3, "reorder_hwm_burst_3");
    // A purely in-order stream only ever uses the one transit slot.
    multicast::ReorderBuffer<int> rbh;
    rbh.push(1, 10); rbh.push(2, 20); rbh.push(3, 30);
    ASSERT(rbh.buffered() == 0 && rbh.max_buffered_depth() == 1, "reorder_hwm_inorder_1");
    // A deeper burst raises it; draining does not lower it.
    rbh.push(9, 90); rbh.push(8, 80); rbh.push(7, 70); rbh.push(6, 60);
    ASSERT(rbh.max_buffered_depth() == 4, "reorder_hwm_deep_burst");
    rbh.push(4, 40); rbh.push(5, 50);            // drains everything
    ASSERT(rbh.buffered() == 0 && rbh.max_buffered_depth() == 5, "reorder_hwm_retained");
    // Duplicates never enter the map and cannot move the mark.
    rbh.push(2, 20);
    ASSERT(rbh.duplicates == 1 && rbh.max_buffered_depth() == 5, "reorder_hwm_dup_ignored");
    rbh.reset();
    ASSERT(rbh.max_buffered_depth() == 0, "reorder_hwm_reset");
    ASSERT(rb.out.size() == 4 && rb.out[1] == 20 && rb.out[2] == 30 && rb.out[3] == 40,
           "reorder_delivered_in_order");
    rb.push(2, 99);                              // < expected -> duplicate
    ASSERT(rb.duplicates == 1 && rb.out.size() == 4, "reorder_drops_duplicate");

    // #115 snapshot vs retransmission: a big gap -> snapshot resync.
    multicast::GapRecovery sr;
    sr.observe(1); sr.observe(10);               // missing 2..9 (8 packets)
    ASSERT(sr.missing_count() == 8, "snap_big_gap");
    ASSERT(sr.recommend_snapshot(5), "snap_recommend_over_threshold");   // 8>=5
    ASSERT(!sr.recommend_snapshot(20), "snap_no_recommend_under");
    sr.snapshot_resync(10);                      // snapshot pokrywa do seq 10
    ASSERT(!sr.has_gaps() && sr.expected == 11, "snap_resync_clears_gaps");

    // #122 MultiChannelRecovery — agregacja po kanalach feedu.
    multicast::MultiChannelRecovery mc;
    mc.observe(1, 1); mc.observe(1, 3);          // channel 1: gap (missing 2)
    mc.observe(2, 5); mc.observe(2, 6);          // channel 2: in order
    ASSERT(mc.channel_count() == 2, "mcr_two_channels");
    ASSERT(mc.any_gaps() && mc.total_missing() == 1, "mcr_gap_in_ch1");
    ASSERT(mc.on_retransmit(1, 2), "mcr_recover_ch1");
    ASSERT(!mc.any_gaps() && mc.total_recovered() == 1, "mcr_all_recovered");

    // #395 worst_channel / channels_with_gaps — triage across channels.
    multicast::MultiChannelRecovery mcw;
    std::uint32_t mcw_ch = 777;
    std::size_t   mcw_miss = 777;
    ASSERT(!mcw.worst_channel(mcw_ch, mcw_miss) && mcw_ch == 777 && mcw_miss == 777,
           "mcr_worst_none_untouched");
    ASSERT(mcw.channels_with_gaps() == 0, "mcr_breadth_zero");
    mcw.observe(10, 1); mcw.observe(10, 4);      // ch 10: missing 2,3 (2 holes)
    mcw.observe(20, 1); mcw.observe(20, 7);      // ch 20: missing 2..6 (5 holes)
    mcw.observe(30, 1); mcw.observe(30, 2);      // ch 30: clean
    ASSERT(mcw.channels_with_gaps() == 2, "mcr_breadth_two_of_three");
    ASSERT(mcw.worst_channel(mcw_ch, mcw_miss) && mcw_ch == 20 && mcw_miss == 5,
           "mcr_worst_is_ch20");
    // Healing ch 20 fully hands the worst spot to ch 10.
    for (std::uint64_t mcs = 2; mcs <= 6; ++mcs) mcw.on_retransmit(20, mcs);
    ASSERT(mcw.channels_with_gaps() == 1, "mcr_breadth_after_heal");
    ASSERT(mcw.worst_channel(mcw_ch, mcw_miss) && mcw_ch == 10 && mcw_miss == 2,
           "mcr_worst_moves_to_ch10");
    mcw.on_retransmit(10, 2); mcw.on_retransmit(10, 3);
    ASSERT(!mcw.worst_channel(mcw_ch, mcw_miss) && mcw.channels_with_gaps() == 0,
           "mcr_worst_all_healed");

    // #132 FeedRateMeter — sliding-window rate.
    multicast::FeedRateMeter fr(1000);                    // 1000 ns window
    fr.on_message(100); fr.on_message(200); fr.on_message(300);
    ASSERT(fr.count(300) == 3, "rate_3_in_window");
    ASSERT(std::fabs(fr.rate_per_sec(300) - 3e6) < 1.0, "rate_per_sec_3M");  // 3*1e9/1000
    ASSERT(fr.count(1301) == 0, "rate_window_expired");   // all older than the window

    // #163 peak rate (burst).
    multicast::FeedRateMeter pm(1000);
    pm.on_message(0); pm.on_message(100); pm.on_message(200);  // 3 w oknie -> peak 3
    ASSERT(pm.peak_count() == 3, "rate_peak_3");
    pm.on_message(1300);                                       // old ones evicted, count 1
    ASSERT(pm.count(1300) == 1 && pm.peak_count() == 3, "rate_peak_holds");
    ASSERT(std::fabs(pm.peak_rate_per_sec() - 3e6) < 1.0, "rate_peak_rate_3M");

    // ring wrap correctness: push 10k events 1 ns apart through the 4096 ring → the
    // index wraps ~2.4×; with a 1000 ns window only the last 1000 stay (9000..9999).
    multicast::FeedRateMeter wrp(1000);
    for (int i = 0; i < 10000; ++i) wrp.on_message(i);
    ASSERT(wrp.count(9999) == 1000, "rate_wrap_window_1000");
    // bounded window: 6000 simultaneous events exceed RING_SIZE-1 → count saturates.
    multicast::FeedRateMeter sat(1'000'000'000);
    for (int i = 0; i < 6000; ++i) sat.on_message(0);
    ASSERT(sat.count(0) == static_cast<std::size_t>(multicast::FeedRateMeter::RING_SIZE - 1),
           "rate_saturates_at_cap");

    // #171 DedupWindow — at-most-once (rejects duplicates).
    multicast::DedupWindow dw(100);
    ASSERT(dw.accept(1), "dedup_1_new");
    ASSERT(!dw.accept(1), "dedup_1_dup");
    ASSERT(dw.accept(2), "dedup_2_new");
    ASSERT(dw.accept(5), "dedup_5_new_gap_ok");     // gap OK, this is not a duplicate
    ASSERT(!dw.accept(5), "dedup_5_dup");
    ASSERT(dw.duplicates == 2, "dedup_count");

    // #403 accepted / total_seen / dup_rate — the hygiene ratio at the
    // dedup stage (GapRecovery::duplicate_rate #321 flags the same
    // pathology after sequencing).
    ASSERT(dw.accepted == 3 && dw.total_seen() == 5, "dedup_totals");
    ASSERT(std::fabs(dw.dup_rate() - 2.0 / 5.0) < 1e-9, "dedup_rate_two_fifths");
    multicast::DedupWindow ddr(100);
    ASSERT(ddr.dup_rate() == 0.0 && ddr.total_seen() == 0, "dedup_rate_empty_zero");
    ddr.accept(1); ddr.accept(2); ddr.accept(3);    // clean single line
    ASSERT(ddr.dup_rate() == 0.0 && ddr.accepted == 3, "dedup_rate_clean_line_zero");
    // A bridged second line replays everything -> the rate steps toward 0.5.
    ddr.accept(1); ddr.accept(2); ddr.accept(3);
    ASSERT(std::fabs(ddr.dup_rate() - 0.5) < 1e-9, "dedup_rate_bridged_half");
    // A stale packet far below the window counts as rejected too.
    ddr.accept(200);                                 // advances high
    ASSERT(!ddr.accept(50) && ddr.duplicates == 4, "dedup_stale_counts_rejected");
    ddr.reset();
    ASSERT(ddr.total_seen() == 0 && ddr.dup_rate() == 0.0, "dedup_rate_reset");

    // #179 BackpressureMonitor — zaleglosc konsumenta wzgledem feedu.
    multicast::BackpressureMonitor bp;
    bp.on_enqueue(); bp.on_enqueue(); bp.on_enqueue();   // depth 3
    ASSERT(bp.depth() == 3 && bp.peak_depth == 3, "bp_depth_peak");
    bp.on_dequeue();                                     // depth 2
    ASSERT(bp.depth() == 2 && bp.peak_depth == 3, "bp_peak_retained");
    ASSERT(bp.overloaded(2) && !bp.overloaded(5), "bp_overloaded_threshold");
    bp.on_dequeue(10);                                   // does not go below 0
    ASSERT(bp.depth() == 0, "bp_no_underflow");
    // #491 drain_ratio / ever_overloaded. bp fully drained (3 in, 3 out).
    ASSERT(std::fabs(bp.drain_ratio() - 1.0) < 1e-9, "bp_drain_full");
    // The queue spiked to 3 then drained: overloaded() is now false but
    // ever_overloaded remembers the breach.
    ASSERT(!bp.overloaded(3) && bp.ever_overloaded(3), "bp_ever_vs_now");
    ASSERT(!bp.ever_overloaded(4), "bp_ever_below_peak_false");
    // A fresh monitor: 1.0 before anything, then the mid-drain fraction.
    multicast::BackpressureMonitor bpd;
    ASSERT(bpd.drain_ratio() == 1.0, "bp_drain_empty_one");
    bpd.on_enqueue(4); bpd.on_dequeue(1);                 // 1 of 4 processed
    ASSERT(std::fabs(bpd.drain_ratio() - 0.25) < 1e-9, "bp_drain_quarter");
    bpd.reset();
    ASSERT(bpd.drain_ratio() == 1.0 && !bpd.ever_overloaded(1), "bp_drain_reset");

    // #187 LossRateMeter — agregatowa stopa utraty.
    multicast::LossRateMeter lrm;
    const uint64_t seqs[] = {1, 2, 3, 5, 6};             // missing 4; range 1..6
    for (uint64_t s : seqs) lrm.on_packet(s);
    ASSERT(lrm.expected() == 6 && lrm.received == 5, "loss_expected_received");
    ASSERT(lrm.lost() == 1, "loss_count");
    ASSERT(std::fabs(lrm.loss_rate() - 1.0/6.0) < 1e-9, "loss_rate");

    // #195 OutOfOrderMeter — the fraction of out-of-order packets.
    multicast::OutOfOrderMeter ooo;
    const uint64_t oseq[] = {1, 2, 4, 3, 5};             // 3 arrives after 4
    for (uint64_t s : oseq) ooo.on_packet(s);
    ASSERT(ooo.total == 5 && ooo.out_of_order == 1, "ooo_counts");
    ASSERT(std::fabs(ooo.ooo_rate() - 0.2) < 1e-9, "ooo_rate");
    // #370 max_reorder_depth: 3 after 4 -> backward jump 4-3 = 1.
    ASSERT(ooo.max_reorder_depth() == 1, "ooo_max_depth_1");
    // deeper reorder: highest reaches 10, then 4 arrives -> depth 6 (dominates).
    multicast::OutOfOrderMeter ooo2;
    const uint64_t dseq[] = {1, 10, 8, 4, 11};   // 8 after 10 (depth 2), 4 after 10 (depth 6)
    for (uint64_t s : dseq) ooo2.on_packet(s);
    ASSERT(ooo2.out_of_order == 2, "ooo_max_depth_counts");
    ASSERT(ooo2.max_reorder_depth() == 6, "ooo_max_depth_6");
    // #467 avg_reorder_depth: depths 2 and 6 -> mean 4.0 (vs max 6 = a
    // freak deep reorder over an otherwise shallow one).
    ASSERT(std::fabs(ooo2.avg_reorder_depth() - 4.0) < 1e-9, "ooo_avg_depth_4");
    // A single shallow swap: avg == max == 1.
    ASSERT(std::fabs(ooo.avg_reorder_depth() - 1.0) < 1e-9, "ooo_avg_depth_shallow_1");
    multicast::OutOfOrderMeter ooo0;
    ooo0.on_packet(1); ooo0.on_packet(2); ooo0.on_packet(3);   // all in order
    ASSERT(ooo0.max_reorder_depth() == 0, "ooo_max_depth_in_order_zero");
    ASSERT(ooo0.avg_reorder_depth() == 0.0, "ooo_avg_depth_in_order_zero");
    ooo2.reset();
    ASSERT(ooo2.avg_reorder_depth() == 0.0 && ooo2.max_reorder_depth() == 0, "ooo_avg_reset");

    // #203 SequenceResetDetector — reset vs reorder.
    multicast::SequenceResetDetector srd(1000);
    ASSERT(!srd.on_seq(5000), "srd_init_no_reset");
    ASSERT(!srd.on_seq(5001), "srd_normal");
    ASSERT(!srd.on_seq(4999), "srd_small_reorder_not_reset");   // drop 2 < 1000
    ASSERT(srd.on_seq(10), "srd_big_drop_is_reset");            // 5001 -> 10
    ASSERT(srd.resets == 1, "srd_reset_count");
    ASSERT(!srd.on_seq(11), "srd_normal_after_reset");          // new base 10

    // #211 SnapshotRequestThrottle — min interval between requests.
    multicast::SnapshotRequestThrottle srt(1000);               // min 1000 ns
    ASSERT(srt.allow(0), "srt_first_allowed");
    ASSERT(!srt.allow(500), "srt_throttled_500");               // 500 < 1000 od 0
    ASSERT(!srt.allow(999), "srt_throttled_999");
    ASSERT(srt.allow(1000), "srt_allowed_1000");                // 1000 >= 1000
    ASSERT(srt.allow(2500), "srt_allowed_2500");                // 1500 od ostatniego
    ASSERT(srt.suppressed == 2, "srt_suppressed_count");

    // #219 TokenBucket — burst up to capacity + refill over time.
    multicast::TokenBucket tb(5.0, 1000.0);                     // 5 tokenow, 1000/s
    for (int i = 0; i < 5; ++i) ASSERT(tb.try_consume(0, 1.0), "tb_burst_ok");  // 5 od reki
    ASSERT(!tb.try_consume(0, 1.0), "tb_empty");               // 6ty pusto
    // after 2.5 ms it refills ~2.5 tokens -> two consumes ok, the third not
    ASSERT(tb.try_consume(2500000, 1.0), "tb_refill_1");
    ASSERT(tb.try_consume(2500000, 1.0), "tb_refill_2");
    ASSERT(!tb.try_consume(2500000, 1.0), "tb_refill_exhausted");

    // #451 available / denied — the probe and the ops counter.
    // Two refusals so far (tb_empty + tb_refill_exhausted).
    ASSERT(tb.denied == 2, "tb_denied_counts_refusals");
    // The balance is ~0.5 (2.5 refilled, 2 consumed); probing takes nothing.
    ASSERT(std::fabs(tb.available(2500000) - 0.5) < 1e-9, "tb_available_balance");
    ASSERT(std::fabs(tb.available(2500000) - 0.5) < 1e-9, "tb_probe_is_free");
    // The probe tops up by elapsed time exactly like try_consume would...
    ASSERT(std::fabs(tb.available(4000000) - 2.0) < 1e-9, "tb_available_refills");
    // ...and caps at capacity on a long idle stretch.
    ASSERT(std::fabs(tb.available(999000000) - 5.0) < 1e-9, "tb_available_caps");
    tb.reset();
    ASSERT(tb.denied == 0, "tb_reset_clears_denied");

    // #227 ConflationBuffer — the latest state per key + a conflation counter.
    multicast::ConflationBuffer cb;
    cb.update(1, 100.0);              // key 1
    cb.update(1, 101.0);             // nadpisanie -> konflacja 1
    cb.update(2, 50.0);             // new key
    cb.update(1, 102.0);            // konflacja 2
    ASSERT(cb.pending() == 2 && cb.conflated == 2, "conflate_pending_count");
    ASSERT(cb.peak_pending() == 2, "conflate_peak_2");   // #515: two distinct keys piled up
    double v = 0.0;
    ASSERT(cb.get(1, v) && std::fabs(v - 102.0) < 1e-9, "conflate_latest_value");
    ASSERT(cb.get(2, v) && std::fabs(v - 50.0) < 1e-9, "conflate_other_key");
    ASSERT(!cb.get(99, v), "conflate_unknown_false");
    cb.drain();
    ASSERT(cb.pending() == 0, "conflate_drain");
    // #515: the peak deliberately SURVIVES drain — pending() collapsed to 0 but
    // the worst backlog (2) is still remembered for buffer sizing.
    ASSERT(cb.peak_pending() == 2, "conflate_peak_survives_drain");

    // #427 received / conflation_ratio — how summarized the consumer's view is.
    // cb saw 4 updates, 2 of which were overwrites -> ratio 0.5.
    ASSERT(cb.received == 4, "conflate_received_counts");
    ASSERT(std::fabs(cb.conflation_ratio() - 0.5) < 1e-9, "conflate_ratio_half");
    // drain() clears the STATE, not the session counters.
    ASSERT(cb.conflated == 2 && std::fabs(cb.conflation_ratio() - 0.5) < 1e-9,
           "conflate_drain_keeps_counters");
    // A fresh buffer with distinct keys only: the consumer sees every tick.
    multicast::ConflationBuffer cbr;
    ASSERT(cbr.conflation_ratio() == 0.0, "conflate_ratio_empty_zero");
    ASSERT(cbr.peak_pending() == 0, "conflate_peak_empty_zero");   // #515
    cbr.update(1, 1.0); cbr.update(2, 2.0); cbr.update(3, 3.0);
    ASSERT(cbr.conflation_ratio() == 0.0 && cbr.received == 3, "conflate_ratio_no_overwrites");
    ASSERT(cbr.peak_pending() == 3, "conflate_peak_distinct_3");   // #515: three distinct keys
    // A hot key drives the ratio up: 1 insert + 3 overwrites of 4 -> 0.75.
    multicast::ConflationBuffer cbh;
    cbh.update(7, 1.0); cbh.update(7, 2.0); cbh.update(7, 3.0); cbh.update(7, 4.0);
    ASSERT(std::fabs(cbh.conflation_ratio() - 0.75) < 1e-9, "conflate_ratio_hot_key");
    // #515: conflated overwrites of ONE key never inflate the backlog -> peak 1.
    ASSERT(cbh.peak_pending() == 1, "conflate_peak_hot_key_1");

    // #235 LatencyTracker — EWMA + peak latency.
    multicast::LatencyTracker lt(0.5);
    lt.sample(100);                  // ewma 100, max 100
    ASSERT(std::fabs(lt.avg_ns() - 100.0) < 1e-9 && lt.peak_ns() == 100, "lat_first");
    lt.sample(200);                  // ewma 0.5*200+0.5*100 = 150, max 200
    ASSERT(std::fabs(lt.avg_ns() - 150.0) < 1e-9 && lt.peak_ns() == 200, "lat_blend_peak");
    lt.sample(50);                   // ewma 0.5*50+0.5*150 = 100, max 200 (peak remains)
    ASSERT(std::fabs(lt.avg_ns() - 100.0) < 1e-9 && lt.peak_ns() == 200, "lat_peak_retained");
    ASSERT(lt.count == 3, "lat_count");
    // #483 min_ns / jitter_ns — the floor and the full spread.
    // samples 100/200/50 -> min 50, max 200, jitter 150.
    ASSERT(lt.min_ns() == 50, "lat_min_50");
    ASSERT(lt.jitter_ns() == 150, "lat_jitter_150");
    // A fresh tracker: 0/0 until a sample; the first sample floors and caps
    // at that value (jitter 0).
    multicast::LatencyTracker lt0(0.5);
    ASSERT(lt0.min_ns() == 0 && lt0.jitter_ns() == 0, "lat_fresh_zero");
    lt0.sample(75);
    ASSERT(lt0.min_ns() == 75 && lt0.peak_ns() == 75 && lt0.jitter_ns() == 0, "lat_single_sample");
    lt.reset();
    ASSERT(lt.min_ns() == 0 && lt.jitter_ns() == 0 && lt.peak_ns() == 0, "lat_reset");

    // #243 ContiguousTracker — cumulative-ack watermark.
    multicast::ContiguousTracker ct;            // next_expected 1
    ct.receive(1);
    ASSERT(ct.contiguous_high() == 1, "contig_first");
    ct.receive(3);                              // gap: missing 2 -> buffer 3
    ASSERT(ct.contiguous_high() == 1 && ct.buffered() == 1, "contig_gap_buffered");
    ct.receive(2);                              // fills the gap -> pulls in 2 and 3
    ASSERT(ct.contiguous_high() == 3 && ct.buffered() == 0, "contig_filled_pulls_ahead");
    ct.receive(1);                              // duplicate -> ignore
    ASSERT(ct.contiguous_high() == 3, "contig_duplicate_ignored");

    // #354 max_lookahead — high-water mark of how far ahead of the gap buffering reached.
    // seq 3 arrived earlier while next_expected was 2 (after receive(1)) -> distance 1.
    ASSERT(ct.max_lookahead() == 1, "contig_max_lookahead_1");
    ct.receive(10);                             // next_expected is 4 -> distance 6, new high
    ASSERT(ct.max_lookahead() == 6, "contig_max_lookahead_grows");
    ct.receive(5);                              // distance 1 -> does not lower the high-water mark
    ASSERT(ct.max_lookahead() == 6, "contig_max_lookahead_stays_high");
    // #499 current_lookahead — the live span, next_expected 4, furthest 10 -> 6.
    ASSERT(ct.current_lookahead() == 6, "contig_cur_lookahead_6");
    // Filling the gap partially DROPS the live span while the peak stays.
    ct.receive(4);                              // next_expected 4 -> pulls 4,5 -> 6; ahead {10}
    ASSERT(ct.current_lookahead() == 4 && ct.max_lookahead() == 6, "contig_cur_drops_peak_holds");
    ct.receive(6); ct.receive(7); ct.receive(8); ct.receive(9);  // fill up to 10 -> drains
    ASSERT(ct.buffered() == 0 && ct.current_lookahead() == 0, "contig_cur_zero_when_drained");
    ct.reset();
    ASSERT(ct.max_lookahead() == 0 && ct.current_lookahead() == 0, "contig_max_lookahead_reset");

    // #257 SlidingWindowRate — count within a moving window (1000 ns).
    multicast::SlidingWindowRate sw(1000);
    sw.on_event(0); sw.on_event(500); sw.on_event(900);
    ASSERT(sw.count() == 3, "swr_all_in_window");          // all within [-100, 900]
    ASSERT(sw.peak_count() == 3, "swr_peak_3");            // #507: high-water = 3
    sw.on_event(1500);                                     // prunes <= 500 (0 and 500)
    ASSERT(sw.count() == 2, "swr_pruned_old");             // keeps 900, 1500
    ASSERT(sw.peak_count() == 3, "swr_peak_holds");        // #507: burst peak survives prune
    ASSERT(std::fabs(sw.peak_rate_per_sec() - 3.0 * 1e9 / 1000.0) < 1e-3, "swr_peak_rate");
    ASSERT(std::fabs(sw.rate_per_sec() - 2.0 * 1e9 / 1000.0) < 1e-3, "swr_rate");
    sw.reset();
    ASSERT(sw.count() == 0 && sw.peak_count() == 0, "swr_reset_clears_peak");   // #507
    // ring wrap: 10k events 1 ns apart wrap the 4096 ring; 1000 ns window keeps 1000.
    multicast::SlidingWindowRate swr(1000);
    for (int i = 0; i < 10000; ++i) swr.on_event(i);
    ASSERT(swr.count() == 1000, "swr_wrap_window_1000");

    // #265 RetransmitTracker — timeout / retry / escalate lifecycle.
    multicast::RetransmitTracker rt(1000, 3);   // 1000 ns timeout, max 3 attempts
    rt.request(5, 0);                            // attempt 1 at t=0
    ASSERT(rt.outstanding() == 1, "rtx_outstanding");
    ASSERT(rt.poll(500) == 0, "rtx_not_timed_out");      // 500 < 1000
    ASSERT(rt.poll(1000) == 1, "rtx_retry_2");           // timeout -> attempt 2
    ASSERT(rt.poll(2000) == 1, "rtx_retry_3");           // attempt 3
    ASSERT(rt.poll(3000) == 0 && rt.escalated == 1 && rt.outstanding() == 0,
           "rtx_escalate");                              // exhausted -> snapshot
    multicast::RetransmitTracker rt2(1000, 3);
    rt2.request(7, 0);
    rt2.on_received(7);                                   // retransmit arrived
    ASSERT(rt2.fulfilled == 1 && rt2.outstanding() == 0, "rtx_fulfilled");

    // #419 escalation_rate / max_attempts_in_flight — recovery-path health.
    multicast::RetransmitTracker rth(1000, 3);
    ASSERT(rth.escalation_rate() == 0.0 && rth.max_attempts_in_flight() == 0,
           "rtx_health_fresh");
    rth.request(1, 0);
    rth.request(2, 0);
    ASSERT(rth.max_attempts_in_flight() == 1, "rtx_health_first_attempts");
    rth.on_received(1);                                   // one resolves cleanly
    ASSERT(rth.escalation_rate() == 0.0, "rtx_health_fulfilled_keeps_zero");
    rth.poll(1000);                                       // seq 2 -> attempt 2
    rth.poll(2000);                                       // seq 2 -> attempt 3 (== max)
    ASSERT(rth.max_attempts_in_flight() == 3, "rtx_health_worst_at_the_edge");
    rth.poll(3000);                                       // exhausted -> escalates
    ASSERT(rth.outstanding() == 0 && rth.max_attempts_in_flight() == 0,
           "rtx_health_escalated_leaves_flight");
    // Resolved: 1 fulfilled + 1 escalated -> the snapshot road took half.
    ASSERT(std::fabs(rth.escalation_rate() - 0.5) < 1e-9, "rtx_health_rate_half");
    rth.reset();
    ASSERT(rth.escalation_rate() == 0.0, "rtx_health_reset");

    // #273 DualFeedReconciler — A/B first-seen dedup + line-win stats.
    multicast::DualFeedReconciler dfr;
    ASSERT(dfr.on_packet(1, 0), "dfr_A_first");           // A delivers seq 1 first
    ASSERT(!dfr.on_packet(1, 1), "dfr_B_dup");            // B's copy is a duplicate
    ASSERT(dfr.on_packet(2, 1), "dfr_B_first");           // B delivers seq 2 first
    ASSERT(!dfr.on_packet(2, 0), "dfr_A_dup");            // A's copy is a duplicate
    ASSERT(dfr.on_packet(3, 0), "dfr_A_first_2");         // A delivers seq 3
    ASSERT(dfr.a_wins == 2 && dfr.b_wins == 1 && dfr.duplicates == 2, "dfr_counts");
    ASSERT(std::fabs(dfr.a_win_rate() - 2.0/3.0) < 1e-9, "dfr_a_win_rate");

    // #281 SnapshotSyncBuffer — snapshot + incremental join.
    multicast::SnapshotSyncBuffer ssb;
    ASSERT(!ssb.on_increment(5), "ssb_buffer_5");        // before snapshot -> buffer
    ASSERT(!ssb.on_increment(6), "ssb_buffer_6");
    ASSERT(!ssb.on_increment(7), "ssb_buffer_7");
    ASSERT(ssb.pending_replay() == 3, "ssb_buffered_three");
    ASSERT(ssb.apply_snapshot(5) == 2, "ssb_replay_two");  // snapshot@5 drops 5, replay 6,7
    ASSERT(ssb.dropped == 1 && ssb.pending_replay() == 2, "ssb_dropped_one");
    ASSERT(ssb.on_increment(8), "ssb_live_apply");        // now live -> apply directly

    // #459 total_buffered / snapshot_coverage — recovery-snapshot freshness.
    // 3 buffered, snapshot covered 1, 2 replay -> coverage 1/3.
    ASSERT(ssb.total_buffered() == 3, "ssb_total_buffered_three");
    ASSERT(std::fabs(ssb.snapshot_coverage() - 1.0 / 3.0) < 1e-9, "ssb_coverage_one_third");
    // A FRESH snapshot that lands past everything buffered -> coverage 1.0.
    multicast::SnapshotSyncBuffer ssf;
    ASSERT(ssf.snapshot_coverage() == 0.0 && ssf.total_buffered() == 0, "ssb_fresh_zero");
    ssf.on_increment(10); ssf.on_increment(11); ssf.on_increment(12);
    // Before the snapshot lands, coverage is 0 (nothing dropped yet).
    ASSERT(ssf.snapshot_coverage() == 0.0 && ssf.total_buffered() == 3, "ssb_pre_apply_zero");
    ASSERT(ssf.apply_snapshot(99) == 0, "ssf_all_covered");   // snapshot@99 covers all 3
    ASSERT(std::fabs(ssf.snapshot_coverage() - 1.0) < 1e-9, "ssb_fresh_full_coverage");
    // A LAGGING snapshot that covers none -> coverage 0.0, all replay.
    multicast::SnapshotSyncBuffer ssl;
    ssl.on_increment(50); ssl.on_increment(51);
    ASSERT(ssl.apply_snapshot(49) == 2, "ssl_none_covered");  // snapshot@49 covers nothing
    ASSERT(ssl.snapshot_coverage() == 0.0 && ssl.total_buffered() == 2, "ssb_lagging_zero_coverage");
    ssl.reset();
    ASSERT(ssl.snapshot_coverage() == 0.0 && ssl.total_buffered() == 0, "ssb_coverage_reset");

    // #289 FeedHealth — composite 0-100 score from loss/reorder/staleness.
    multicast::FeedHealth fh;                             // defaults loss 200, ooo 100, stale 50, min 70
    ASSERT(std::fabs(fh.score(0.0, 0.0, false) - 100.0) < 1e-9 && fh.is_healthy(0.0, 0.0, false),
           "fh_perfect");
    ASSERT(std::fabs(fh.score(0.05, 0.0, false) - 90.0) < 1e-9, "fh_5pct_loss"); // 100 - 0.05*200
    // 10% loss + 10% ooo -> 100 - 20 - 10 = 70 (exactly the threshold -> healthy)
    ASSERT(std::fabs(fh.score(0.10, 0.10, false) - 70.0) < 1e-9 && fh.is_healthy(0.10, 0.10, false),
           "fh_threshold");
    ASSERT(std::fabs(fh.score(0.0, 0.0, true) - 50.0) < 1e-9 && !fh.is_healthy(0.0, 0.0, true),
           "fh_stale_unhealthy");
    ASSERT(std::fabs(fh.score(0.6, 0.0, false) - 0.0) < 1e-9, "fh_clamped_zero"); // 100 - 120 -> 0

    // #297 GapFillTimer — recovery-SLA timing.
    multicast::GapFillTimer gft;
    gft.record(1000, 1050);   // 50 ms
    gft.record(2000, 2150);   // 150 ms
    gft.record(3000, 3010);   // 10 ms
    ASSERT(gft.gaps == 3 && gft.max_recovery_ms == 150, "gft_count_max");
    ASSERT(std::fabs(gft.avg_recovery_ms() - 70.0) < 1e-9, "gft_avg");  // (50+150+10)/3
    gft.record(5000, 4000);   // negative -> ignored
    ASSERT(gft.gaps == 3, "gft_negative_ignored");

    // #435 min_recovery_ms / recovery_jitter_ms — the envelope's floor.
    ASSERT(gft.min_recovery_ms() == 10, "gft_min_10");
    ASSERT(gft.recovery_jitter_ms() == 140, "gft_jitter_150_minus_10");
    // A slower fill never lowers the floor; a faster one does.
    gft.record(6000, 6200);   // 200 ms
    ASSERT(gft.min_recovery_ms() == 10, "gft_min_survives_slow");
    gft.record(7000, 7005);   // 5 ms — a new best case
    ASSERT(gft.min_recovery_ms() == 5 && gft.recovery_jitter_ms() == 195, "gft_new_floor");
    // Fresh timer: 0/0 until the first recovery; an instant fill floors at 0.
    multicast::GapFillTimer gfz;
    ASSERT(gfz.min_recovery_ms() == 0 && gfz.recovery_jitter_ms() == 0, "gft_fresh_zero");
    gfz.record(100, 100);     // 0 ms fill — valid, and the floor is truly 0
    ASSERT(gfz.gaps == 1 && gfz.min_recovery_ms() == 0, "gft_instant_fill_floor");
    gfz.reset();
    ASSERT(gfz.min_recovery_ms() == 0 && gfz.gaps == 0, "gft_reset");

    // #305 InterArrivalStats — feed jitter envelope.
    multicast::InterArrivalStats ias;
    ias.on_message(1000);     // first -> no gap
    ias.on_message(1050);     // gap 50
    ias.on_message(1060);     // gap 10
    ias.on_message(1200);     // gap 140
    ASSERT(ias.count == 3 && ias.min_gap == 10 && ias.max_gap == 140, "ias_min_max");
    ASSERT(std::fabs(ias.mean_gap() - (200.0 / 3.0)) < 1e-9, "ias_mean");  // (50+10+140)/3
    ASSERT(ias.jitter() == 130, "ias_jitter");                            // 140 - 10
    // #362 last_gap — the most recent gap (the 140 above), a live cadence probe.
    ASSERT(ias.last_gap() == 140, "ias_last_gap");
    ias.on_message(1205);                                                 // gap 5
    ASSERT(ias.last_gap() == 5, "ias_last_gap_updates");
    multicast::InterArrivalStats ias0;
    ASSERT(ias0.last_gap() == 0, "ias_last_gap_before_second_msg");
    ias0.on_message(500);
    ASSERT(ias0.last_gap() == 0, "ias_last_gap_after_first_msg");

    // #313 PacketStats — wire-level packet/byte accounting.
    multicast::PacketStats ps;
    ps.on_packet(100); ps.on_packet(300); ps.on_packet(200);
    ASSERT(ps.packets == 3 && ps.total_bytes == 600, "ps_count_bytes");
    ASSERT(ps.max_bytes == 300, "ps_max");
    ASSERT(std::fabs(ps.mean_bytes() - 200.0) < 1e-9, "ps_mean");
    // #346 min_bytes / byte_range: smallest packet + spread (300 - 100 = 200).
    ASSERT(ps.min_bytes == 100, "ps_min");
    ASSERT(ps.byte_range() == 200, "ps_range");
    ps.reset();
    ASSERT(ps.packets == 0 && ps.mean_bytes() == 0.0, "ps_reset");
    ASSERT(ps.min_bytes == 0 && ps.byte_range() == 0, "ps_reset_min_range");

    // #142 InterArrivalMeter — min/max/avg/jitter of the gaps.
    multicast::InterArrivalMeter im;
    im.on_message(0); im.on_message(100); im.on_message(150); im.on_message(400);
    ASSERT(im.min_gap_ns() == 50, "iam_min_50");          // gaps: 100,50,250
    ASSERT(im.max_gap_ns() == 250, "iam_max_250");
    ASSERT(im.jitter_ns() == 200, "iam_jitter_200");      // 250-50
    ASSERT(std::fabs(im.avg_gap_ns() - 400.0/3.0) < 1e-6, "iam_avg");
    // #523 burst_ratio — worst gap over the average (scale-free): 250/(400/3) = 1.875.
    ASSERT(std::fabs(im.burst_ratio() - 250.0 / (400.0 / 3.0)) < 1e-6, "iam_burst_ratio");
    multicast::InterArrivalMeter imf;
    ASSERT(imf.burst_ratio() == 0.0, "iam_burst_empty_zero");
    // A perfectly even feed -> every gap equals the mean -> ratio exactly 1.0.
    multicast::InterArrivalMeter ime;
    ime.on_message(0); ime.on_message(100); ime.on_message(200); ime.on_message(300);
    ASSERT(std::fabs(ime.burst_ratio() - 1.0) < 1e-9, "iam_burst_even_one");
    // A single long stall among tight gaps -> gaps 100,100,1000 -> avg 400, max
    // 1000 -> ratio 2.5, well above the even feed's 1.0 (scale-free burst flag).
    multicast::InterArrivalMeter imb;
    imb.on_message(0); imb.on_message(100); imb.on_message(200); imb.on_message(1200);
    ASSERT(std::fabs(imb.burst_ratio() - 2.5) < 1e-9 && imb.burst_ratio() > ime.burst_ratio(),
           "iam_burst_bursty_2p5");

    // #91 A/B line arbitration — the first line wins, the second is deduped; B patches A's gap.
    multicast::ABLineArbitrator arb;
    ASSERT(arb.on_packet(1, true),  "ab_a1_new");
    ASSERT(!arb.on_packet(1, false), "ab_b1_dup");        // B's 1 = duplicate
    ASSERT(arb.on_packet(2, true),  "ab_a2_new");
    ASSERT(arb.on_packet(4, true),  "ab_a4_gap");          // A jumped → missing 3
    ASSERT(arb.has_gaps() && arb.missing_count() == 1, "ab_gap_3_pending");
    ASSERT(arb.on_packet(3, false), "ab_b3_fills_gap");    // B dostarcza 3
    ASSERT(!arb.has_gaps(), "ab_self_healed");
    ASSERT(!arb.on_packet(4, false), "ab_b4_dup");
    ASSERT(arb.a_first == 3 && arb.b_first == 1, "ab_first_counts");

    // #387 A/B line health: win rate, lagging-line alarm, duplicate rate.
    multicast::ABLineArbitrator abh;
    ASSERT(std::fabs(abh.a_win_rate() - 0.5) < 1e-9 && abh.lagging_line() == '-'
           && abh.dup_rate() == 0.0, "ab_health_neutral_start");
    // A consistently first, B delivering the duplicate: A wins every race,
    // B lags but is ALIVE (dup_rate ~0.5 proves it still delivers).
    for (int abi = 1; abi <= 10; ++abi) {
        abh.on_packet(abi, true);
        abh.on_packet(abi, false);
    }
    ASSERT(std::fabs(abh.a_win_rate() - 1.0) < 1e-9, "ab_health_a_wins_all");
    ASSERT(abh.lagging_line() == 'B', "ab_health_b_lagging");
    ASSERT(std::fabs(abh.dup_rate() - 0.5) < 1e-9, "ab_health_b_alive_dups_half");
    // Balanced racing: wins split evenly -> no alarm.
    multicast::ABLineArbitrator abb;
    for (int abj = 1; abj <= 10; ++abj) {
        const bool a_now = (abj % 2) == 0;
        abb.on_packet(abj, a_now);
        abb.on_packet(abj, !a_now);
    }
    ASSERT(std::fabs(abb.a_win_rate() - 0.5) < 1e-9 && abb.lagging_line() == '-',
           "ab_health_balanced_no_alarm");
    // DEAD line B: A wins everything AND no duplicate ever arrives.
    multicast::ABLineArbitrator abd;
    for (int abk = 1; abk <= 10; ++abk) abd.on_packet(abk, true);
    ASSERT(abd.lagging_line() == 'B' && abd.dup_rate() == 0.0, "ab_health_dead_line_no_dups");
    // Threshold: 3 wins of 10 (rate 0.3) lags under 0.35 but not under 0.1.
    multicast::ABLineArbitrator abt;
    for (int abm = 1; abm <= 10; ++abm) abt.on_packet(abm, abm <= 3);
    ASSERT(abt.lagging_line(0.35) == 'A' && abt.lagging_line(0.1) == '-', "ab_health_threshold");

    // #98 staleness: no packet > timeout = dead feed.
    multicast::FeedStalenessMonitor sm;
    ASSERT(!sm.check(1000, 500), "stale_not_started");   // no first packet
    sm.on_packet(1000);
    ASSERT(!sm.check(1400, 500), "stale_fresh");          // 400 <= 500
    ASSERT(sm.check(1600, 500), "stale_after_timeout");   // 600 > 500
    ASSERT(sm.is_stale() && sm.stale_events == 1, "stale_event_counted");
    sm.on_packet(1700);
    ASSERT(!sm.check(1800, 500), "stale_recovered");

    // #443 outage durations — how LONG the feed died, not just how often.
    // sm's revival at 1700 came from a stale spell whose last packet was at
    // 1000 -> a completed 700ns outage.
    ASSERT(sm.longest_outage_ns() == 700 && sm.total_outage_ns() == 700,
           "stale_outage_recorded_from_last_packet");
    // A second, shorter outage adds to the total but not the maximum.
    ASSERT(sm.check(2300, 500), "stale_second_spell");        // 600 > 500
    sm.on_packet(2350);                                        // outage 650
    ASSERT(sm.longest_outage_ns() == 700, "stale_longest_retained");
    ASSERT(sm.total_outage_ns() == 1350, "stale_total_accumulates");
    // A healthy inter-packet gap (never detected stale) records nothing.
    sm.on_packet(2400);
    ASSERT(sm.total_outage_ns() == 1350, "stale_healthy_gap_not_outage");
    sm.reset();
    ASSERT(sm.longest_outage_ns() == 0 && sm.total_outage_ns() == 0, "stale_outage_reset");

    // #321 GapRecovery::duplicate_rate / primary_packets
    multicast::GapRecovery grdup;
    grdup.observe(1); grdup.observe(2); grdup.observe(3);  // 3 in-order
    grdup.observe(1); grdup.observe(2);                     // 2 duplicates (seq < expected, not missing)
    grdup.observe(7);                                       // gap: 4,5,6 missing
    // primary_packets = 6 observes total; duplicates = 2
    ASSERT(grdup.primary_packets == 6, "grdup_primary_6");
    ASSERT(grdup.duplicates == 2, "grdup_dup_count");
    ASSERT(std::fabs(grdup.duplicate_rate() - 2.0/6.0) < 1e-9, "grdup_dup_rate");
    // fresh tracker -> rate = 0
    multicast::GapRecovery grfresh;
    ASSERT(std::fabs(grfresh.duplicate_rate()) < 1e-12, "grdup_fresh_dup");
}

// Momentum #85 — trend-following; the decision sign is opposite to mean-reversion.
void test_momentum() {
    SECTION("Momentum Strategy (#85)");
    MomentumStrategy m(/*window=*/3, /*threshold_pct=*/0.1, /*order_size=*/100);
    m.on_market_data("AAPL", 100.0);
    m.on_market_data("AAPL", 100.0);
    m.on_market_data("AAPL", 100.0);                 // window full, SMA=100
    const Signal up = m.on_market_data("AAPL", 105.0);
    ASSERT(up.valid && up.side == Side::BUY, "momentum_breakout_buys");

    MomentumStrategy md(3, 0.1, 100);
    md.on_market_data("X", 100.0); md.on_market_data("X", 100.0); md.on_market_data("X", 100.0);
    const Signal dn = md.on_market_data("X", 95.0);
    ASSERT(dn.valid && dn.side == Side::SELL, "momentum_breakdown_sells");

    // Kontrast: mean-reversion na tym samym dolku gra PRZECIWNIE (BUY).
    MeanReversionStrategy mr(3, 0.1, 100);
    mr.on_market_data("X", 100.0); mr.on_market_data("X", 100.0); mr.on_market_data("X", 100.0);
    const Signal mrdn = mr.on_market_data("X", 95.0);
    ASSERT(mrdn.valid && mrdn.side == Side::BUY, "meanrev_opposite_side");
}

// Donchian #124 — channel breakout (breaking the N-period max/min).
void test_donchian() {
    SECTION("Donchian Breakout (#124)");
    DonchianBreakout up(3, 100);                  // channel from the previous 3
    up.on_market_data("X", 100.0);
    up.on_market_data("X", 101.0);
    up.on_market_data("X", 99.0);                 // window full: hi=101, lo=99
    const Signal su = up.on_market_data("X", 102.0);
    ASSERT(su.valid && su.side == Side::BUY, "donchian_break_high_buys");

    DonchianBreakout dn(3, 100);
    dn.on_market_data("Y", 100.0);
    dn.on_market_data("Y", 101.0);
    dn.on_market_data("Y", 99.0);
    const Signal sd = dn.on_market_data("Y", 98.0);
    ASSERT(sd.valid && sd.side == Side::SELL, "donchian_break_low_sells");

    DonchianBreakout fl(3, 100);
    fl.on_market_data("Z", 100.0);
    fl.on_market_data("Z", 101.0);
    fl.on_market_data("Z", 99.0);
    const Signal sf = fl.on_market_data("Z", 100.0);  // inside [99,101]
    ASSERT(!sf.valid, "donchian_inside_channel_holds");
}

// RSI #135 — oscylator pedu; rosnaca seria -> RSI 100 -> SELL, malejaca -> BUY.
void test_rsi() {
    SECTION("RSI Strategy (#135)");
    RSIStrategy up(3, 70.0, 30.0, 100);
    up.on_market_data("X", 100.0);                // baseline
    up.on_market_data("X", 101.0);
    up.on_market_data("X", 102.0);
    const Signal su = up.on_market_data("X", 103.0);  // window full: all gains -> RSI=100
    ASSERT(su.valid && su.side == Side::SELL, "rsi_overbought_sells");

    RSIStrategy dn(3, 70.0, 30.0, 100);
    dn.on_market_data("Y", 100.0);
    dn.on_market_data("Y", 99.0);
    dn.on_market_data("Y", 98.0);
    const Signal sd = dn.on_market_data("Y", 97.0);   // all losses -> RSI=0
    ASSERT(sd.valid && sd.side == Side::BUY, "rsi_oversold_buys");
}

// MA Crossover #157 — golden/death cross (przeciecie szybkiej i wolnej SMA).
void test_ma_crossover() {
    SECTION("MA Crossover (#157)");
    MACrossover x(2, 3, 100);                          // fast=2, slow=3
    x.on_market_data("X", 100.0);
    x.on_market_data("X", 100.0);
    x.on_market_data("X", 100.0);                      // setup, fast==slow, no signal
    const Signal up = x.on_market_data("X", 110.0);    // fast 105 > slow 103.3 -> golden cross
    ASSERT(up.valid && up.side == Side::BUY, "macross_golden_buys");
    const Signal dn = x.on_market_data("X", 90.0);     // fast 100 == slow 100 -> not above -> death cross
    ASSERT(dn.valid && dn.side == Side::SELL, "macross_death_sells");
}

// Volatility #165 — rolling return volatility + vol-targeting sizing.
void test_volatility() {
    SECTION("Volatility Estimator (#165)");
    VolatilityEstimator c(4);
    for (int i = 0; i < 5; ++i) c.on_price(100.0);          // constant price -> zero vol
    ASSERT(c.volatility() == 0.0, "vol_constant_zero");

    VolatilityEstimator v(4);
    v.on_price(100.0); v.on_price(110.0); v.on_price(100.0); v.on_price(110.0); v.on_price(100.0);
    ASSERT(v.volatility() > 0.0, "vol_varying_positive");
    ASSERT(v.samples() == 4, "vol_samples");
    // vol-targeting: high volatility (~10%) vs 1% target -> smaller position
    ASSERT(v.target_size(1000, 0.01) < 1000, "vol_target_size_smaller");
    // no volatility -> base_size
    ASSERT(c.target_size(1000, 0.01) == 1000, "vol_target_size_base_when_calm");
}

// EMA #173 — exponential moving average.
void test_ema() {
    SECTION("EMA (#173)");
    EMA e(0.5);
    ASSERT(std::fabs(e.update(100.0) - 100.0) < 1e-9, "ema_first_is_seed");   // pierwsza = seed
    ASSERT(std::fabs(e.update(200.0) - 150.0) < 1e-9, "ema_blend");           // 0.5*200 + 0.5*100
    ASSERT(e.ready(), "ema_ready");
    // alpha z okresu: 9-okresowa -> 2/10 = 0.2
    EMA p = EMA::from_period(9);
    ASSERT(std::fabs(p.alpha() - 0.2) < 1e-9, "ema_from_period_alpha");
    p.reset();
    ASSERT(!p.ready(), "ema_reset");
}

// MACD #182 — momentum from three EMAs.
void test_macd() {
    SECTION("MACD (#182)");
    MACD m;                                   // 12/26/9
    for (int i = 1; i <= 60; ++i) m.update(100.0 + i);   // monotonic rise
    ASSERT(m.ready(), "macd_ready");
    ASSERT(m.macd() > 0.0, "macd_positive_on_uptrend");   // fast EMA nad slow
    ASSERT(m.bullish(), "macd_bullish_on_uptrend");
    ASSERT(std::fabs(m.histogram() - (m.macd() - m.signal())) < 1e-9, "macd_histogram_def");
    MACD d;
    for (int i = 1; i <= 60; ++i) d.update(200.0 - i);   // monotonic drop
    ASSERT(d.macd() < 0.0, "macd_negative_on_downtrend");
    ASSERT(!d.bullish(), "macd_not_bullish_on_downtrend");
}

// Stochastic #190 — oscylator %K.
void test_stochastic() {
    SECTION("Stochastic (#190)");
    Stochastic up(5);
    for (int i = 1; i <= 5; ++i) up.update(i);            // 1..5, cur=5 = peak
    ASSERT(up.ready(), "stoch_ready");
    ASSERT(std::fabs(up.percent_k() - 100.0) < 1e-9, "stoch_k_top_100");
    ASSERT(up.overbought(), "stoch_overbought");
    Stochastic dn(5);
    for (int i = 0; i < 5; ++i) dn.update(5.0 - i);       // 5,4,3,2,1, cur=1 = trough
    ASSERT(std::fabs(dn.percent_k() - 0.0) < 1e-9, "stoch_k_bottom_0");
    ASSERT(dn.oversold(), "stoch_oversold");
    Stochastic mid(3);
    mid.update(30); mid.update(10); mid.update(20);       // lo10 hi30 cur20 -> 50
    ASSERT(std::fabs(mid.percent_k() - 50.0) < 1e-9, "stoch_k_mid_50");
    Stochastic flat(3);
    flat.update(100); flat.update(100);                   // plaskie -> 50
    ASSERT(std::fabs(flat.percent_k() - 50.0) < 1e-9, "stoch_k_flat_neutral");
}

// WMA #198 — linearly weighted moving average.
void test_wma() {
    SECTION("WMA (#198)");
    WMA w(3);
    w.update(1.0); w.update(2.0); w.update(3.0);          // (1*1+2*2+3*3)/(1+2+3)=14/6
    ASSERT(w.ready(), "wma_ready");
    ASSERT(std::fabs(w.value() - 14.0/6.0) < 1e-9, "wma_weighted");
    // window shift: 4 comes in, 1 drops out -> (2*1+3*2+4*3)/6 = 20/6
    w.update(4.0);
    ASSERT(std::fabs(w.value() - 20.0/6.0) < 1e-9, "wma_window_slide");
    WMA s(1);
    s.update(42.0);
    ASSERT(std::fabs(s.value() - 42.0) < 1e-9, "wma_period1_is_last");
}

// HullMA #206 — a low-lag average built on WMA.
void test_hull_ma() {
    SECTION("HullMA (#206)");
    HullMA c(9);
    for (int i = 0; i < 30; ++i) c.update(50.0);          // stala -> HMA = stala
    ASSERT(c.ready(), "hma_ready");
    ASSERT(std::fabs(c.value() - 50.0) < 1e-9, "hma_constant");
    // rising trend -> HMA rises and (low lag) leads the window's simple average
    HullMA r(9);
    for (int i = 1; i <= 29; ++i) r.update(static_cast<double>(i));
    const double prev = r.value();
    r.update(30.0);
    ASSERT(r.value() > prev, "hma_rises_on_uptrend");
    ASSERT(r.value() > 15.5, "hma_low_lag_above_mean");  // average 1..30 = 15.5
}

// DEMA #214 — podwojna EMA o niskiej zwloce.
void test_dema() {
    SECTION("DEMA (#214)");
    DEMA c(5);
    for (int i = 0; i < 25; ++i) c.update(50.0);          // stala -> 2*50 - 50 = 50
    ASSERT(c.ready(), "dema_ready");
    ASSERT(std::fabs(c.value() - 50.0) < 1e-9, "dema_constant");
    DEMA r(5);
    for (int i = 1; i <= 20; ++i) r.update(static_cast<double>(i));
    const double prev = r.value();
    r.update(21.0);
    ASSERT(r.value() > prev, "dema_rises_on_uptrend");
    ASSERT(r.value() > 15.0, "dema_low_lag");             // tracks fresh values (1..21 average ~11)
}

// TEMA #222 — triple EMA, the lowest lag.
void test_tema() {
    SECTION("TEMA (#222)");
    TEMA c(5);
    for (int i = 0; i < 30; ++i) c.update(50.0);          // stala -> 3*50-3*50+50 = 50
    ASSERT(c.ready(), "tema_ready");
    ASSERT(std::fabs(c.value() - 50.0) < 1e-9, "tema_constant");
    TEMA r(5);
    for (int i = 1; i <= 20; ++i) r.update(static_cast<double>(i));
    const double prev = r.value();
    r.update(21.0);
    ASSERT(r.value() > prev, "tema_rises_on_uptrend");
    ASSERT(r.value() > 15.0, "tema_low_lag");             // low lag on a ramp
}

// TRIX #230 — momentum oscillator from a triple EMA.
void test_trix() {
    SECTION("TRIX (#230)");
    TRIX c(5);
    for (int i = 0; i < 40; ++i) c.update(50.0);          // constant -> no change -> 0
    ASSERT(c.ready(), "trix_ready");
    ASSERT(std::fabs(c.value() - 0.0) < 1e-9, "trix_flat_zero");
    TRIX up(5);
    for (int i = 1; i <= 40; ++i) up.update(static_cast<double>(i));  // rosnaco
    ASSERT(up.value() > 0.0, "trix_positive_on_uptrend");
    TRIX dn(5);
    for (int i = 0; i < 30; ++i) dn.update(100.0 - i);   // decreasing (positive prices)
    ASSERT(dn.value() < 0.0, "trix_negative_on_downtrend");
}

// CCI #238 — Commodity Channel Index.
void test_cci() {
    SECTION("CCI (#238)");
    CCI flat(5);
    for (int i = 0; i < 6; ++i) flat.update(50.0);        // stala -> mad 0 -> 0
    ASSERT(flat.ready(), "cci_ready");
    ASSERT(std::fabs(flat.value() - 0.0) < 1e-9, "cci_flat_zero");
    CCI up(5);
    for (int i = 1; i <= 5; ++i) up.update(static_cast<double>(i));  // 1..5: CCI ~+111
    ASSERT(up.value() > 100.0 && up.overbought(), "cci_overbought_uptrend");
    CCI dn(5);
    for (int i = 0; i < 5; ++i) dn.update(5.0 - i);       // 5..1: CCI ~-111
    ASSERT(dn.value() < -100.0 && dn.oversold(), "cci_oversold_downtrend");
}

// Bollinger %B #246.
void test_bollinger_pctb() {
    SECTION("Bollinger %B (#246)");
    BollingerPercentB flat(5, 2.0);
    for (int i = 0; i < 6; ++i) flat.update(50.0);        // stala -> sd 0 -> 0.5
    ASSERT(flat.ready(), "pctb_ready");
    ASSERT(std::fabs(flat.value() - 0.5) < 1e-9, "pctb_flat_mid");
    BollingerPercentB up(5, 2.0);
    for (int i = 1; i <= 5; ++i) up.update(static_cast<double>(i));  // {1..5}
    // mean 3, sd=sqrt(2); %B = (5 - (3 - 2*sqrt2)) / (4*sqrt2)
    const double exp = (5.0 - (3.0 - 2.0 * std::sqrt(2.0))) / (4.0 * std::sqrt(2.0));
    ASSERT(std::fabs(up.value() - exp) < 1e-9, "pctb_known_series");
    ASSERT(up.value() > 0.5, "pctb_above_mean_on_uptrend");
}

// ROC #253 — Rate of Change momentum.
void test_roc() {
    SECTION("ROC (#253)");
    ROC r(3);
    r.update(100.0); r.update(101.0); r.update(102.0); r.update(110.0);  // 110 vs 100
    ASSERT(r.ready(), "roc_ready");
    ASSERT(std::fabs(r.value() - 10.0) < 1e-9, "roc_up_10pct");          // (110-100)/100*100
    ROC d(2);
    d.update(100.0); d.update(90.0); d.update(81.0);                      // 81 vs 100
    ASSERT(std::fabs(d.value() - (-19.0)) < 1e-9, "roc_down_neg19");
    ROC n(5);
    n.update(100.0);
    ASSERT(!n.ready() && n.value() == 0.0, "roc_not_ready_zero");
}

// Backtester analytics (#263) — Sortino, Calmar/recovery, Kelly, ulcer, VaR/CVaR.
void test_backtest() {
    SECTION("Backtester analytics (#263)");
    using backtest::Backtester;
    {   // core: hit-rate, profit factor, fill-rate, gross, payoff, kelly
        Backtester bt;
        bt.on_order(true); bt.on_trade(+100.0);
        bt.on_order(true); bt.on_trade(-40.0);
        bt.on_order(true); bt.on_trade(+60.0);
        bt.on_order(false);
        const auto r = bt.compute();
        ASSERT(r.trades == 3 && r.wins == 2 && r.losses == 1, "btc_counts");
        ASSERT(std::fabs(r.profit_factor - 4.0) < 1e-9, "btc_profit_factor");    // 160/40
        ASSERT(std::fabs(r.fill_rate - 0.75) < 1e-9, "btc_fill_rate");
        ASSERT(std::fabs(r.payoff_ratio - 2.0) < 1e-9, "btc_payoff");            // 80/40
        ASSERT(std::fabs(r.kelly_fraction - (2.0/3.0 - (1.0/3.0)/2.0)) < 1e-9, "btc_kelly");
    }
    {   // drawdown, recovery factor, dd duration
        Backtester bt;
        bt.on_trade(+100.0); bt.on_trade(-70.0); bt.on_trade(+50.0);
        const auto r = bt.compute();
        ASSERT(std::fabs(r.max_drawdown - 70.0) < 1e-9, "btc_max_dd");
        ASSERT(std::fabs(r.recovery_factor - 80.0/70.0) < 1e-9, "btc_recovery");
        ASSERT(bt.max_drawdown_duration() == 2, "btc_dd_duration");
    }
    {   // Sortino (downside-only) > Sharpe when down moves are few
        Backtester bt;
        bt.on_trade(+30.0); bt.on_trade(-10.0); bt.on_trade(+20.0); bt.on_trade(+40.0);
        const auto r = bt.compute();
        ASSERT(std::fabs(r.sortino - 8.0) < 1e-9, "btc_sortino");   // 20/5*2
        ASSERT(r.sortino > r.sharpe, "btc_sortino_gt_sharpe");
    }
    {   // VaR / CVaR on a known distribution
        Backtester bt;
        bt.on_trade(-50.0); bt.on_trade(-30.0); bt.on_trade(-10.0);
        bt.on_trade(+20.0); bt.on_trade(+100.0);
        ASSERT(std::fabs(bt.value_at_risk(0.8) - 50.0) < 1e-9, "btc_var_80");
        ASSERT(std::fabs(bt.conditional_value_at_risk(0.6) - 40.0) < 1e-9, "btc_cvar_60");
        ASSERT(bt.ulcer_index() > 0.0, "btc_ulcer_positive");
    }
    {   // per-tag attribution
        Backtester bt;
        bt.on_trade(+10.0, "AAPL"); bt.on_trade(-5.0, "MSFT"); bt.on_trade(+3.0, "AAPL");
        ASSERT(std::fabs(bt.pnl_for_tag("AAPL") - 13.0) < 1e-9, "btc_tag_aapl");
        ASSERT(std::fabs(bt.pnl_for_tag("MSFT") + 5.0) < 1e-9, "btc_tag_msft");
        ASSERT(bt.tag_count() == 2, "btc_tag_count");
    }
}

// Aroon #260 — trend-strength oscillator.
void test_aroon() {
    SECTION("Aroon (#260)");
    Aroon up(4);
    for (int i = 1; i <= 5; ++i) up.update(static_cast<double>(i));   // rising: high last, low first
    ASSERT(up.ready(), "aroon_ready");
    ASSERT(std::fabs(up.up() - 100.0) < 1e-9, "aroon_up_100_on_uptrend");
    ASSERT(std::fabs(up.down() - 0.0) < 1e-9, "aroon_down_0_on_uptrend");
    Aroon dn(4);
    for (int i = 0; i < 5; ++i) dn.update(5.0 - i);                   // falling: high first, low last
    ASSERT(std::fabs(dn.up() - 0.0) < 1e-9, "aroon_up_0_on_downtrend");
    ASSERT(std::fabs(dn.down() - 100.0) < 1e-9, "aroon_down_100_on_downtrend");
}

// CMO #268 — Chande Momentum Oscillator.
void test_cmo() {
    SECTION("CMO (#268)");
    CMO up(4);
    for (int i = 1; i <= 5; ++i) up.update(static_cast<double>(i));   // all up moves
    ASSERT(up.ready(), "cmo_ready");
    ASSERT(std::fabs(up.value() - 100.0) < 1e-9, "cmo_all_up_100");
    ASSERT(up.overbought(), "cmo_overbought");
    CMO dn(4);
    for (int i = 0; i < 5; ++i) dn.update(5.0 - i);                   // all down moves
    ASSERT(std::fabs(dn.value() - (-100.0)) < 1e-9, "cmo_all_down_neg100");
    ASSERT(dn.oversold(), "cmo_oversold");
    CMO mix(4);
    mix.update(100); mix.update(110); mix.update(105); mix.update(115); mix.update(110);
    // changes +10,-5,+10,-5 -> up=20 down=10 -> (20-10)/30*100 = 33.33
    ASSERT(std::fabs(mix.value() - 100.0/3.0) < 1e-9, "cmo_mixed");
}

// ZScore #276 — rolling standard-score of price.
void test_zscore() {
    SECTION("ZScore (#276)");
    ZScore c(5);
    for (int i = 0; i < 5; ++i) c.update(50.0);          // flat -> sd 0 -> 0
    ASSERT(c.ready(), "zscore_ready");
    ASSERT(std::fabs(c.value() - 0.0) < 1e-9, "zscore_flat_zero");
    ZScore z(5);
    for (int i = 1; i <= 5; ++i) z.update(static_cast<double>(i));   // {1..5}
    // mean 3, pop var ((4+1+0+1+4)/5)=2, sd=sqrt(2); z = (5-3)/sqrt(2) = sqrt(2)
    ASSERT(std::fabs(z.value() - std::sqrt(2.0)) < 1e-9, "zscore_known");
    ASSERT(z.value() > 0.0, "zscore_positive_above_mean");
}

// TSI #284 — True Strength Index (double-smoothed momentum).
void test_tsi() {
    SECTION("TSI (#284)");
    TSI up(3, 2);
    for (int i = 1; i <= 40; ++i) up.update(static_cast<double>(i));   // steady +1 momentum
    ASSERT(up.ready(), "tsi_ready");
    ASSERT(std::fabs(up.value() - 100.0) < 1e-6, "tsi_uptrend_100");
    TSI dn(3, 2);
    for (int i = 0; i < 40; ++i) dn.update(100.0 - i);                  // steady -1 momentum
    ASSERT(std::fabs(dn.value() - (-100.0)) < 1e-6, "tsi_downtrend_neg100");
}

// DPO #292 — Detrended Price Oscillator.
void test_dpo() {
    SECTION("DPO (#292)");
    DPO c(4);
    for (int i = 0; i < 4; ++i) c.update(50.0);          // flat -> price == SMA -> 0
    ASSERT(c.ready(), "dpo_ready");
    ASSERT(std::fabs(c.value() - 0.0) < 1e-9, "dpo_flat_zero");
    DPO d(4);
    d.update(1.0); d.update(2.0); d.update(3.0); d.update(4.0);   // window {1,2,3,4}
    // SMA = 2.5, shift = 3, price 3 ago = window[0] = 1 -> DPO = 1 - 2.5 = -1.5
    ASSERT(std::fabs(d.value() - (-1.5)) < 1e-9, "dpo_known");
}

// KAMA #300 — Kaufman Adaptive Moving Average (milestone).
void test_kama() {
    SECTION("KAMA (#300)");
    KAMA c(10);
    for (int i = 0; i < 15; ++i) c.update(50.0);     // flat: ER=0, but price==KAMA -> stays
    ASSERT(c.ready(), "kama_ready");
    ASSERT(std::fabs(c.value() - 50.0) < 1e-9, "kama_constant_stays");
    // Clean uptrend: ER ~ 1 -> fast smoothing -> KAMA tracks price closely but lags.
    KAMA t(10);
    for (int i = 1; i <= 19; ++i) t.update(static_cast<double>(i));
    const double v19 = t.value();
    t.update(20.0);
    const double v20 = t.value();
    ASSERT(v20 > v19, "kama_tracks_uptrend");                 // moves with the trend
    ASSERT(v20 > 15.0 && v20 < 20.0, "kama_fast_adapt_lag");  // fast adaptation, slight lag
}

// LinReg #308 — least-squares regression slope + LSMA endpoint.
void test_linreg() {
    SECTION("LinReg (#308)");
    LinReg up(14);
    for (int i = 1; i <= 14; ++i) up.update(static_cast<double>(i));   // perfect +1 ramp
    ASSERT(up.ready(), "linreg_ready");
    ASSERT(std::fabs(up.slope() - 1.0) < 1e-9, "linreg_slope_up");
    ASSERT(std::fabs(up.value() - 14.0) < 1e-9, "linreg_lsma_endpoint");  // line at last bar
    LinReg fl(5);
    for (int i = 0; i < 5; ++i) fl.update(50.0);                        // flat
    ASSERT(std::fabs(fl.slope() - 0.0) < 1e-9, "linreg_slope_flat");
    ASSERT(std::fabs(fl.value() - 50.0) < 1e-9, "linreg_value_flat");
    LinReg dn(5);
    const double seq[] = {5.0, 4.0, 3.0, 2.0, 1.0};                     // perfect -1 ramp
    for (double p : seq) dn.update(p);
    ASSERT(std::fabs(dn.slope() - (-1.0)) < 1e-9, "linreg_slope_down");
    ASSERT(std::fabs(dn.value() - 1.0) < 1e-9, "linreg_lsma_down");
}

// RollingStdDev #316 — rolling sample standard deviation (volatility primitive).
void test_rolling_stddev() {
    SECTION("RollingStdDev (#316)");
    RollingStdDev fl(4);
    for (int i = 0; i < 4; ++i) fl.update(5.0);                 // constant -> zero dispersion
    ASSERT(fl.ready(), "rsd_ready");
    ASSERT(std::fabs(fl.value() - 0.0) < 1e-9, "rsd_constant_zero");
    RollingStdDev sd(4);
    sd.update(2.0); sd.update(4.0); sd.update(4.0); sd.update(6.0);  // mean 4, dev^2 4+0+0+4=8
    ASSERT(std::fabs(sd.mean() - 4.0) < 1e-9, "rsd_mean");
    ASSERT(std::fabs(sd.variance() - (8.0 / 3.0)) < 1e-9, "rsd_sample_variance"); // 8/(4-1)
    ASSERT(std::fabs(sd.value() - std::sqrt(8.0 / 3.0)) < 1e-9, "rsd_sample_std");
}

// FisherTransform #324 — Ehlers' Fisher Transform oscillator.
void test_fisher() {
    SECTION("FisherTransform (#324)");
    FisherTransform warm(5);
    for (int i = 0; i < 4; ++i) warm.update(10.0 + i);
    ASSERT(!warm.ready(), "fisher_not_ready_before_period");
    warm.update(14.0);
    ASSERT(warm.ready(), "fisher_ready_at_period");

    // flat input -> range 0 every step -> position 0 -> Fisher stays exactly 0
    FisherTransform fl(5);
    for (int i = 0; i < 10; ++i) fl.update(42.0);
    ASSERT(std::fabs(fl.value()) < 1e-12, "fisher_flat_zero");
    ASSERT(std::fabs(fl.trigger()) < 1e-12, "fisher_flat_trigger_zero");

    // monotonic rising -> price sits at the window high -> positive, growing Fisher
    FisherTransform up(5);
    for (int i = 1; i <= 20; ++i) up.update(static_cast<double>(i));
    ASSERT(up.value() > 0.5, "fisher_uptrend_positive");
    // trigger is the 1-bar-lagged value; in a steady uptrend value > its own lag
    ASSERT(up.value() >= up.trigger(), "fisher_uptrend_above_trigger");

    // monotonic falling -> price sits at the window low -> negative Fisher
    FisherTransform dn(5);
    for (int i = 0; i < 20; ++i) dn.update(100.0 - i);
    ASSERT(dn.value() < -0.5, "fisher_downtrend_negative");

    // clamp guard: an extreme one-sided run must never overflow to inf/nan
    FisherTransform cl(3);
    for (int i = 0; i < 50; ++i) cl.update(static_cast<double>(i) * 1000.0);
    ASSERT(std::isfinite(cl.value()), "fisher_clamp_finite");

    dn.reset();
    ASSERT(!dn.ready() && std::fabs(dn.value()) < 1e-12, "fisher_reset");
}

// Coppock Curve #333 — long-horizon momentum oscillator (WMA of summed ROCs).
void test_coppock() {
    SECTION("Coppock Curve (#333)");
    // Default params need ROC_long(14)+1 warmup, then WMA(10) feeds -> ~25 prices.
    Coppock cop;            // WMA=10, ROC_long=14, ROC_short=11
    for (int i = 0; i < 14; ++i) cop.update(100.0 + i);
    ASSERT(!cop.ready(), "coppock_not_ready_during_warmup");

    // Steady uptrend -> both ROCs positive -> Coppock turns positive (bullish).
    Coppock up(3, 5, 3);   // shorter periods for a compact deterministic test
    for (int i = 0; i < 40; ++i) up.update(100.0 + 2.0 * i);
    ASSERT(up.ready(), "coppock_ready_after_warmup");
    ASSERT(up.value() > 0.0, "coppock_uptrend_positive");
    ASSERT(up.bullish(), "coppock_uptrend_bullish");

    // Steady downtrend -> both ROCs negative -> Coppock negative, not bullish.
    Coppock dn(3, 5, 3);
    for (int i = 0; i < 40; ++i) dn.update(300.0 - 2.0 * i);
    ASSERT(dn.ready(), "coppock_dn_ready");
    ASSERT(dn.value() < 0.0, "coppock_downtrend_negative");
    ASSERT(!dn.bullish(), "coppock_downtrend_not_bullish");

    up.reset();
    ASSERT(!up.ready() && up.value() == 0.0, "coppock_reset");
}

// OBV #341 — On-Balance Volume (cumulative volume-flow indicator).
void test_obv() {
    SECTION("OBV (#341)");
    OBV fresh;
    ASSERT(!fresh.ready(), "obv_not_ready_before_first_print");
    ASSERT(std::fabs(fresh.obv() - 0.0) < 1e-9, "obv_starts_zero");

    // First print only seeds last_price_ — no prior price to compare, obv_ stays 0.
    fresh.on_trade(100.0, 500);
    ASSERT(fresh.ready(), "obv_ready_after_first_print");
    ASSERT(std::fabs(fresh.obv() - 0.0) < 1e-9, "obv_first_print_no_move");

    // Up-tick adds volume; down-tick subtracts; flat leaves it unchanged.
    OBV o;
    o.on_trade(100.0, 500);
    o.on_trade(101.0, 300);   // up -> +300
    ASSERT(std::fabs(o.obv() - 300.0) < 1e-9, "obv_uptick_adds");
    o.on_trade(101.0, 200);   // flat -> unchanged
    ASSERT(std::fabs(o.obv() - 300.0) < 1e-9, "obv_flat_unchanged");
    o.on_trade(99.0, 400);    // down -> -400
    ASSERT(std::fabs(o.obv() - (-100.0)) < 1e-9, "obv_downtick_subtracts");

    // Invalid prints (non-positive price / non-positive volume) are ignored.
    OBV inv;
    inv.on_trade(100.0, 100);
    inv.on_trade(0.0, 100);
    inv.on_trade(-5.0, 100);
    inv.on_trade(105.0, 0);
    inv.on_trade(105.0, -50);
    ASSERT(std::fabs(inv.obv() - 0.0) < 1e-9, "obv_ignores_invalid_prints");

    o.reset();
    ASSERT(!o.ready() && std::fabs(o.obv()) < 1e-9, "obv_reset");
}

// VolumeOscillator #349 — trend of trading ACTIVITY (fast/slow EMA of volume).
void test_volume_oscillator() {
    SECTION("VolumeOscillator (#349)");
    VolumeOscillator empty;
    ASSERT(!empty.ready(), "volosc_not_ready_before_first_print");
    ASSERT(std::fabs(empty.value()) < 1e-9, "volosc_starts_zero");

    // Non-positive volume is ignored (no print).
    VolumeOscillator ig;
    ig.on_trade(100.0, 0);
    ig.on_trade(100.0, -50);
    ASSERT(!ig.ready(), "volosc_ignores_nonpositive_volume");

    // fast=5 (alpha 1/3), slow=20 (alpha 2/21). First print seeds both -> flat.
    VolumeOscillator vo(5, 20);
    vo.on_trade(0.0, 1000);
    ASSERT(vo.ready(), "volosc_ready_after_first_print");
    ASSERT(std::fabs(vo.value()) < 1e-9, "volosc_first_print_flat");
    // Volume jumps to 4000: fast=2000 exactly, slow=27000/21 -> value = 500/9 (%).
    vo.on_trade(0.0, 4000);
    ASSERT(std::fabs(vo.value() - 500.0 / 9.0) < 1e-9, "volosc_spike_up_value");
    ASSERT(vo.rising(), "volosc_spike_up_rising");

    // Fresh oscillator, volume drops sharply: fast=700, slow=19200/21 -> value = -23.4375%.
    VolumeOscillator dn(5, 20);
    dn.on_trade(0.0, 1000);
    dn.on_trade(0.0, 100);
    ASSERT(std::fabs(dn.value() - (-23.4375)) < 1e-9, "volosc_drop_value");
    ASSERT(!dn.rising(), "volosc_drop_not_rising");

    dn.reset();
    ASSERT(!dn.ready() && std::fabs(dn.value()) < 1e-9, "volosc_reset");
}

// PVT #357 — Price Volume Trend (volume weighted by % price-change magnitude).
void test_pvt() {
    SECTION("PVT (#357)");
    PVT p;
    ASSERT(!p.ready(), "pvt_not_ready_before_first_print");
    ASSERT(std::fabs(p.value()) < 1e-9, "pvt_starts_zero");

    // First print only seeds last_price_ -> no prior price, pvt_ stays 0.
    p.on_trade(100.0, 1000);
    ASSERT(p.ready(), "pvt_ready_after_first_print");
    ASSERT(std::fabs(p.value()) < 1e-9, "pvt_first_print_no_move");

    // +10% move on 1000 volume -> pvt += 1000 * 0.10 = 100.
    p.on_trade(110.0, 1000);
    ASSERT(std::fabs(p.value() - 100.0) < 1e-9, "pvt_up_move");
    // -10% move (110 -> 99) on 500 volume -> pvt += 500 * (-0.1) = -50 -> 50.
    p.on_trade(99.0, 500);
    ASSERT(std::fabs(p.value() - 50.0) < 1e-9, "pvt_down_move");

    // Flat print (0% change) leaves pvt_ unchanged.
    p.on_trade(99.0, 2000);
    ASSERT(std::fabs(p.value() - 50.0) < 1e-9, "pvt_flat_unchanged");

    // Same direction, twice the volume moves pvt_ proportionally more.
    PVT big, small;
    big.on_trade(100.0, 1); big.on_trade(110.0, 2000);
    small.on_trade(100.0, 1); small.on_trade(110.0, 1000);
    ASSERT(std::fabs(big.value() - 2.0 * small.value()) < 1e-9, "pvt_scales_with_volume");

    // Invalid prints (non-positive price / non-positive volume) are ignored.
    PVT inv;
    inv.on_trade(100.0, 100);
    inv.on_trade(0.0, 100);
    inv.on_trade(-5.0, 100);
    inv.on_trade(105.0, 0);
    inv.on_trade(105.0, -50);
    ASSERT(std::fabs(inv.value()) < 1e-9, "pvt_ignores_invalid_prints");

    p.reset();
    ASSERT(!p.ready() && std::fabs(p.value()) < 1e-9, "pvt_reset");
}

// PPO #365 — Percentage Price Oscillator (percentage-normalized MACD sibling).
void test_ppo() {
    SECTION("PPO (#365)");
    PPO warm;
    ASSERT(!warm.ready(), "ppo_not_ready_before_update");
    warm.update(100.0);
    ASSERT(warm.ready(), "ppo_ready_after_update");
    // Flat input -> fast EMA == slow EMA -> PPO 0, signal 0, histogram 0.
    PPO fl;
    for (int i = 0; i < 40; ++i) fl.update(50.0);
    ASSERT(std::fabs(fl.ppo()) < 1e-9, "ppo_flat_zero");
    ASSERT(std::fabs(fl.histogram()) < 1e-9, "ppo_flat_hist_zero");

    // Steady uptrend -> fast EMA leads slow -> PPO positive (line sign is robust).
    PPO up(3, 10, 5);
    for (int i = 1; i <= 60; ++i) up.update(100.0 + 1.5 * i);
    ASSERT(up.ppo() > 0.0, "ppo_uptrend_positive");
    // Steady downtrend -> fast EMA below slow -> PPO negative.
    PPO dn(3, 10, 5);
    for (int i = 0; i < 60; ++i) dn.update(300.0 - 1.5 * i);
    ASSERT(dn.ppo() < 0.0, "ppo_downtrend_negative");

    // bullish() = histogram (PPO - signal) sign. Test it deterministically with a
    // flat baseline (PPO=signal=0) then a single jump: the fresh PPO spikes above
    // its still-lagging signal (bullish on an up-jump, not on a down-jump). In a
    // *sustained* trend PPO is a percentage that plateaus, so the histogram
    // converges to ~0 — that's why the trend tests above only check the line sign.
    PPO ju; for (int i = 0; i < 30; ++i) ju.update(50.0);   // flat -> PPO 0, signal 0
    ASSERT(std::fabs(ju.histogram()) < 1e-9, "ppo_flat_before_jump");
    ju.update(60.0);                                        // up-jump
    ASSERT(ju.ppo() > 0.0 && ju.bullish(), "ppo_upjump_bullish");
    PPO jd; for (int i = 0; i < 30; ++i) jd.update(50.0);
    jd.update(40.0);                                        // down-jump
    ASSERT(jd.ppo() < 0.0 && !jd.bullish(), "ppo_downjump_not_bullish");

    // Scale invariance vs MACD: PPO is a PERCENT, so the same shape at 10x the
    // price level gives the same PPO. (MACD would be ~10x larger.)
    PPO lo(3, 10, 5), hi(3, 10, 5);
    for (int i = 1; i <= 60; ++i) { lo.update(10.0 * i); hi.update(100.0 * i); }
    ASSERT(std::fabs(lo.ppo() - hi.ppo()) < 1e-9, "ppo_scale_invariant");

    up.reset();
    ASSERT(!up.ready() && std::fabs(up.ppo()) < 1e-9, "ppo_reset");
}

// ForceIndex #373 — Elder's Force Index (volume * dprice, EMA-smoothed).
void test_force_index() {
    SECTION("ForceIndex (#373)");
    ForceIndex fresh(13);
    ASSERT(!fresh.ready(), "fi_not_ready_before_first_print");
    ASSERT(std::fabs(fresh.value()) < 1e-9, "fi_starts_zero");

    // First print only seeds last_price_ -> nothing feeds the EMA yet.
    fresh.on_trade(100.0, 500);
    ASSERT(!fresh.ready(), "fi_not_ready_after_first_print");
    ASSERT(std::fabs(fresh.raw()) < 1e-9, "fi_first_print_no_raw");

    // raw = volume * dprice. period 1 -> EMA alpha 1 -> value == latest raw.
    ForceIndex f(1);
    f.on_trade(100.0, 500);
    f.on_trade(102.0, 300);        // +2 * 300 = +600
    ASSERT(f.ready(), "fi_ready_after_second_print");
    ASSERT(std::fabs(f.raw() - 600.0) < 1e-9, "fi_raw_up");
    ASSERT(std::fabs(f.value() - 600.0) < 1e-9, "fi_value_period1_equals_raw");
    ASSERT(f.bullish(), "fi_bullish_on_up");
    f.on_trade(99.0, 400);         // -3 * 400 = -1200
    ASSERT(std::fabs(f.raw() - (-1200.0)) < 1e-9, "fi_raw_down");
    ASSERT(!f.bullish(), "fi_not_bullish_on_down");

    // Flat print (no price change) -> raw 0.
    ForceIndex fl(1);
    fl.on_trade(50.0, 100);
    fl.on_trade(50.0, 900);        // dprice 0 -> raw 0
    ASSERT(std::fabs(fl.raw()) < 1e-9, "fi_flat_zero_raw");

    // Invalid prints (non-positive price / volume) are ignored.
    ForceIndex inv(3);
    inv.on_trade(100.0, 100);
    inv.on_trade(0.0, 100);
    inv.on_trade(-5.0, 100);
    inv.on_trade(105.0, 0);
    inv.on_trade(105.0, -50);
    ASSERT(!inv.ready() && std::fabs(inv.value()) < 1e-9, "fi_ignores_invalid_prints");

    f.reset();
    ASSERT(!f.ready() && std::fabs(f.value()) < 1e-9 && std::fabs(f.raw()) < 1e-9, "fi_reset");
}

// MFI #382 — Money Flow Index, volume-weighted RSI in [0,100].
void test_mfi() {
    SECTION("MFI (#382)");
    MFI neutral(14);
    ASSERT(std::fabs(neutral.value() - 50.0) < 1e-9, "mfi_neutral_50_before_data");
    neutral.on_trade(100.0, 500);                 // seeds the baseline only
    ASSERT(std::fabs(neutral.value() - 50.0) < 1e-9, "mfi_first_print_still_50");
    ASSERT(!neutral.ready(), "mfi_not_ready_after_seed");

    // All upticks -> 100; all downticks -> 0.
    MFI up(4);
    up.on_trade(10.0, 100);
    up.on_trade(11.0, 100); up.on_trade(12.0, 100);
    ASSERT(std::fabs(up.value() - 100.0) < 1e-9, "mfi_all_upticks_100");
    MFI dn(4);
    dn.on_trade(10.0, 100);
    dn.on_trade(9.0, 100); dn.on_trade(8.0, 100);
    ASSERT(std::fabs(dn.value() - 0.0) < 1e-9, "mfi_all_downticks_0");

    // Mixed flows: +11*300 = 3300 up, 10*100 = 1000 down -> 100*3300/4300.
    MFI mx(14);
    mx.on_trade(10.0, 500);
    mx.on_trade(11.0, 300);                       // uptick, flow +3300
    mx.on_trade(10.0, 100);                       // downtick, flow -1000
    ASSERT(std::fabs(mx.value() - 100.0 * 3300.0 / 4300.0) < 1e-9, "mfi_mixed_dollar_weighted");

    // Volume weighting: the SAME price path with the volumes swapped gives a
    // different MFI (RSI would see both identically).
    MFI vw(14);
    vw.on_trade(10.0, 500);
    vw.on_trade(11.0, 100);                       // small uptick volume
    vw.on_trade(10.0, 300);                       // big downtick volume
    ASSERT(vw.value() < 50.0 && mx.value() > 50.0, "mfi_volume_direction_matters");

    // Flat print: dropped entirely — value unchanged, no window slot consumed.
    const double mx_before = mx.value();
    mx.on_trade(10.0, 999999);
    ASSERT(std::fabs(mx.value() - mx_before) < 1e-9, "mfi_flat_print_dropped");

    // Rolling window: period 2, a third decided tick evicts the oldest flow.
    MFI rw(2);
    rw.on_trade(10.0, 100);
    rw.on_trade(11.0, 100);                       // +1100
    rw.on_trade(12.0, 100);                       // +1200
    ASSERT(rw.ready(), "mfi_ready_at_full_window");
    rw.on_trade(11.0, 100);                       // -1100 evicts the +1100
    ASSERT(std::fabs(rw.value() - 100.0 * 1200.0 / 2300.0) < 1e-9, "mfi_window_evicts_oldest");

    // Invalid prints ignored; reset returns to the neutral state.
    rw.on_trade(0.0, 100); rw.on_trade(-1.0, 100); rw.on_trade(11.0, 0);
    ASSERT(std::fabs(rw.value() - 100.0 * 1200.0 / 2300.0) < 1e-9, "mfi_ignores_invalid");
    rw.reset();
    ASSERT(!rw.ready() && std::fabs(rw.value() - 50.0) < 1e-9, "mfi_reset_neutral");
}

// VWMA #390 — Volume-Weighted Moving Average (MILESTONE 390).
void test_vwma() {
    SECTION("VWMA (#390)");
    VWMA vempty(20);
    ASSERT(vempty.value() == 0.0 && !vempty.ready(), "vwma_empty_zero");

    // Constant volume -> degenerates to the plain rolling mean.
    VWMA veq(4);
    veq.on_trade(10.0, 100); veq.on_trade(20.0, 100);
    veq.on_trade(30.0, 100); veq.on_trade(40.0, 100);
    ASSERT(veq.ready(), "vwma_ready_at_full_window");
    ASSERT(std::fabs(veq.value() - 25.0) < 1e-9, "vwma_const_volume_is_mean");

    // Volume tilt: the same two prices, weight on the 10 -> below the mean.
    VWMA vtl(4);
    vtl.on_trade(10.0, 300);
    vtl.on_trade(20.0, 100);
    // (10*300 + 20*100) / 400 = 5000/400 = 12.5 (mean would be 15)
    ASSERT(std::fabs(vtl.value() - 12.5) < 1e-9, "vwma_tilts_to_volume");

    // Same prices, volumes swapped -> mirrored tilt above the mean.
    VWMA vts(4);
    vts.on_trade(10.0, 100);
    vts.on_trade(20.0, 300);
    ASSERT(std::fabs(vts.value() - 17.5) < 1e-9, "vwma_mirror_tilt");

    // Rolling window: the fifth print evicts the oldest.
    VWMA vrw(4);
    vrw.on_trade(10.0, 100); vrw.on_trade(20.0, 100);
    vrw.on_trade(30.0, 100); vrw.on_trade(40.0, 100);
    vrw.on_trade(50.0, 100);                       // evicts the 10
    // (20+30+40+50)/4 = 35
    ASSERT(std::fabs(vrw.value() - 35.0) < 1e-9, "vwma_window_evicts_oldest");

    // Invalid prints are ignored and do not consume a slot.
    const double vrw_before = vrw.value();
    vrw.on_trade(0.0, 100); vrw.on_trade(-5.0, 100); vrw.on_trade(60.0, 0); vrw.on_trade(60.0, -10);
    ASSERT(std::fabs(vrw.value() - vrw_before) < 1e-9, "vwma_ignores_invalid");

    // Period 1 -> tracks the latest print exactly.
    VWMA vp1(1);
    vp1.on_trade(11.0, 500);
    vp1.on_trade(13.0, 1);
    ASSERT(std::fabs(vp1.value() - 13.0) < 1e-9, "vwma_period1_latest");

    vrw.reset();
    ASSERT(vrw.value() == 0.0 && !vrw.ready(), "vwma_reset");
}

// NVI/PVI #398 — volume-gated cumulative indices (crowd vs smart money).
void test_nvi_pvi() {
    SECTION("NVI/PVI (#398)");
    VolumeIndices vgi;
    ASSERT(vgi.nvi() == 1000.0 && vgi.pvi() == 1000.0 && !vgi.ready(), "nvipvi_base_1000");
    vgi.on_trade(100.0, 500);                    // seeds only
    ASSERT(vgi.nvi() == 1000.0 && vgi.pvi() == 1000.0 && vgi.ready(), "nvipvi_seed_no_move");

    // Uptick on RISING volume -> PVI moves by the % change, NVI frozen.
    vgi.on_trade(102.0, 800);                    // +2% on rising volume
    ASSERT(std::fabs(vgi.pvi() - 1020.0) < 1e-9, "nvipvi_pvi_rising_volume");
    ASSERT(vgi.nvi() == 1000.0, "nvipvi_nvi_frozen_on_rising");

    // Downtick on FALLING volume -> NVI moves, PVI frozen.
    vgi.on_trade(100.98, 300);                   // -1% on falling volume
    ASSERT(std::fabs(vgi.nvi() - 990.0) < 1e-9, "nvipvi_nvi_falling_volume");
    ASSERT(std::fabs(vgi.pvi() - 1020.0) < 1e-9, "nvipvi_pvi_frozen_on_falling");

    // Unchanged volume -> neither index moves, but the price baseline does.
    vgi.on_trade(105.0, 300);                    // volume equal to previous
    ASSERT(std::fabs(vgi.nvi() - 990.0) < 1e-9
           && std::fabs(vgi.pvi() - 1020.0) < 1e-9, "nvipvi_equal_volume_frozen");
    // ...so the next % change is measured from 105, not 100.98.
    vgi.on_trade(107.1, 400);                    // +2% on rising volume
    ASSERT(std::fabs(vgi.pvi() - 1040.4) < 1e-9, "nvipvi_baseline_advanced");

    // Invalid prints are ignored and do not disturb the baselines.
    vgi.on_trade(0.0, 100); vgi.on_trade(-2.0, 100); vgi.on_trade(110.0, 0);
    ASSERT(std::fabs(vgi.pvi() - 1040.4) < 1e-9
           && std::fabs(vgi.nvi() - 990.0) < 1e-9, "nvipvi_ignores_invalid");

    vgi.reset();
    ASSERT(vgi.nvi() == 1000.0 && vgi.pvi() == 1000.0 && !vgi.ready(), "nvipvi_reset");
}

// CloseATR #406 — close-to-close ATR (Wilder), volatility in price units.
void test_close_atr() {
    SECTION("CloseATR (#406)");
    CloseATR fresh(14);
    ASSERT(fresh.value() == 0.0 && !fresh.ready(), "catr_empty_zero");
    fresh.update(100.0);                          // seeds only — no range yet
    ASSERT(fresh.value() == 0.0 && !fresh.ready(), "catr_seed_no_range");

    // Warmup is the exact mean of the first N ranges: |+2|, |-4|, |+6| -> 4.
    CloseATR cwu(3);
    cwu.update(100.0);
    cwu.update(102.0); cwu.update(98.0); cwu.update(104.0);
    ASSERT(cwu.ready(), "catr_ready_after_period_ranges");
    ASSERT(std::fabs(cwu.value() - 4.0) < 1e-9, "catr_warmup_exact_mean");

    // After warmup, Wilder smoothing: ATR = (4*2 + 1)/3 = 3.
    cwu.update(105.0);                            // TR = 1
    ASSERT(std::fabs(cwu.value() - 3.0) < 1e-9, "catr_wilder_step");

    // A constant price stream decays the ATR toward zero (TR = 0 forever).
    CloseATR cdec(2);
    cdec.update(50.0); cdec.update(54.0); cdec.update(50.0);   // mean(4,4) = 4
    ASSERT(std::fabs(cdec.value() - 4.0) < 1e-9, "catr_two_ranges");
    cdec.update(50.0); cdec.update(50.0);         // TR 0 twice: 4 -> 2 -> 1
    ASSERT(std::fabs(cdec.value() - 1.0) < 1e-9, "catr_decays_on_quiet");

    // Direction does not matter — only the magnitude of the move.
    CloseATR cup(2), cdn(2);
    cup.update(10.0); cup.update(12.0); cup.update(14.0);
    cdn.update(10.0); cdn.update(8.0);  cdn.update(6.0);
    ASSERT(std::fabs(cup.value() - cdn.value()) < 1e-9, "catr_direction_agnostic");

    // Invalid prices are ignored and do not disturb the baseline.
    const double cup_before = cup.value();
    cup.update(0.0); cup.update(-3.0);
    ASSERT(std::fabs(cup.value() - cup_before) < 1e-9, "catr_ignores_invalid");

    cup.reset();
    ASSERT(cup.value() == 0.0 && !cup.ready(), "catr_reset");
}

// Keltner #414 — EMA midline + CloseATR bands (the ATR-based envelope).
void test_keltner() {
    SECTION("Keltner (#414)");
    Keltner kfresh(20, 14);
    ASSERT(!kfresh.ready() && kfresh.mid() == 0.0, "kelt_empty");

    // ema_period 1 -> the midline IS the latest price; atr_period 2.
    Keltner kc(1, 2);
    kc.update(100.0);                             // seeds both legs
    ASSERT(!kc.ready(), "kelt_seed_not_ready");
    kc.update(102.0); kc.update(98.0);            // ranges 2, 4 -> ATR 3
    ASSERT(kc.ready(), "kelt_ready_after_atr_period");
    ASSERT(std::fabs(kc.mid() - 98.0) < 1e-9, "kelt_mid_tracks_price");
    ASSERT(std::fabs(kc.atr() - 3.0) < 1e-9, "kelt_atr_mean_of_ranges");
    // Bands at mult 2: 98 +/- 6.
    ASSERT(std::fabs(kc.upper(2.0) - 104.0) < 1e-9, "kelt_upper");
    ASSERT(std::fabs(kc.lower(2.0) - 92.0) < 1e-9, "kelt_lower");
    // Band coordinates: mid -> 0.5, upper -> 1.0, beyond -> > 1.
    ASSERT(std::fabs(kc.percent_b(98.0, 2.0) - 0.5) < 1e-9, "kelt_pctb_mid");
    ASSERT(std::fabs(kc.percent_b(104.0, 2.0) - 1.0) < 1e-9, "kelt_pctb_upper");
    ASSERT(kc.percent_b(106.0, 2.0) > 1.0, "kelt_pctb_breakout");
    // A tighter multiplier narrows the envelope symmetrically.
    ASSERT(std::fabs(kc.upper(1.0) - 101.0) < 1e-9
           && std::fabs(kc.lower(1.0) - 95.0) < 1e-9, "kelt_mult_scales");
    // No width yet (flat tape, ATR 0) -> percent_b stays neutral.
    Keltner kflat(1, 2);
    kflat.update(50.0); kflat.update(50.0); kflat.update(50.0);
    ASSERT(std::fabs(kflat.percent_b(50.0, 2.0) - 0.5) < 1e-9, "kelt_flat_neutral");
    kc.reset();
    ASSERT(!kc.ready() && kc.mid() == 0.0, "kelt_reset");
}

// ZLEMA #422 — Zero-Lag EMA (input de-lagged by 2p - p[t-lag]).
void test_zlema() {
    SECTION("ZLEMA (#422)");
    ZLEMA zfresh(20);
    ASSERT(!zfresh.ready() && zfresh.value() == 0.0 && zfresh.lag() == 9, "zlema_fresh");

    // Constant tape: the de-lagged input equals the price -> ZLEMA == price.
    ZLEMA zc(5);
    for (int zi = 0; zi < 10; ++zi) zc.update(42.0);
    ASSERT(zc.ready() && std::fabs(zc.value() - 42.0) < 1e-9, "zlema_constant_tape");

    // Algebraic identity: period 3 (alpha 0.5, lag 1) tracks ANY path
    // exactly — 0.5*(2p - prev) + 0.5*prev == p by induction.
    ZLEMA z3(3);
    z3.update(10.0);
    // Lag 1: a single print both seeds the EMA and fills the buffer.
    ASSERT(z3.ready() && std::fabs(z3.value() - 10.0) < 1e-9, "zlema_seeded_at_first");
    z3.update(20.0);
    ASSERT(z3.ready() && std::fabs(z3.value() - 20.0) < 1e-9, "zlema3_tracks_up");
    z3.update(30.0);
    ASSERT(std::fabs(z3.value() - 30.0) < 1e-9, "zlema3_tracks_ramp");
    z3.update(17.0);                                    // sharp reversal
    ASSERT(std::fabs(z3.value() - 17.0) < 1e-9, "zlema3_tracks_reversal");

    // On a steady ramp ZLEMA hugs the price where the plain EMA trails.
    ZLEMA zl(9);
    EMA   zre = EMA::from_period(9);
    double zpx = 100.0;
    for (int zj = 0; zj < 40; ++zj) {
        zpx += 1.0;
        zl.update(zpx);
        zre.update(zpx);
    }
    ASSERT(std::fabs(zl.value() - zpx) < std::fabs(zre.value() - zpx),
           "zlema_less_lag_than_ema");
    ASSERT(zre.value() < zpx, "zlema_ema_reference_trails");

    // Invalid prices are ignored; reset returns to the fresh state.
    const double zl_before = zl.value();
    zl.update(0.0); zl.update(-5.0);
    ASSERT(std::fabs(zl.value() - zl_before) < 1e-9, "zlema_ignores_invalid");
    zl.reset();
    ASSERT(!zl.ready() && zl.value() == 0.0, "zlema_reset");
}

// Chandelier #430 — ATR-anchored trailing stop levels (MILESTONE 430).
void test_chandelier() {
    SECTION("Chandelier (#430)");
    Chandelier chf(22, 14);
    ASSERT(!chf.ready() && chf.highest() == 0.0 && chf.lowest() == 0.0, "chand_empty");

    // period 3, atr_period 2; prices 100, 104, 96: extremes 104/96,
    // TRs 4 and 8 -> ATR (warmup mean) = 6, both legs ready.
    Chandelier ch(3, 2);
    ch.update(100.0);
    ASSERT(!ch.ready(), "chand_not_ready_early");
    ch.update(104.0); ch.update(96.0);
    ASSERT(ch.ready(), "chand_ready_both_legs");
    ASSERT(std::fabs(ch.highest() - 104.0) < 1e-9 && std::fabs(ch.lowest() - 96.0) < 1e-9,
           "chand_extremes");
    ASSERT(std::fabs(ch.atr() - 6.0) < 1e-9, "chand_atr_6");
    ASSERT(std::fabs(ch.long_stop(1.0) - 98.0) < 1e-9, "chand_long_stop_1x");
    ASSERT(std::fabs(ch.short_stop(1.0) - 102.0) < 1e-9, "chand_short_stop_1x");
    // The multiplier scales the hang distance symmetrically.
    ASSERT(std::fabs(ch.long_stop(2.0) - 92.0) < 1e-9
           && std::fabs(ch.short_stop(2.0) - 108.0) < 1e-9, "chand_mult_scales");

    // A new high evicts the oldest print and RATCHETS the long stop up:
    // 110 replaces 100 -> highest 110, TR 14 -> Wilder ATR 6+(14-6)/2 = 10.
    ch.update(110.0);
    ASSERT(std::fabs(ch.highest() - 110.0) < 1e-9, "chand_ratchet_high");
    ASSERT(std::fabs(ch.atr() - 10.0) < 1e-9, "chand_wilder_step");
    ASSERT(std::fabs(ch.long_stop(1.0) - 100.0) < 1e-9, "chand_long_stop_ratchets");

    // A quiet tape tightens the stop toward the extreme (ATR decays).
    Chandelier chq(3, 2);
    chq.update(50.0); chq.update(54.0); chq.update(54.0);
    const double chq_wide = chq.long_stop(1.0);
    chq.update(54.0); chq.update(54.0);
    ASSERT(chq.long_stop(1.0) > chq_wide, "chand_quiet_tightens");

    // Invalid prices ignored; reset returns to the fresh state.
    const double ch_ls = ch.long_stop(1.0);
    ch.update(0.0); ch.update(-1.0);
    ASSERT(std::fabs(ch.long_stop(1.0) - ch_ls) < 1e-9, "chand_ignores_invalid");
    ch.reset();
    ASSERT(!ch.ready() && ch.highest() == 0.0, "chand_reset");
}

// AwesomeOsc #438 — Bill Williams' AO: SMA(fast) - SMA(slow).
void test_awesome() {
    SECTION("AwesomeOsc (#438)");
    AwesomeOsc aofr(5, 34);
    ASSERT(!aofr.ready() && aofr.value() == 0.0, "ao_fresh");
    ASSERT(aofr.fast() == 5 && aofr.slow() == 34, "ao_default_periods");

    // Not ready until the SLOW window fills; the fast leg lives earlier.
    AwesomeOsc ao(2, 4);
    ao.update(10.0); ao.update(20.0); ao.update(30.0);
    ASSERT(!ao.ready() && ao.value() == 0.0, "ao_not_ready_early");
    ASSERT(std::fabs(ao.fast_ma() - 25.0) < 1e-9, "ao_fast_leg_early");   // (20+30)/2
    ao.update(40.0);
    ASSERT(ao.ready(), "ao_ready_at_slow");
    // fast (30+40)/2 = 35, slow (10+..+40)/4 = 25 -> AO = +10.
    ASSERT(std::fabs(ao.value() - 10.0) < 1e-9, "ao_ramp_plus_10");
    // On a LINEAR ramp the gap between the two means is constant.
    ao.update(50.0);
    ASSERT(std::fabs(ao.value() - 10.0) < 1e-9, "ao_linear_ramp_constant");

    // A falling tape mirrors the sign.
    AwesomeOsc aod(2, 4);
    aod.update(40.0); aod.update(30.0); aod.update(20.0); aod.update(10.0);
    ASSERT(std::fabs(aod.value() + 10.0) < 1e-9, "ao_downramp_minus_10");

    // A flat tape reads EXACTLY zero (simple means, unlike MACD's EMAs).
    AwesomeOsc aof(2, 4);
    for (int aoi = 0; aoi < 8; ++aoi) aof.update(77.0);
    ASSERT(aof.ready() && aof.value() == 0.0, "ao_flat_exact_zero");

    // Constructor guards: fast must stay the shorter leg.
    AwesomeOsc aog(10, 5);
    ASSERT(aog.slow() == 5 && aog.fast() == 4, "ao_fast_clamped_below_slow");

    // Invalid prices ignored; reset returns to fresh.
    const double ao_before = ao.value();
    ao.update(0.0); ao.update(-2.0);
    ASSERT(std::fabs(ao.value() - ao_before) < 1e-9, "ao_ignores_invalid");
    ao.reset();
    ASSERT(!ao.ready() && ao.value() == 0.0, "ao_reset");
}

// AccelDecel #446 — Bill Williams' AC: the derivative of AO momentum.
void test_accel_decel() {
    SECTION("AccelDecel (#446)");
    AccelDecel acf(5, 34, 5);
    ASSERT(!acf.ready() && acf.value() == 0.0, "ac_fresh");

    // On a LINEAR ramp the AO is constant (#438) -> AC is exactly 0:
    // constant momentum carries zero acceleration.
    AccelDecel acl(2, 4, 3);
    double aclp = 10.0;
    for (int aci = 0; aci < 12; ++aci) { aclp += 5.0; acl.update(aclp); }
    ASSERT(acl.ready(), "ac_ready");
    ASSERT(std::fabs(acl.value()) < 1e-9, "ac_linear_ramp_zero");
    ASSERT(std::fabs(acl.ao() - 5.0) < 1e-9, "ac_ao_leg_constant");   // (2-mean)-(4-mean) gap

    // A BEND in the path (acceleration) pushes AC positive: the fresh AO
    // readings outrun their own recent mean.
    acl.update(aclp + 20.0);
    ASSERT(acl.value() > 0.0, "ac_acceleration_positive");
    // Deceleration (a stall after the ramp) flips it negative.
    AccelDecel acd(2, 4, 3);
    double acdp = 10.0;
    for (int acj = 0; acj < 12; ++acj) { acdp += 5.0; acd.update(acdp); }
    acd.update(acdp); acd.update(acdp);            // momentum bleeds off
    ASSERT(acd.value() < 0.0, "ac_deceleration_negative");

    // A flat tape is zero all the way through (AO 0, mean 0).
    AccelDecel acz(2, 4, 3);
    for (int ack = 0; ack < 10; ++ack) acz.update(50.0);
    ASSERT(acz.ready() && acz.value() == 0.0, "ac_flat_zero");

    // Invalid prices ignored; reset returns to fresh.
    const double ac_before = acl.value();
    acl.update(0.0); acl.update(-1.0);
    ASSERT(std::fabs(acl.value() - ac_before) < 1e-9, "ac_ignores_invalid");
    acl.reset();
    ASSERT(!acl.ready() && acl.value() == 0.0, "ac_reset");
}

// RollingMedian #454 — the outlier-immune price filter.
void test_rolling_median() {
    SECTION("RollingMedian (#454)");
    RollingMedian rmf(9);
    ASSERT(!rmf.ready() && rmf.value() == 0.0, "rmed_empty");

    // Odd window: the exact middle value regardless of arrival order.
    RollingMedian rmo(5);
    rmo.update(30.0); rmo.update(10.0); rmo.update(50.0);
    rmo.update(20.0); rmo.update(40.0);
    ASSERT(rmo.ready() && std::fabs(rmo.value() - 30.0) < 1e-9, "rmed_odd_exact_middle");

    // Even window: the mean of the two middle values.
    RollingMedian rme(4);
    rme.update(10.0); rme.update(20.0); rme.update(30.0); rme.update(40.0);
    ASSERT(std::fabs(rme.value() - 25.0) < 1e-9, "rmed_even_middle_pair");

    // THE point: a fat-finger print cannot move it. {10,10,10,1000,10}
    // reads 10 on the median while the mean is dragged to 208.
    RollingMedian rmr(5);
    rmr.update(10.0); rmr.update(10.0); rmr.update(10.0);
    rmr.update(1000.0); rmr.update(10.0);
    ASSERT(std::fabs(rmr.value() - 10.0) < 1e-9, "rmed_outlier_immune");
    // ...until half the window agrees the move is real.
    rmr.update(1000.0); rmr.update(1000.0);        // window {10,1000,10,1000,1000}
    ASSERT(std::fabs(rmr.value() - 1000.0) < 1e-9, "rmed_majority_confirms");

    // Rolling eviction: the oldest print leaves the window.
    RollingMedian rmv(3);
    rmv.update(10.0); rmv.update(20.0); rmv.update(30.0);
    ASSERT(std::fabs(rmv.value() - 20.0) < 1e-9, "rmed_window_median");
    rmv.update(40.0);                              // {20,30,40}
    ASSERT(std::fabs(rmv.value() - 30.0) < 1e-9, "rmed_evicts_oldest");

    // Partial windows use what has arrived; invalid prints ignored; reset.
    RollingMedian rmp(9);
    rmp.update(7.0); rmp.update(9.0);
    ASSERT(!rmp.ready() && std::fabs(rmp.value() - 8.0) < 1e-9, "rmed_partial_window");
    rmp.update(0.0); rmp.update(-3.0);
    ASSERT(std::fabs(rmp.value() - 8.0) < 1e-9, "rmed_ignores_invalid");
    rmp.reset();
    ASSERT(!rmp.ready() && rmp.value() == 0.0, "rmed_reset");
}

// SuperTrend #462 — ATR-banded trend regime signal.
void test_supertrend() {
    SECTION("SuperTrend (#462)");
    SuperTrend stf(10, 3.0);
    ASSERT(!stf.ready() && stf.value() == 0.0, "st_fresh");
    ASSERT(stf.direction() == 1, "st_seed_regime_up");

    // Warm the ATR then ride a steady uptrend: the regime stays long and
    // the line (lower band) sits BELOW price, ratcheting up.
    SuperTrend st(3, 2.0);
    double p = 100.0;
    for (int i = 0; i < 6; ++i) { p += 2.0; st.update(p); }   // rising ramp
    ASSERT(st.ready() && st.is_uptrend(), "st_uptrend_on_ramp");
    ASSERT(st.value() < p, "st_line_below_price_when_long");
    const double line_lo = st.value();
    st.update(p + 2.0);                                        // one more up bar
    ASSERT(st.value() >= line_lo, "st_line_ratchets_up");

    // A crash far through the lower band FLIPS the regime to short; the
    // line jumps to the upper band, now ABOVE price.
    st.update(50.0);
    ASSERT(!st.is_uptrend() && st.direction() == -1, "st_flips_to_down");
    ASSERT(st.value() > 50.0, "st_line_above_price_when_short");

    // A recovery back above the upper band flips it long again.
    for (int j = 0; j < 6; ++j) st.update(200.0);
    ASSERT(st.is_uptrend(), "st_flips_back_up");

    // Invalid prices ignored; reset returns to the seed state.
    const double st_line = st.value();
    st.update(0.0); st.update(-5.0);
    ASSERT(std::fabs(st.value() - st_line) < 1e-9, "st_ignores_invalid");
    st.reset();
    ASSERT(!st.ready() && st.value() == 0.0 && st.direction() == 1, "st_reset");
}

// UltimateOsc #470 — Williams' three-timeframe momentum oscillator.
void test_ultimate() {
    SECTION("UltimateOsc (#470)");
    UltimateOsc uof(7, 14, 28);
    ASSERT(!uof.ready() && std::fabs(uof.value() - 50.0) < 1e-9, "uo_neutral_before_ready");

    // Small windows for exact tests: 2/3/4.
    UltimateOsc uo(2, 3, 4);
    uo.update(100.0);                 // seed
    ASSERT(!uo.ready(), "uo_not_ready_after_seed");

    // Pure uptick run: every BP == TR, so every avg == 1 -> UO = 100.
    UltimateOsc up(2, 3, 4);
    double px = 100.0;
    for (int i = 0; i < 6; ++i) { px += 1.0; up.update(px); }
    ASSERT(up.ready() && std::fabs(up.value() - 100.0) < 1e-9, "uo_pure_uptrend_100");

    // Pure downtick run: every BP == 0 -> every avg == 0 -> UO = 0.
    UltimateOsc dn(2, 3, 4);
    px = 100.0;
    for (int i = 0; i < 6; ++i) { px -= 1.0; dn.update(px); }
    ASSERT(std::fabs(dn.value()) < 1e-9, "uo_pure_downtrend_0");

    // A balanced alternating tape sits strictly between the extremes.
    UltimateOsc bal(2, 3, 4);
    const double bseq[] = {100, 101, 100, 101, 100, 101, 100};
    for (double v : bseq) bal.update(v);
    ASSERT(bal.value() > 0.0 && bal.value() < 100.0, "uo_balanced_mid");

    // Bounded in [0,100] on a mixed tape.
    UltimateOsc mix(2, 3, 4);
    const double mseq[] = {100, 103, 101, 105, 102, 108, 104, 110};
    for (double v : mseq) mix.update(v);
    ASSERT(mix.value() >= 0.0 && mix.value() <= 100.0, "uo_bounded");

    // Invalid prints ignored; reset returns to the neutral state.
    const double up_before = up.value();
    up.update(0.0); up.update(-3.0);
    ASSERT(std::fabs(up.value() - up_before) < 1e-9, "uo_ignores_invalid");
    up.reset();
    ASSERT(!up.ready() && std::fabs(up.value() - 50.0) < 1e-9, "uo_reset");
}

// ChoppinessIndex #478 — trend-vs-range regime classifier.
void test_choppiness() {
    SECTION("ChoppinessIndex (#478)");
    ChoppinessIndex cif(14);
    ASSERT(!cif.ready() && std::fabs(cif.value() - 50.0) < 1e-9, "chop_neutral_before_ready");

    // A perfect oscillation fills the window with motion but no net span:
    // window {105,100,105,100}, TRs all 5 -> sumTR 20, range 5, ratio 4 = n
    // -> CI = 100 (maximally choppy). period 4.
    ChoppinessIndex chop(4);
    const double oseq[] = {100, 105, 100, 105, 100};
    for (double v : oseq) chop.update(v);
    ASSERT(chop.ready() && std::fabs(chop.value() - 100.0) < 1e-9, "chop_oscillation_100");

    // A steady one-directional ramp is efficient: much LOWER CI than the
    // chop, and strictly inside (0,100).
    ChoppinessIndex trend(4);
    double px = 100.0;
    for (int i = 0; i < 6; ++i) { px += 1.0; trend.update(px); }
    ASSERT(trend.ready(), "chop_trend_ready");
    ASSERT(trend.value() < chop.value(), "chop_trend_below_oscillation");
    ASSERT(trend.value() > 0.0 && trend.value() < 100.0, "chop_trend_bounded");

    // A dead-flat window is the degenerate case -> 0 (no range).
    ChoppinessIndex flat(4);
    for (int i = 0; i < 6; ++i) flat.update(50.0);
    ASSERT(flat.ready() && flat.value() == 0.0, "chop_flat_zero");

    // Always bounded in [0,100] on a mixed tape.
    ChoppinessIndex mix(4);
    const double mseq[] = {100, 103, 99, 106, 97, 108, 95, 110};
    for (double v : mseq) mix.update(v);
    ASSERT(mix.value() >= 0.0 && mix.value() <= 100.0, "chop_bounded");

    // Invalid prints ignored; reset returns to neutral.
    const double chop_before = chop.value();
    chop.update(0.0); chop.update(-2.0);
    ASSERT(std::fabs(chop.value() - chop_before) < 1e-9, "chop_ignores_invalid");
    chop.reset();
    ASSERT(!chop.ready() && std::fabs(chop.value() - 50.0) < 1e-9, "chop_reset");
}

// RollingPercentRank #486 — non-parametric price position.
void test_percent_rank() {
    SECTION("RollingPercentRank (#486)");
    RollingPercentRank prf(20);
    ASSERT(!prf.ready() && std::fabs(prf.value() - 50.0) < 1e-9, "prank_neutral_before_data");

    // {10,20,30,40,50}: last print 50 is the highest -> rank 100 (4 below / 4).
    RollingPercentRank ph(5);
    for (double v : {10.0, 20.0, 30.0, 40.0, 50.0}) ph.update(v);
    ASSERT(ph.ready() && std::fabs(ph.value() - 100.0) < 1e-9, "prank_highest_100");
    // Now a low print: {20,30,40,50,15} -> 15 is the lowest -> rank 0.
    ph.update(15.0);
    ASSERT(std::fabs(ph.value()) < 1e-9, "prank_lowest_0");
    // A middle print: {30,40,50,15,35} -> below 35 are {30,15} = 2 of 4 -> 50.
    ph.update(35.0);
    ASSERT(std::fabs(ph.value() - 50.0) < 1e-9, "prank_middle_50");

    // The outlier immunity vs Stochastic: {10,11,12,13,100}, last print 13.
    // Three of four are below -> rank 75, where a min-max stochastic would
    // read ~3% (13 barely above the 10 low of a range stretched to 100).
    RollingPercentRank po(5);
    for (double v : {10.0, 11.0, 12.0, 100.0, 13.0}) po.update(v);
    ASSERT(std::fabs(po.value() - 75.0) < 1e-9, "prank_outlier_immune");
    const double stoch_like = 100.0 * (13.0 - 10.0) / (100.0 - 10.0);   // ~3.33
    ASSERT(po.value() > stoch_like + 50.0, "prank_diverges_from_range");

    // Ties: a value equal to others counts only STRICTLY-below.
    RollingPercentRank pt(4);
    for (double v : {50.0, 50.0, 50.0, 50.0}) pt.update(v);
    ASSERT(std::fabs(pt.value()) < 1e-9, "prank_all_equal_zero");

    // Invalid prints ignored; reset returns to neutral.
    const double po_before = po.value();
    po.update(0.0); po.update(-1.0);
    ASSERT(std::fabs(po.value() - po_before) < 1e-9, "prank_ignores_invalid");
    po.reset();
    ASSERT(!po.ready() && std::fabs(po.value() - 50.0) < 1e-9, "prank_reset");
}

// ChandeForecastOsc #494 — % deviation of price from its LSMA forecast.
void test_cfo() {
    SECTION("ChandeForecastOsc (#494)");
    ChandeForecastOsc cff(5);
    ASSERT(!cff.ready() && cff.value() == 0.0, "cfo_zero_before_ready");

    // A perfectly linear ramp: the price sits ON its own regression line,
    // so CFO ~ 0 however steep the trend.
    ChandeForecastOsc cr(5);
    double px = 100.0;
    for (int i = 0; i < 5; ++i) { cr.update(px); px += 2.0; }
    ASSERT(cr.ready(), "cfo_ready");
    ASSERT(std::fabs(cr.value()) < 1e-9, "cfo_on_trend_zero");

    // A price that OVERSHOOTS above the regression endpoint -> positive CFO.
    ChandeForecastOsc cu(5);
    for (double v : {100.0, 101.0, 102.0, 103.0, 104.0}) cu.update(v);
    ASSERT(std::fabs(cu.value()) < 1e-9, "cfo_ramp_zero");   // on trend
    cu.update(120.0);                                        // spike above trend
    ASSERT(cu.value() > 0.0, "cfo_overshoot_positive");
    ASSERT(cu.lsma() < 120.0, "cfo_lsma_below_spike");

    // A price that LAGS below the regression -> negative CFO.
    ChandeForecastOsc cd(5);
    for (double v : {100.0, 101.0, 102.0, 103.0, 104.0}) cd.update(v);
    cd.update(90.0);                                         // drop below trend
    ASSERT(cd.value() < 0.0, "cfo_undershoot_negative");

    // Invalid prints ignored; reset returns to the unready state.
    const double cu_before = cu.value();
    cu.update(0.0); cu.update(-5.0);
    ASSERT(std::fabs(cu.value() - cu_before) < 1e-9, "cfo_ignores_invalid");
    cu.reset();
    ASSERT(!cu.ready() && cu.value() == 0.0, "cfo_reset");
}

// PrettyGoodOsc #502 — ATR-normalized displacement from the SMA.
void test_pgo() {
    SECTION("PrettyGoodOsc (#502)");
    PrettyGoodOsc pgf(14);
    ASSERT(!pgf.ready() && pgf.value() == 0.0, "pgo_zero_before_ready");

    // period 2 (ATR(n) needs n ranges = n+1 prices): 10, 20, 30 -> rolling
    // SMA over the last two (20,30) = 25, ATR mean(|+10|,|+10|) = 10, latest
    // 30 -> PGO = (30 - 25) / 10 = +0.5.
    PrettyGoodOsc pg(2);
    pg.update(10.0); pg.update(20.0); pg.update(30.0);
    ASSERT(pg.ready(), "pgo_ready");
    ASSERT(std::fabs(pg.atr() - 10.0) < 1e-9, "pgo_atr_10");
    ASSERT(std::fabs(pg.value() - 0.5) < 1e-9, "pgo_half_range_above");

    // A price BELOW its SMA reads negative: 30,20,10 -> SMA(20,10)=15,
    // latest 10 -> (10 - 15) / 10 = -0.5.
    PrettyGoodOsc pd(2);
    pd.update(30.0); pd.update(20.0); pd.update(10.0);
    ASSERT(std::fabs(pd.value() + 0.5) < 1e-9, "pgo_below_mean_negative");

    // A flat tape has zero ATR -> no scale -> 0 (not a division blow-up).
    PrettyGoodOsc pf(2);
    for (int i = 0; i < 5; ++i) pf.update(50.0);
    ASSERT(pf.ready() && pf.value() == 0.0, "pgo_flat_zero");

    // Invalid prints ignored; reset returns to the unready state.
    const double pg_before = pg.value();
    pg.update(0.0); pg.update(-5.0);
    ASSERT(std::fabs(pg.value() - pg_before) < 1e-9, "pgo_ignores_invalid");
    pg.reset();
    ASSERT(!pg.ready() && pg.value() == 0.0, "pgo_reset");
}

// KST #510 — Know Sure Thing (Pring's weighted sum of four smoothed ROCs).
void test_kst() {
    SECTION("KST Know Sure Thing (#510)");
    // Default params need ROC(30)+1, then SMA(15), then signal(9) -> ~53 prices.
    KST kdef;
    for (int i = 0; i < 20; ++i) kdef.update(100.0 + i);
    ASSERT(!kdef.ready(), "kst_not_ready_during_warmup");

    // Compact params for deterministic tests: ROC 3/5/7/9, SMA 3 each, signal 3.
    // Steady linear uptrend -> every ROC positive -> KST > 0.
    KST up(3, 5, 7, 9, 3, 3, 3, 3, 3);
    for (int i = 0; i < 40; ++i) up.update(100.0 + 2.0 * i);
    ASSERT(up.ready(), "kst_ready_after_warmup");
    ASSERT(up.value() > 0.0, "kst_uptrend_positive");

    // Accelerating uptrend -> KST leads its lagging signal line -> bullish.
    KST acc(3, 5, 7, 9, 3, 3, 3, 3, 3);
    double pa = 100.0, g = 1.005;
    for (int i = 0; i < 40; ++i) { acc.update(pa); pa *= g; g += 0.002; }
    ASSERT(acc.value() > 0.0 && acc.bullish(), "kst_accelerating_bullish");

    // Steady downtrend -> every ROC negative -> KST < 0, not bullish.
    KST dn(3, 5, 7, 9, 3, 3, 3, 3, 3);
    for (int i = 0; i < 40; ++i) dn.update(300.0 - 2.0 * i);
    ASSERT(dn.value() < 0.0, "kst_downtrend_negative");
    ASSERT(!dn.bullish(), "kst_downtrend_not_bullish");

    // Flat tape -> every ROC 0 -> KST 0 (no division blow-up).
    KST fl(3, 5, 7, 9, 3, 3, 3, 3, 3);
    for (int i = 0; i < 40; ++i) fl.update(50.0);
    ASSERT(fl.ready() && std::fabs(fl.value()) < 1e-9, "kst_flat_zero");

    // Scale invariance: KST sums ROC PERCENTAGES, so a 10x-priced but
    // identically-shaped path gives the same reading bit-for-bit in the limit.
    KST sa(3, 5, 7, 9, 3, 3, 3, 3, 3), sb(3, 5, 7, 9, 3, 3, 3, 3, 3);
    for (int i = 0; i < 40; ++i) { const double x = 30.0 + i; sa.update(x); sb.update(x * 10.0); }
    ASSERT(std::fabs(sa.value() - sb.value()) < 1e-9, "kst_scale_invariant");

    up.reset();
    ASSERT(!up.ready() && up.value() == 0.0, "kst_reset");
}

// VIDYA #518 — Variable Index Dynamic Average (CMO-adaptive MA).
void test_vidya() {
    SECTION("VIDYA (#518)");
    VIDYA vf;                                  // period 9 -> CMO needs 10 prices
    for (int i = 0; i < 5; ++i) vf.update(100.0 + i);
    ASSERT(!vf.ready(), "vidya_not_ready_during_warmup");

    // Flat tape: CMO = 0 -> effective alpha 0 -> VIDYA pins exactly to the level.
    VIDYA vc(9);
    for (int i = 0; i < 30; ++i) vc.update(50.0);
    ASSERT(vc.ready() && std::fabs(vc.value() - 50.0) < 1e-9, "vidya_flat_pins_price");

    // Steady uptrend: CMO -> +100 (all up moves) so VIDYA behaves as an EMA,
    // lagging below the latest price while rising monotonically.
    VIDYA vu(9);
    double last = 0.0;
    for (int i = 0; i < 30; ++i) { last = 100.0 + 2.0 * i; vu.update(last); }
    ASSERT(vu.ready(), "vidya_ready");
    ASSERT(vu.value() > 0.0 && vu.value() < last, "vidya_uptrend_lags_below_price");
    const double before = vu.value();
    for (int i = 30; i < 40; ++i) { last = 100.0 + 2.0 * i; vu.update(last); }
    ASSERT(vu.value() > before, "vidya_uptrend_rises");
    ASSERT(vu.value() < last, "vidya_bounded_below_latest");  // an EMA never overshoots a ramp

    vu.reset();
    ASSERT(!vu.ready() && vu.value() == 0.0, "vidya_reset");
}

// CenterOfGravity #526 — Ehlers' zero-centered balance-point oscillator.
void test_cog() {
    SECTION("Center of Gravity (#526)");
    CenterOfGravity cf;                            // period 10
    for (int i = 0; i < 6; ++i) cf.update(100.0 + i);
    ASSERT(!cf.ready(), "cog_not_ready_during_warmup");

    // A flat window is perfectly centered -> COG exactly 0.
    CenterOfGravity cc(3);
    cc.update(10.0); cc.update(10.0); cc.update(10.0);
    ASSERT(cc.ready() && std::fabs(cc.value()) < 1e-12, "cog_flat_zero");

    // Rising window (recent heavier) -> COG positive; period 3, 10/20/30 -> +1/3.
    CenterOfGravity cu(3);
    cu.update(10.0); cu.update(20.0); cu.update(30.0);
    ASSERT(std::fabs(cu.value() - 1.0 / 3.0) < 1e-9, "cog_rising_plus_third");

    // The falling window is the exact mirror -> -1/3.
    CenterOfGravity cd(3);
    cd.update(30.0); cd.update(20.0); cd.update(10.0);
    ASSERT(std::fabs(cd.value() + 1.0 / 3.0) < 1e-9, "cog_falling_minus_third");
    ASSERT(std::fabs(cu.value() + cd.value()) < 1e-12, "cog_symmetric");

    cu.reset();
    ASSERT(!cu.ready() && cu.value() == 0.0, "cog_reset");
}

// Ensemble #140 — voting of signals (agreement >= min_agree).
void test_ensemble() {
    SECTION("Signal Ensemble (#140)");
    auto mk = [](Side s, int32_t q) {
        Signal sig; sig.valid = true; sig.side = s; sig.quantity = q; sig.price = 100.0;
        std::strncpy(sig.stock, "X", 8); return sig;
    };
    // 2 BUY vs 1 SELL, min_agree 2 -> BUY with total qty 150
    Signal arr[3] = { mk(Side::BUY, 100), mk(Side::BUY, 50), mk(Side::SELL, 80) };
    const Signal r = combine_signals(arr, 3, 2);
    ASSERT(r.valid && r.side == Side::BUY && r.quantity == 150, "ensemble_majority_buy");
    // 1 vs 1, min_agree 2 -> no agreement -> HOLD
    Signal tie[2] = { mk(Side::BUY, 100), mk(Side::SELL, 100) };
    ASSERT(!combine_signals(tie, 2, 2).valid, "ensemble_tie_holds");
    // min_agree 3 but only 2 BUY -> HOLD
    ASSERT(!combine_signals(arr, 3, 3).valid, "ensemble_below_threshold_holds");
}

// TrailingStop #147 — stop kroczacy (ratchet + wyjscie przy odwroceniu).
void test_trailing_stop() {
    SECTION("Trailing Stop (#147)");
    auto close = [](double a, double b) { const double d = a - b; return (d<0?-d:d) < 1e-9; };
    // long, entry 100, trail 5 -> stop 95
    TrailingStop ts(true, 100.0, 5.0);
    ASSERT(close(ts.stop(), 95.0), "ts_initial_stop");
    ASSERT(!ts.update(110.0), "ts_no_stop_on_rise");     // stop ratchet -> 105
    ASSERT(close(ts.stop(), 105.0), "ts_ratchets_up");
    ASSERT(!ts.update(108.0), "ts_stop_holds");          // 108-5=103 < 105 -> stop 105
    ASSERT(close(ts.stop(), 105.0), "ts_no_loosen");
    ASSERT(ts.update(105.0), "ts_stopped_out");          // 105 <= 105 -> exit
    ASSERT(!ts.active(), "ts_inactive_after");
    ASSERT(!ts.update(90.0), "ts_no_retrigger");

    // short, entry 100, trail 5 -> stop 105; a drop tightens, a rise triggers
    TrailingStop ss(false, 100.0, 5.0);
    ASSERT(close(ss.stop(), 105.0), "ts_short_initial");
    ASSERT(!ss.update(90.0), "ts_short_ratchet");        // stop -> 95
    ASSERT(close(ss.stop(), 95.0), "ts_short_ratchets_down");
    ASSERT(ss.update(95.0), "ts_short_stopped");         // 95 >= 95 -> exit
}

// POV #99 — Percentage-of-Volume execution algo (slicing adaptive to volume).
void test_pov_algo() {
    SECTION("POV Execution Algo (#99)");
    POVExecutor pov(1000, 0.10);                       // parent 1000, 10% of volume
    ASSERT(pov.on_market_volume(2000) == 200, "pov_slice_10pct_200");
    ASSERT(pov.remaining() == 800, "pov_remaining_800");
    ASSERT(pov.on_market_volume(10000) == 800, "pov_capped_to_remaining");  // 1000→800
    ASSERT(pov.done(), "pov_done");
    ASSERT(pov.on_market_volume(5000) == 0, "pov_zero_after_done");
    ASSERT(pov.slices() == 2, "pov_slice_count");

    POVExecutor low(1000, 0.10);
    ASSERT(low.on_market_volume(4) == 0, "pov_tiny_vol_no_slice");   // 0.4 < 0.5
}

// SignalThrottle #104 — minimum interval between signals per symbol.
void test_signal_throttle() {
    SECTION("Signal Throttle (#104)");
    SignalThrottle th(5);                                  // min 5 sekwencji
    ASSERT(th.allow("AAPL", 0), "throttle_first_passes");  // the first one always
    ASSERT(!th.allow("AAPL", 3), "throttle_too_soon");      // 3 < 5 -> stlumiony
    ASSERT(th.allow("AAPL", 5), "throttle_after_cooldown"); // 5 >= 5
    ASSERT(th.allow("MSFT", 1), "throttle_per_symbol_indep"); // inny symbol niezalezny
    ASSERT(th.suppressed() == 1, "throttle_suppressed_count");
    th.reset_symbol("AAPL");
    ASSERT(th.allow("AAPL", 6), "throttle_reset_symbol");   // after the reset it passes again
}

// VWAPTracker #113 — market VWAP + execution slippage in bps.
void test_vwap_tracker() {
    SECTION("VWAP Tracker (#113)");
    auto close = [](double a, double b) { const double d = a - b; return (d<0?-d:d) < 1e-3; };
    VWAPTracker v;
    v.on_trade(100.0, 100);
    v.on_trade(102.0, 100);                       // VWAP = (10000+10200)/200 = 101
    ASSERT(close(v.vwap(), 101.0), "vwap_value");
    ASSERT(v.volume() == 200, "vwap_volume");
    // BUY @102 vs VWAP 101 -> (102-101)/101*1e4 = +99.01 bps (worse)
    ASSERT(close(v.slippage_bps(102.0, true), 99.0099), "vwap_buy_slippage_positive");
    // SELL @102 vs VWAP 101 -> pobilismy VWAP -> ujemne
    ASSERT(v.slippage_bps(102.0, false) < 0.0, "vwap_sell_beats_negative");
    ASSERT(close(v.slippage_bps(101.0, true), 0.0), "vwap_at_vwap_zero");
}

// Bollinger #93 — mean-reversion adaptive to volatility (±k·σ bands).
void test_bollinger() {
    SECTION("Bollinger Strategy (#93)");
    // window=2, k=0.5: for 2 points b-mean == σ, so b>a by anything breaks
    // the band (k<1) → a deterministic signal.
    BollingerStrategy up(2, 0.5, 100);
    up.on_market_data("X", 100.0);
    const Signal su = up.on_market_data("X", 102.0);
    ASSERT(su.valid && su.side == Side::SELL, "bollinger_above_band_sells");

    BollingerStrategy dn(2, 0.5, 100);
    dn.on_market_data("Y", 100.0);
    const Signal sd = dn.on_market_data("Y", 98.0);
    ASSERT(sd.valid && sd.side == Side::BUY, "bollinger_below_band_buys");

    // Zero volatility (σ=0) → no signal (adaptation: a calm market = silence).
    BollingerStrategy flat(2, 0.5, 100);
    flat.on_market_data("Z", 100.0);
    const Signal sf = flat.on_market_data("Z", 100.0);
    ASSERT(!sf.valid, "bollinger_zero_vol_no_signal");
}

// Router #81 — EWMA zmierzonej latencji + partiale (unfilled_qty).
void test_router_ewma_partial() {
    SECTION("Router EWMA + Partials (#81)");

    // --- EWMA: A statycznie szybsze, ale w realu zwalnia → re-route na B ---
    {
        SmartOrderRouter r(RoutingStrategy::LOWEST_LATENCY);
        r.add_venue(Venue("A", 100, 0.0));   // static mean 100ns
        r.add_venue(Venue("B", 200, 0.0));   // static mean 200ns
        r.update_quote("A", 10.0, 11.0, 100, 100);
        r.update_quote("B", 10.0, 11.0, 100, 100);
        ASSERT(std::strcmp(r.route_order("BUY", 10).venue, "A") == 0, "ewma_static_picks_A");
        for (int i = 0; i < 5; ++i) { r.record_latency("A", 900); r.record_latency("B", 150); }
        ASSERT(std::strcmp(r.route_order("BUY", 10).venue, "B") == 0, "ewma_reroutes_to_B");
    }

    // --- Partial single-venue: order > top-of-book size ---
    {
        SmartOrderRouter r(RoutingStrategy::BEST_PRICE);
        r.add_venue(Venue("V", 100, 0.0));
        r.update_quote("V", 10.0, 11.0, 50, 50);   // only 50 available
        const RouteDecision d = r.route_order("BUY", 200);
        ASSERT(d.valid && d.quantity == 50, "partial_filled_50");
        ASSERT(d.unfilled_qty == 150, "partial_unfilled_150");
    }

    // --- Split shortfall: Σliquidity < order ---
    {
        SmartOrderRouter r(RoutingStrategy::SPLIT, 100);
        r.add_venue(Venue("X", 100, 0.0));
        r.add_venue(Venue("Y", 100, 0.0));
        r.update_quote("X", 10.0, 11.0, 100, 100);
        r.update_quote("Y", 10.0, 11.0, 80, 80);
        const RouteDecision d = r.route_order("BUY", 500);
        ASSERT(d.quantity == 180, "split_filled_180");
        ASSERT(d.unfilled_qty == 320, "split_unfilled_320");
        ASSERT(d.num_venues == 2, "split_two_venues");
    }

    // --- #86 venue health: a streak of rejects disables a venue, a success reactivates ---
    {
        SmartOrderRouter r(RoutingStrategy::BEST_PRICE);
        r.set_failure_threshold(3);
        r.add_venue(Venue("A", 100, 0.0));     // lepszy fee
        r.add_venue(Venue("B", 100, 0.001));
        r.update_quote("A", 10.0, 11.0, 100, 100);
        r.update_quote("B", 10.0, 11.0, 100, 100);
        ASSERT(std::strcmp(r.route_order("BUY", 10).venue, "A") == 0, "health_default_A");
        r.record_reject("A"); r.record_reject("A"); r.record_reject("A");
        ASSERT(!r.venue_active("A"), "health_A_disabled");
        ASSERT(std::strcmp(r.route_order("BUY", 10).venue, "B") == 0, "health_reroute_B");
        r.record_success("A");
        ASSERT(r.venue_active("A"), "health_A_recovered");
        ASSERT(std::strcmp(r.route_order("BUY", 10).venue, "A") == 0, "health_A_back");
    }

    // --- #97 NBBO: best bid/ask/mid agregowane po venue ---
    {
        auto close = [](double a, double b) { const double d = a - b; return (d<0?-d:d) < 1e-6; };
        SmartOrderRouter r(RoutingStrategy::BEST_PRICE);
        r.add_venue(Venue("A", 100, 0.0));
        r.add_venue(Venue("B", 100, 0.0));
        r.add_venue(Venue("C", 100, 0.0));
        r.update_quote("A", 99.98, 100.04, 100, 100);
        r.update_quote("B", 100.00, 100.03, 100, 100);   // best bid
        r.update_quote("C", 99.99, 100.02, 100, 100);    // best ask
        ASSERT(close(r.national_best_bid(), 100.00), "nbbo_best_bid");
        ASSERT(close(r.national_best_ask(), 100.02), "nbbo_best_ask");
        ASSERT(close(r.nbbo_mid(), 100.01), "nbbo_mid");
    }

    // --- #318 nbbo_microprice: size-weighted consolidated mid across venues ---
    {
        auto closem = [](double a, double b) { const double d = a - b; return (d<0?-d:d) < 1e-6; };
        SmartOrderRouter rmic(RoutingStrategy::BEST_PRICE);
        rmic.add_venue(Venue("A", 100, 0.0));
        rmic.add_venue(Venue("C", 100, 0.0));
        // NBB 100.00 @ size 300 (venue A); NBO 100.02 @ size 100 (venue C).
        rmic.update_quote("A", 100.00, 100.04, 300, 50);
        rmic.update_quote("C", 99.99, 100.02, 80, 100);
        // micro = (NBB*ask_sz + NBO*bid_sz)/(bid_sz+ask_sz)
        //       = (100.00*100 + 100.02*300)/400 = 100.015 (>mid 100.01, bid-heavy)
        ASSERT(closem(rmic.nbbo_microprice(), 100.015), "router_nbbo_microprice");
        ASSERT(rmic.nbbo_microprice() > rmic.nbbo_mid(), "router_micro_above_mid");
        // Balanced sizes -> microprice == mid.
        SmartOrderRouter rbal(RoutingStrategy::BEST_PRICE);
        rbal.add_venue(Venue("A", 100, 0.0));
        rbal.update_quote("A", 100.00, 100.02, 100, 100);
        ASSERT(closem(rbal.nbbo_microprice(), rbal.nbbo_mid()), "router_micro_balanced_eq_mid");
        // No two-sided liquidity -> 0.
        SmartOrderRouter remp(RoutingStrategy::BEST_PRICE);
        ASSERT(remp.nbbo_microprice() == 0.0, "router_micro_empty_zero");
    }

    // --- #326 nbbo_imbalance: consolidated top-of-book pressure across venues ---
    {
        auto closei = [](double a, double b) { const double d = a - b; return (d<0?-d:d) < 1e-9; };
        // NBB 100.00 sz 300 (A), NBO 100.02 sz 100 (C) -> (300-100)/400 = 0.5 (bid-heavy)
        SmartOrderRouter rib(RoutingStrategy::BEST_PRICE);
        rib.add_venue(Venue("A", 100, 0.0));
        rib.add_venue(Venue("C", 100, 0.0));
        rib.update_quote("A", 100.00, 100.04, 300, 50);
        rib.update_quote("C", 99.99, 100.02, 80, 100);
        ASSERT(closei(rib.nbbo_imbalance(), 0.5), "router_nbbo_imb_bid_heavy");

        // sizes summed across venues quoting the SAME NBB / NBO price
        SmartOrderRouter rsum(RoutingStrategy::BEST_PRICE);
        rsum.add_venue(Venue("A", 100, 0.0));
        rsum.add_venue(Venue("B", 100, 0.0));
        rsum.add_venue(Venue("C", 100, 0.0));
        rsum.update_quote("A", 100.00, 100.05, 200, 100);   // bid at NBB
        rsum.update_quote("B", 100.00, 100.02, 300, 100);   // bid at NBB, ask at NBO
        rsum.update_quote("C", 99.98,  100.02, 90,  150);   // ask at NBO (bid below NBB, excluded)
        // NBB 100.00 sz 200+300=500 ; NBO 100.02 sz 100+150=250 -> (500-250)/750 = 1/3
        ASSERT(closei(rsum.nbbo_imbalance(), 250.0/750.0), "router_nbbo_imb_summed");
        // #367 venues_at_nbbo: A+B at NBB 100.00 (C's bid 99.98 excluded) -> 2;
        // B+C at NBO 100.02 (A's ask 100.05 excluded) -> 2.
        ASSERT(rsum.venues_at_nbbo(true) == 2, "router_vatn_bid_two");
        ASSERT(rsum.venues_at_nbbo(false) == 2, "router_vatn_ask_two");
        // #480 nbbo_depth: BUY takes asks at NBO 100.02 -> B 100 + C 150 = 250;
        // SELL takes bids at NBB 100.00 -> A 200 + B 300 = 500.
        ASSERT(rsum.nbbo_depth(true) == 250, "router_nbbo_depth_ask");
        ASSERT(rsum.nbbo_depth(false) == 500, "router_nbbo_depth_bid");
        // Distinct from available_liquidity (#109): A's ask (100 @ 100.05) is
        // top-of-book but NOT at the NBO, so it counts there but not here.
        ASSERT(rsum.available_liquidity(true) > rsum.nbbo_depth(true),
               "router_nbbo_depth_excludes_off_touch");
        rsum.set_venue_active("B", false);   // B was at both NBB and NBO
        // NBB now only A -> 1; NBO now only C -> 1 (fragile touch).
        ASSERT(rsum.venues_at_nbbo(true) == 1, "router_vatn_bid_one_after_disable");
        ASSERT(rsum.venues_at_nbbo(false) == 1, "router_vatn_ask_one_after_disable");
        // #480: disabling B shrinks the touch depth to the lone remaining venue.
        ASSERT(rsum.nbbo_depth(true) == 150 && rsum.nbbo_depth(false) == 200,
               "router_nbbo_depth_after_disable");

        // balanced sizes -> 0
        SmartOrderRouter rbl(RoutingStrategy::BEST_PRICE);
        rbl.add_venue(Venue("A", 100, 0.0));
        rbl.update_quote("A", 100.00, 100.02, 100, 100);
        ASSERT(closei(rbl.nbbo_imbalance(), 0.0), "router_nbbo_imb_balanced_zero");

        // one-sided / empty -> 0
        SmartOrderRouter rem(RoutingStrategy::BEST_PRICE);
        ASSERT(rem.nbbo_imbalance() == 0.0, "router_nbbo_imb_empty_zero");
        ASSERT(rem.venues_at_nbbo(true) == 0 && rem.venues_at_nbbo(false) == 0,
               "router_vatn_empty_zero");
        // #480: empty router has no touch depth.
        ASSERT(rem.nbbo_depth(true) == 0 && rem.nbbo_depth(false) == 0, "router_nbbo_depth_empty");
    }

    // --- #376 liquidity_at_limit: marketable size at a limit price ---
    {
        SmartOrderRouter rlal(RoutingStrategy::BEST_PRICE);
        rlal.add_venue(Venue("A", 100, 0.0));
        rlal.add_venue(Venue("B", 100, 0.0));
        rlal.add_venue(Venue("C", 100, 0.0));
        rlal.update_quote("A", 99.98, 100.02, 200, 150);
        rlal.update_quote("B", 99.99, 100.04, 300, 250);
        rlal.update_quote("C", 100.00, 100.06, 100, 120);
        // BUY limit 100.04: asks 100.02 (150) + 100.04 (250) qualify, 100.06 out.
        ASSERT(rlal.liquidity_at_limit(true, 100.04) == 400, "router_lal_buy_two_venues");
        // BUY limit exactly at the best ask (zero fee -> all-in == quote): only A.
        ASSERT(rlal.liquidity_at_limit(true, 100.02) == 150, "router_lal_buy_exact_touch");
        // Non-marketable limit -> 0 shares; agrees with is_marketable (#184).
        ASSERT(rlal.liquidity_at_limit(true, 100.01) == 0, "router_lal_buy_none");
        ASSERT(!rlal.is_marketable(true, 100.01), "router_lal_agrees_marketable_no");
        ASSERT(rlal.is_marketable(true, 100.02), "router_lal_agrees_marketable_yes");
        // SELL limit 99.99: bids 100.00 (100) + 99.99 (300) qualify, 99.98 out.
        ASSERT(rlal.liquidity_at_limit(false, 99.99) == 400, "router_lal_sell_two_venues");
        // Limit crossing the whole book -> equals available_liquidity (#109).
        ASSERT(rlal.liquidity_at_limit(true, 101.00) == rlal.available_liquidity(true),
               "router_lal_cross_equals_available");
        // Inactive venue excluded from the sum.
        rlal.set_venue_active("B", false);
        ASSERT(rlal.liquidity_at_limit(true, 100.04) == 150, "router_lal_excludes_inactive");

        // Taker fee shifts the all-in price: ask 100.00 + 0.03/share fee is
        // 100.03 all-in -> out at a 100.02 limit, in at a 100.10 limit.
        SmartOrderRouter rlf(RoutingStrategy::BEST_PRICE);
        rlf.add_venue(Venue("F", 100, 0.03));
        rlf.update_quote("F", 99.90, 100.00, 200, 150);
        ASSERT(rlf.liquidity_at_limit(true, 100.02) == 0, "router_lal_fee_pushes_out");
        ASSERT(rlf.liquidity_at_limit(true, 100.10) == 150, "router_lal_fee_still_in");
        // SELL side: bid 99.90 - 0.03 = 99.87 all-in -> out at 99.89, in at 99.80.
        ASSERT(rlf.liquidity_at_limit(false, 99.89) == 0, "router_lal_fee_sell_out");
        ASSERT(rlf.liquidity_at_limit(false, 99.80) == 200, "router_lal_fee_sell_in");

        // Empty router -> 0.
        SmartOrderRouter remlal(RoutingStrategy::BEST_PRICE);
        ASSERT(remlal.liquidity_at_limit(true, 100.0) == 0, "router_lal_empty_zero");
    }

    // --- #109 available_liquidity: sum of top-of-book across active venues ---
    {
        SmartOrderRouter r(RoutingStrategy::BEST_PRICE);
        r.add_venue(Venue("A", 100, 0.0));
        r.add_venue(Venue("B", 100, 0.0));
        r.update_quote("A", 10.0, 11.0, 200, 150);   // bid 200, ask 150
        r.update_quote("B", 10.0, 11.0, 300, 250);
        ASSERT(r.available_liquidity(true) == 400, "liq_buy_sum_asks");   // 150+250
        ASSERT(r.available_liquidity(false) == 500, "liq_sell_sum_bids"); // 200+300
        r.record_reject("A"); r.record_reject("A"); r.record_reject("A"); // A disabled
        ASSERT(r.available_liquidity(true) == 250, "liq_excludes_inactive");
    }

    // --- #335 sweep_to_fill / sweep_slippage_bps: multi-venue price-priority sweep ---
    {
        SmartOrderRouter rs(RoutingStrategy::BEST_PRICE);
        rs.add_venue(Venue("A", 100, 0.0));
        rs.add_venue(Venue("B", 100, 0.0));
        rs.add_venue(Venue("C", 100, 0.0));
        rs.update_quote("A", 99.99, 100.00, 100, 100);   // best ask 100.00 sz 100
        rs.update_quote("B", 99.98, 100.01, 100, 200);   // next ask 100.01 sz 200
        rs.update_quote("C", 99.97, 100.02, 100, 300);   // next ask 100.02 sz 300
        double vwap = 0.0;
        // BUY 250: 100@100.00 + 150@100.01 = 25001.5 / 250 = 100.006
        ASSERT(rs.sweep_to_fill(true, 250, vwap) == 250, "router_sweep_filled_250");
        ASSERT(std::fabs(vwap - 100.006) < 1e-9, "router_sweep_vwap");
        // BUY 1000 > total 600 displayed -> fills only 600, vwap of full ask stack
        ASSERT(rs.sweep_to_fill(true, 1000, vwap) == 600, "router_sweep_capped_depth");
        ASSERT(std::fabs(vwap - (60008.0 / 600.0)) < 1e-9, "router_sweep_full_vwap");
        // slippage vs nbbo_mid: best bid 99.99 / best ask 100.00 -> mid 99.995.
        // BUY 100 sweeps only venue A @100.00 -> (100.00-99.995)/99.995*1e4 bps > 0
        const double slp = rs.sweep_slippage_bps(true, 100);
        ASSERT(slp > 0.0 && std::fabs(slp - (0.005/99.995*10000.0)) < 1e-6, "router_sweep_slip_buy");
        ASSERT(rs.sweep_slippage_bps(true, 0) == 0.0, "router_sweep_slip_zero_shares");
        SmartOrderRouter rse(RoutingStrategy::BEST_PRICE);
        ASSERT(rse.sweep_to_fill(true, 100, vwap) == 0 && vwap == 0.0, "router_sweep_empty_zero");

        // #343 venues_to_fill: same A/B/C book (asks 100.00(100), 100.01(200), 100.02(300)).
        ASSERT(rs.venues_to_fill(true, 0)    == 0,  "router_vtf_zero_shares");
        ASSERT(rs.venues_to_fill(true, 100)  == 1,  "router_vtf_one_venue");
        ASSERT(rs.venues_to_fill(true, 250)  == 2,  "router_vtf_two_venues");   // 100@A + 150@B
        ASSERT(rs.venues_to_fill(true, 1000) == -1, "router_vtf_insufficient"); // > 600 displayed
        ASSERT(rse.venues_to_fill(true, 100) == -1, "router_vtf_no_venues");
    }

    // --- #117 TCA: routed-volume per venue ---
    {
        SmartOrderRouter r(RoutingStrategy::BEST_PRICE);
        r.add_venue(Venue("A", 100, 0.0));
        r.add_venue(Venue("B", 100, 0.001));   // gorszy fee
        r.update_quote("A", 10.0, 11.0, 1000, 1000);
        r.update_quote("B", 10.0, 11.0, 1000, 1000);
        r.route_order("BUY", 100);             // A lepszy -> 100 na A
        r.route_order("BUY", 50);              // A -> +50
        ASSERT(r.venue_routed_shares("A") == 150, "tca_routed_A_150");
        ASSERT(r.venue_routed_shares("B") == 0, "tca_routed_B_0");
        // SPLIT divides between both (threshold 100)
        SmartOrderRouter rs(RoutingStrategy::SPLIT, 100);
        rs.add_venue(Venue("X", 100, 0.0));
        rs.add_venue(Venue("Y", 100, 0.0));
        rs.update_quote("X", 10.0, 11.0, 60, 60);
        rs.update_quote("Y", 10.0, 11.0, 100, 100);
        rs.route_order("BUY", 120);            // 60 z X + 60 z Y
        ASSERT(rs.venue_routed_shares("X") == 60, "tca_split_X_60");
        ASSERT(rs.venue_routed_shares("Y") == 60, "tca_split_Y_60");
        // #130 agregat + reset
        ASSERT(rs.total_routed_shares() == 120, "tca_total_120");

        // #456 venue_route_count / avg_route_size — the typical clip per venue.
        // A took two BEST_PRICE landings (100 + 50) of 150 shares -> avg 75.
        ASSERT(r.venue_route_count("A") == 2, "vrc_a_two_landings");
        ASSERT(std::fabs(r.avg_route_size("A") - 75.0) < 1e-9, "ars_a_avg_75");
        // B was quoted but never chosen: count 0, avg 0 (not a division trap).
        ASSERT(r.venue_route_count("B") == 0 && r.avg_route_size("B") == 0.0, "ars_b_never_routed");
        // Unknown venue -> -1 count sentinel, 0 avg.
        ASSERT(r.venue_route_count("GHOST") == -1 && r.avg_route_size("GHOST") == 0.0,
               "vrc_unknown_minus1");
        // SPLIT: X and Y each took one 60-share leg -> avg 60 apiece.
        ASSERT(rs.venue_route_count("X") == 1 && rs.venue_route_count("Y") == 1, "vrc_split_one_each");
        ASSERT(std::fabs(rs.avg_route_size("X") - 60.0) < 1e-9, "ars_split_60");

        // #384 routing_concentration — HHI over routed volume.
        // r sent all 150 shares to A -> fully concentrated, HHI 1.0.
        ASSERT(std::fabs(r.routing_concentration() - 1.0) < 1e-9, "rhhi_single_venue_one");
        // rs split 60/60 across X and Y -> even two-venue flow, HHI 0.5.
        ASSERT(std::fabs(rs.routing_concentration() - 0.5) < 1e-9, "rhhi_even_split_half");
        // Uneven flow: 90/120 and 30/120 -> 0.75^2 + 0.25^2 = 0.625.
        SmartOrderRouter rhu(RoutingStrategy::BEST_PRICE);
        rhu.add_venue(Venue("P", 100, 0.0));
        rhu.add_venue(Venue("Q", 100, 0.001));         // worse fee -> loses ties
        rhu.update_quote("P", 10.0, 11.0, 1000, 90);   // only 90 displayed
        rhu.update_quote("Q", 10.0, 11.0, 1000, 1000);
        rhu.route_order("BUY", 90);                    // all to P (better all-in)
        rhu.update_quote("P", 10.0, 12.0, 1000, 1000); // P now worse-priced
        rhu.route_order("BUY", 30);                    // all to Q
        ASSERT(rhu.venue_routed_shares("P") == 90 && rhu.venue_routed_shares("Q") == 30,
               "rhhi_uneven_setup");
        ASSERT(std::fabs(rhu.routing_concentration() - 0.625) < 1e-9, "rhhi_uneven_0625");
        // Nothing routed yet -> 0.
        SmartOrderRouter rhe(RoutingStrategy::BEST_PRICE);
        rhe.add_venue(Venue("Z", 100, 0.0));
        ASSERT(rhe.routing_concentration() == 0.0, "rhhi_nothing_routed_zero");

        rs.reset_routing_stats();
        ASSERT(rs.total_routed_shares() == 0, "tca_reset_zero");
        ASSERT(rs.routing_concentration() == 0.0, "rhhi_reset_zero");
        // #456: the reset zeroes the route count and average too.
        ASSERT(rs.venue_route_count("X") == 0 && rs.avg_route_size("X") == 0.0, "ars_reset_zero");
    }

    // --- #392 quote staleness: clock-injected freshness of the NBBO inputs ---
    {
        SmartOrderRouter rqs(RoutingStrategy::BEST_PRICE);
        rqs.add_venue(Venue("A", 100, 0.0));
        rqs.add_venue(Venue("B", 100, 0.0));
        rqs.add_venue(Venue("C", 100, 0.0));                  // never quotes
        ASSERT(rqs.venue_quote_age_ns("A", 999) == -1, "rqs_never_quoted_minus1");
        ASSERT(rqs.stalest_quote_age_ns(999) == -1, "rqs_no_quotes_minus1");
        rqs.update_quote("A", 10.0, 10.02, 100, 100, 1000);   // synthetic stamps
        rqs.update_quote("B", 10.0, 10.02, 100, 100, 5000);
        ASSERT(rqs.venue_quote_age_ns("A", 6000) == 5000, "rqs_age_a_exact");
        ASSERT(rqs.venue_quote_age_ns("B", 6000) == 1000, "rqs_age_b_exact");
        ASSERT(rqs.venue_quote_age_ns("GHOST", 6000) == -1, "rqs_unknown_minus1");
        ASSERT(rqs.venue_quote_age_ns("C", 6000) == -1, "rqs_c_never_minus1");
        // Stalest = A's 5000; the never-quoted C is excluded, not infinite.
        ASSERT(rqs.stalest_quote_age_ns(6000) == 5000, "rqs_stalest_is_a");
        // Refreshing A hands the worst age to B.
        rqs.update_quote("A", 10.0, 10.02, 100, 100, 5500);
        ASSERT(rqs.stalest_quote_age_ns(6000) == 1000, "rqs_refresh_moves_worst");
        // Disabling B removes it from the watch (A's 500 remains).
        rqs.set_venue_active("B", false);
        ASSERT(rqs.stalest_quote_age_ns(6000) == 500, "rqs_inactive_excluded");
        // A read clocked before the stamp clamps to 0.
        ASSERT(rqs.venue_quote_age_ns("A", 5000) == 0, "rqs_clamp_zero");
    }

    // --- #416 venue_failure_streak / least_healthy_venue ---
    {
        SmartOrderRouter rvh(RoutingStrategy::BEST_PRICE);
        rvh.set_failure_threshold(3);
        rvh.add_venue(Venue("A", 100, 0.0));
        rvh.add_venue(Venue("B", 100, 0.0));
        int32_t rvh_streak = -99;
        ASSERT(rvh.venue_failure_streak("A") == 0, "rvh_fresh_zero");
        ASSERT(rvh.venue_failure_streak("GHOST") == -1, "rvh_unknown_minus1");
        ASSERT(rvh.least_healthy_venue(rvh_streak) == nullptr && rvh_streak == -99,
               "rvh_all_healthy_null");
        rvh.record_reject("B");
        rvh.record_reject("B");                    // streak 2 of 3 — still active
        ASSERT(rvh.venue_failure_streak("B") == 2 && rvh.venue_active("B"),
               "rvh_streak_visible_before_trip");
        const char* rvh_who = rvh.least_healthy_venue(rvh_streak);
        ASSERT(rvh_who != nullptr && std::strcmp(rvh_who, "B") == 0 && rvh_streak == 2,
               "rvh_names_b");
        // A success wipes the streak — the alarm clears.
        rvh.record_success("B");
        ASSERT(rvh.venue_failure_streak("B") == 0
               && rvh.least_healthy_venue(rvh_streak) == nullptr, "rvh_success_clears");
        // The third reject trips the breaker: the venue leaves the ACTIVE
        // pool and with it the least-healthy watch (it is already gone).
        rvh.record_reject("A"); rvh.record_reject("A"); rvh.record_reject("A");
        ASSERT(!rvh.venue_active("A"), "rvh_third_reject_disables");
        ASSERT(rvh.venue_failure_streak("A") == 3, "rvh_streak_readable_after_trip");
        ASSERT(rvh.least_healthy_venue(rvh_streak) == nullptr, "rvh_disabled_excluded");
    }

    // --- #424 fresh_nbbo_mid — the staleness-gated NBBO ---
    {
        auto closf = [](double a, double b) { const double d = a - b; return (d < 0 ? -d : d) < 1e-9; };
        SmartOrderRouter rfm(RoutingStrategy::BEST_PRICE);
        rfm.add_venue(Venue("A", 100, 0.0));
        rfm.add_venue(Venue("B", 100, 0.0));
        rfm.add_venue(Venue("C", 100, 0.0));                   // never quotes
        ASSERT(rfm.fresh_nbbo_mid(1000, 5000) == 0.0, "rfm_no_quotes_zero");
        rfm.update_quote("A", 10.00, 10.06, 100, 100, 4000);   // best bid, fresh
        rfm.update_quote("B", 9.98, 10.02, 100, 100, 4500);    // best ask, fresher
        // Everything fresh at t=5000 with a 2000ns budget -> equals nbbo_mid.
        ASSERT(closf(rfm.fresh_nbbo_mid(2000, 5000), rfm.nbbo_mid()), "rfm_all_fresh_matches");
        ASSERT(closf(rfm.fresh_nbbo_mid(2000, 5000), (10.00 + 10.02) / 2.0), "rfm_mid_value");
        // A tighter budget (A's age 1000 > 600, B's 500 <= 600) drops venue
        // A ENTIRELY: the fresh NBB falls back to B's 9.98.
        ASSERT(closf(rfm.fresh_nbbo_mid(600, 5000), (9.98 + 10.02) / 2.0), "rfm_stale_bid_dropped");
        // The raw nbbo_mid keeps blending the stale bid — the divergence IS
        // the mispricing this guards against.
        ASSERT(rfm.nbbo_mid() > rfm.fresh_nbbo_mid(600, 5000), "rfm_raw_blends_stale");
        // No fresh two-sided market -> 0 (not tradeable).
        ASSERT(rfm.fresh_nbbo_mid(100, 99999) == 0.0, "rfm_all_stale_zero");

        // #432 fresh_available_liquidity — the SIZE companion to #424.
        // All fresh at a 2000ns budget: both asks count (100 + 100).
        ASSERT(rfm.fresh_available_liquidity(true, 2000, 5000) == 200, "rfl_all_fresh");
        ASSERT(rfm.fresh_available_liquidity(true, 2000, 5000)
               == rfm.available_liquidity(true), "rfl_matches_raw_when_fresh");
        // The 600ns budget drops venue A (age 1000): its ask size is phantom.
        ASSERT(rfm.fresh_available_liquidity(true, 600, 5000) == 100, "rfl_stale_dropped");
        // The gap vs #109 is exactly the phantom-size share of the touch.
        ASSERT(rfm.available_liquidity(true)
               - rfm.fresh_available_liquidity(true, 600, 5000) == 100, "rfl_phantom_gap");
        ASSERT(rfm.fresh_available_liquidity(false, 600, 5000) == 100, "rfl_sell_side");
        // Everything stale -> nothing sweepable.
        ASSERT(rfm.fresh_available_liquidity(true, 100, 99999) == 0, "rfl_all_stale_zero");

        // An inactive venue is out regardless of freshness.
        rfm.set_venue_active("B", false);
        ASSERT(closf(rfm.fresh_nbbo_mid(2000, 5000), (10.00 + 10.06) / 2.0),
               "rfm_inactive_excluded");
        ASSERT(rfm.fresh_available_liquidity(true, 2000, 5000) == 100, "rfl_inactive_excluded");
        rfm.set_venue_active("B", true);

        // #440 sweep_to_fill_fresh — the phantom-free sweep plan (MILESTONE 440).
        double rsf_vwap = -1.0;
        // Everything fresh: identical to the unguarded sweep.
        double rsf_raw = 0.0;
        const int32_t rsf_all = rfm.sweep_to_fill(true, 150, rsf_raw);
        ASSERT(rfm.sweep_to_fill_fresh(true, 150, 2000, 5000, rsf_vwap) == rsf_all
               && closf(rsf_vwap, rsf_raw), "rsf_all_fresh_matches_raw");
        // B (ask 10.02 x 100) fresh, A (ask 10.06 x 100) stale at 600ns:
        // the plan takes B first and CANNOT finish 150 from fresh venues.
        ASSERT(rfm.sweep_to_fill_fresh(true, 150, 600, 5000, rsf_vwap) == 100,
               "rsf_stale_leg_dropped");
        ASSERT(closf(rsf_vwap, 10.02), "rsf_vwap_fresh_only");
        // The raw sweep happily books the phantom 50 at A's stale 10.06.
        ASSERT(rfm.sweep_to_fill(true, 150, rsf_raw) == 150, "rsf_raw_books_phantom");
        // Nothing fresh -> nothing planned, vwap zeroed.
        ASSERT(rfm.sweep_to_fill_fresh(true, 150, 100, 99999, rsf_vwap) == 0
               && rsf_vwap == 0.0, "rsf_all_stale_zero");
        // Zero-size request is a no-op.
        ASSERT(rfm.sweep_to_fill_fresh(true, 0, 2000, 5000, rsf_vwap) == 0, "rsf_zero_shares");

        // #448 fresh_venue_count — the breadth behind the fresh picture.
        // A (age 1000) + B (age 500) quoted; C never did.
        ASSERT(rfm.fresh_venue_count(2000, 5000) == 2, "rfc_both_fresh");
        // The 600ns budget leaves the picture resting on B alone — the
        // fragile single-venue case the count exists to expose.
        ASSERT(rfm.fresh_venue_count(600, 5000) == 1, "rfc_single_venue_fragile");
        ASSERT(rfm.fresh_venue_count(100, 99999) == 0, "rfc_all_stale");
        // Inactive and never-quoted venues never count.
        rfm.set_venue_active("B", false);
        ASSERT(rfm.fresh_venue_count(2000, 5000) == 1, "rfc_inactive_excluded");
        rfm.set_venue_active("B", true);
    }

    // --- #400 sweep_to_fill_at_limit — the marketable-limit sweep planner ---
    {
        auto closr = [](double a, double b) { const double d = a - b; return (d < 0 ? -d : d) < 1e-9; };
        SmartOrderRouter rsl(RoutingStrategy::BEST_PRICE);
        rsl.add_venue(Venue("A", 100, 0.0));
        rsl.add_venue(Venue("B", 100, 0.0));
        rsl.add_venue(Venue("C", 100, 0.0));
        rsl.update_quote("A", 9.99, 10.02, 300, 150);
        rsl.update_quote("B", 10.00, 10.04, 100, 250);
        rsl.update_quote("C", 9.98, 10.06, 200, 120);
        double rsl_vwap = -1.0;
        // BUY limit 10.04: A (150 @ 10.02) + B (250 @ 10.04); C excluded.
        ASSERT(rsl.sweep_to_fill_at_limit(true, 1000, 10.04, rsl_vwap) == 400,
               "rsl_buy_limit_caps_fill");
        ASSERT(closr(rsl_vwap, (10.02 * 150 + 10.04 * 250) / 400.0), "rsl_buy_vwap_blended");
        // Given enough shares the fill equals liquidity_at_limit (#376).
        ASSERT(rsl.sweep_to_fill_at_limit(true, 1000000, 10.04, rsl_vwap)
               == rsl.liquidity_at_limit(true, 10.04), "rsl_matches_liquidity_at_limit");
        // The share cap binds before the limit does: 150 @ A + 50 @ B.
        ASSERT(rsl.sweep_to_fill_at_limit(true, 200, 10.04, rsl_vwap) == 200,
               "rsl_share_cap_binds");
        ASSERT(closr(rsl_vwap, (10.02 * 150 + 10.04 * 50) / 200.0), "rsl_share_cap_vwap");
        // A limit below the best ask fills nothing and zeroes the VWAP.
        ASSERT(rsl.sweep_to_fill_at_limit(true, 100, 10.01, rsl_vwap) == 0 && rsl_vwap == 0.0,
               "rsl_unmarketable_zero");
        // SELL limit 9.99: B (100 @ 10.00) + A (300 @ 9.99); C's 9.98 excluded.
        ASSERT(rsl.sweep_to_fill_at_limit(false, 1000, 9.99, rsl_vwap) == 400,
               "rsl_sell_limit_caps_fill");
        ASSERT(closr(rsl_vwap, (10.00 * 100 + 9.99 * 300) / 400.0), "rsl_sell_vwap_blended");
        // Fees gate on the ALL-IN price and price the VWAP: ask 10.00 with a
        // 0.03 taker fee is 10.03 all-in -> out at 10.02, in at 10.05 where
        // the blended cost is the all-in 10.03, not the raw 10.00.
        SmartOrderRouter rslf(RoutingStrategy::BEST_PRICE);
        rslf.add_venue(Venue("F", 100, 0.03));
        rslf.update_quote("F", 9.90, 10.00, 200, 150);
        ASSERT(rslf.sweep_to_fill_at_limit(true, 100, 10.02, rsl_vwap) == 0,
               "rsl_fee_gates_all_in");
        ASSERT(rslf.sweep_to_fill_at_limit(true, 100, 10.05, rsl_vwap) == 100
               && closr(rsl_vwap, 10.03), "rsl_fee_priced_all_in");

        // #408 venues_to_fill_at_limit — the venue-count face of #400.
        // rsl asks within a 10.04 limit: A 150 + B 250 (C's 10.06 gated out).
        ASSERT(rsl.venues_to_fill_at_limit(true, 150, 10.04) == 1, "rvtl_touch_absorbs");
        ASSERT(rsl.venues_to_fill_at_limit(true, 151, 10.04) == 2, "rvtl_spills_to_second");
        ASSERT(rsl.venues_to_fill_at_limit(true, 400, 10.04) == 2, "rvtl_exact_capacity");
        // 401 shares cannot fill within the limit (C is gated) -> -1.
        ASSERT(rsl.venues_to_fill_at_limit(true, 401, 10.04) == -1, "rvtl_insufficient_minus1");
        // Lifting the limit admits C: the same 401 now needs 3 venues.
        ASSERT(rsl.venues_to_fill_at_limit(true, 401, 10.06) == 3, "rvtl_lifted_limit_three");
        // Consistency with #400: fillable shares -> a venue count, not -1.
        ASSERT(rsl.sweep_to_fill_at_limit(true, 400, 10.04, rsl_vwap) == 400
               && rsl.venues_to_fill_at_limit(true, 400, 10.04) > 0, "rvtl_consistent_with_400");
        // SELL mirror within a 9.99 limit: B 100 + A 300 (C's 9.98 gated out).
        ASSERT(rsl.venues_to_fill_at_limit(false, 100, 9.99) == 1, "rvtl_sell_touch");
        ASSERT(rsl.venues_to_fill_at_limit(false, 400, 9.99) == 2, "rvtl_sell_two");
        ASSERT(rsl.venues_to_fill_at_limit(false, 401, 9.99) == -1, "rvtl_sell_insufficient");
        ASSERT(rsl.venues_to_fill_at_limit(true, 0, 10.04) == 0, "rvtl_zero_shares");
    }

    // --- #138 cumulative fee cost ---
    {
        auto close = [](double a, double b) { const double d = a - b; return (d<0?-d:d) < 1e-6; };
        SmartOrderRouter r(RoutingStrategy::BEST_PRICE);
        r.add_venue(Venue("A", 100, 0.002));         // taker fee 0.002/share
        r.update_quote("A", 10.0, 11.0, 1000, 1000);
        r.route_order("BUY", 100);                    // fee 0.2
        r.route_order("BUY", 50);                     // fee 0.1
        ASSERT(close(r.total_fees_paid(), 0.3), "fees_paid_cumulative");
    }

    // --- #146 manualne wlacz/wylacz venue + EWMA getter ---
    {
        auto close = [](double a, double b) { const double d = a - b; return (d<0?-d:d) < 1e-6; };
        SmartOrderRouter r(RoutingStrategy::BEST_PRICE);
        r.add_venue(Venue("A", 100, 0.0));
        r.add_venue(Venue("B", 100, 0.001));
        r.update_quote("A", 10.0, 11.0, 100, 100);
        r.update_quote("B", 10.0, 11.0, 100, 100);
        ASSERT(std::strcmp(r.route_order("BUY", 10).venue, "A") == 0, "setactive_default_A");
        ASSERT(r.set_venue_active("A", false), "setactive_disable_A");
        ASSERT(!r.venue_active("A"), "setactive_A_off");
        ASSERT(std::strcmp(r.route_order("BUY", 10).venue, "B") == 0, "setactive_reroute_B");
        r.set_venue_active("A", true);
        ASSERT(r.venue_active("A"), "setactive_reenable_A");
        ASSERT(!r.set_venue_active("GHOST", true), "setactive_unknown_false");
        r.record_latency("A", 500);
        ASSERT(close(r.venue_ewma_latency("A"), 500.0), "venue_ewma_getter");
    }

    // --- #154 best_effective_price + active_venue_count ---
    {
        auto close = [](double a, double b) { const double d = a - b; return (d<0?-d:d) < 1e-6; };
        SmartOrderRouter r(RoutingStrategy::BEST_PRICE);
        r.add_venue(Venue("A", 100, 0.005));         // ask 11 + fee 0.005 = 11.005
        r.add_venue(Venue("B", 100, 0.001));         // ask 11 + fee 0.001 = 11.001 (better)
        r.update_quote("A", 10.0, 11.0, 100, 100);
        r.update_quote("B", 10.0, 11.0, 100, 100);
        ASSERT(r.active_venue_count() == 2, "best_eff_active_2");
        ASSERT(close(r.best_effective_price(true), 11.001), "best_eff_buy_B");   // min all-in
        // SELL: bid 10 - fee; B: 10-0.001=9.999, A: 10-0.005=9.995 -> max 9.999
        ASSERT(close(r.best_effective_price(false), 9.999), "best_eff_sell_B");
        // #464 effective_price_dispersion — best (#155) vs worst all-in gap.
        // BUY: 11.005 - 11.001 = 0.004; SELL: 9.999 - 9.995 = 0.004 (the fee
        // spread, since both quote the same raw price).
        ASSERT(close(r.effective_price_dispersion(true), 0.004), "epd_buy_fee_spread");
        ASSERT(close(r.effective_price_dispersion(false), 0.004), "epd_sell_fee_spread");
        // #359 liquidity_venue_count: both A and B quote both sides -> 2 each.
        ASSERT(r.liquidity_venue_count(true) == 2 && r.liquidity_venue_count(false) == 2,
               "liqvenue_both_quoting");
        r.set_venue_active("B", false);
        ASSERT(r.active_venue_count() == 1, "best_eff_active_1_after_disable");
        ASSERT(close(r.best_effective_price(true), 11.005), "best_eff_buy_A_only");
        // #464: one venue quoting -> nothing to disperse.
        ASSERT(r.effective_price_dispersion(true) == 0.0, "epd_single_venue_zero");
        // #359 only A active now -> 1 quoting venue per side.
        ASSERT(r.liquidity_venue_count(true) == 1, "liqvenue_one_after_disable");
        // A active but no quote on a side (fresh venue C) -> not counted.
        r.add_venue(Venue("C", 100, 0.0));           // active but never quoted
        ASSERT(r.liquidity_venue_count(true) == 1, "liqvenue_unquoted_not_counted");
        SmartOrderRouter empt(RoutingStrategy::BEST_PRICE);
        ASSERT(empt.liquidity_venue_count(true) == 0, "liqvenue_empty_zero");
        // #464: an empty router disperses nothing.
        ASSERT(empt.effective_price_dispersion(true) == 0.0, "epd_empty_zero");
    }

    // --- #496 cheapest_fee_venue — best fee tier, distinct from best price ---
    {
        auto close = [](double a, double b) { const double d = a - b; return (d<0?-d:d) < 1e-9; };
        SmartOrderRouter rcf(RoutingStrategy::BEST_PRICE);
        rcf.add_venue(Venue("A", 100, 0.003));    // taker fee
        rcf.add_venue(Venue("B", 100, 0.001));    // cheaper taker
        rcf.add_venue(Venue("C", 100, -0.002));   // maker rebate (best fee)
        rcf.update_quote("A", 9.99, 10.00, 100, 100);   // all-in ask 10.003
        rcf.update_quote("B", 9.99, 10.00, 100, 100);   // all-in ask 10.001 (cheapest to CROSS)
        rcf.update_quote("C", 9.99, 10.10, 100, 100);   // bad quote, all-in 10.098
        double cf = 999.0, df = 999.0;
        // cheapest_fee_venue picks by FEE alone -> C (the rebate).
        ASSERT(std::strcmp(rcf.cheapest_fee_venue(cf), "C") == 0, "rcf_best_fee_C");
        ASSERT(close(cf, -0.002), "rcf_best_fee_value");
        // #520 dearest_fee_venue — the mirror: the HIGHEST fee -> A (0.003).
        ASSERT(std::strcmp(rcf.dearest_fee_venue(df), "A") == 0 && close(df, 0.003), "rdf_dearest_A");
        // #504 rebate_venue_count / avg_venue_fee: only C rebates -> 1;
        // mean fee (0.003 + 0.001 - 0.002)/3 = 0.002/3.
        ASSERT(rcf.rebate_venue_count() == 1, "rvf_one_rebate");
        ASSERT(close(rcf.avg_venue_fee(), 0.002 / 3.0), "rvf_avg_fee");
        // cheapest_venue (#200) picks by ALL-IN price -> B (C's quote is bad).
        ASSERT(std::strcmp(rcf.cheapest_venue(true), "B") == 0, "rcf_cheapest_price_B_differs");
        // Disabling C hands the best fee to B (0.001).
        rcf.set_venue_active("C", false);
        ASSERT(std::strcmp(rcf.cheapest_fee_venue(cf), "B") == 0 && close(cf, 0.001),
               "rcf_best_fee_after_disable");
        // #504: disabling the only rebate venue leaves an all-taker landscape.
        ASSERT(rcf.rebate_venue_count() == 0, "rvf_no_rebate_after_disable");
        ASSERT(close(rcf.avg_venue_fee(), 0.002), "rvf_avg_fee_after_disable");   // (0.003+0.001)/2
        // #520: A is still the dearest with C gone; disabling A hands it to B.
        ASSERT(std::strcmp(rcf.dearest_fee_venue(df), "A") == 0, "rdf_dearest_still_A");
        rcf.set_venue_active("A", false);
        ASSERT(std::strcmp(rcf.dearest_fee_venue(df), "B") == 0 && close(df, 0.001),
               "rdf_dearest_B_after_disable");
        // Empty router -> nullptr, zero rebates, zero avg.
        SmartOrderRouter rce(RoutingStrategy::BEST_PRICE);
        ASSERT(rce.cheapest_fee_venue(cf) == nullptr, "rcf_empty_null");
        ASSERT(rce.dearest_fee_venue(df) == nullptr, "rdf_empty_null");   // #520
        ASSERT(rce.rebate_venue_count() == 0 && rce.avg_venue_fee() == 0.0, "rvf_empty_zero");
    }

    // --- #488 touch_concentration — liquidity fraction at the NBBO ---
    {
        auto close = [](double a, double b) { const double d = a - b; return (d<0?-d:d) < 1e-9; };
        SmartOrderRouter rtc(RoutingStrategy::BEST_PRICE);
        rtc.add_venue(Venue("A", 100, 0.0));
        rtc.add_venue(Venue("B", 100, 0.0));
        rtc.add_venue(Venue("C", 100, 0.0));
        rtc.update_quote("A", 9.98, 10.02, 100, 100);   // ask at NBO
        rtc.update_quote("B", 9.98, 10.02, 100, 100);   // ask at NBO
        rtc.update_quote("C", 9.98, 10.05, 100, 50);    // ask BEHIND the touch
        // BUY: NBO depth 200 (A+B) of 250 available -> 0.8 at the touch.
        ASSERT(close(rtc.touch_concentration(true), 0.8), "router_touch_conc_scattered");
        // Consolidating C onto the NBO -> everything at the touch -> 1.0.
        rtc.update_quote("C", 9.98, 10.02, 100, 50);
        ASSERT(close(rtc.touch_concentration(true), 1.0), "router_touch_conc_consolidated");
        // Empty router -> 0 (no liquidity, no division trap).
        SmartOrderRouter rte(RoutingStrategy::BEST_PRICE);
        ASSERT(rte.touch_concentration(true) == 0.0, "router_touch_conc_empty_zero");
    }

    // --- #464 effective_price_dispersion — real cross-venue price spread ---
    {
        auto close = [](double a, double b) { const double d = a - b; return (d<0?-d:d) < 1e-6; };
        SmartOrderRouter rd(RoutingStrategy::BEST_PRICE);
        rd.add_venue(Venue("A", 100, 0.0));
        rd.add_venue(Venue("B", 100, 0.0));
        rd.add_venue(Venue("C", 100, 0.0));
        rd.update_quote("A", 9.98, 10.02, 100, 100);   // ask 10.02
        rd.update_quote("B", 9.99, 10.05, 100, 100);   // ask 10.05
        rd.update_quote("C", 10.00, 10.10, 100, 100);  // ask 10.10
        // BUY all-in asks 10.02/10.05/10.10 -> dispersion 0.08.
        ASSERT(close(rd.effective_price_dispersion(true), 0.08), "epd_buy_wide");
        // SELL bids 9.98/9.99/10.00 -> dispersion 0.02.
        ASSERT(close(rd.effective_price_dispersion(false), 0.02), "epd_sell_narrow");
        // #472 most_expensive_venue — the culprit behind the dispersion.
        // BUY worst all-in ask is C (10.10); SELL worst (min bid) is A (9.98).
        ASSERT(std::strcmp(rd.most_expensive_venue(true), "C") == 0, "mev_buy_C");
        ASSERT(std::strcmp(rd.most_expensive_venue(false), "A") == 0, "mev_sell_A");
        // Mirror of cheapest_venue (#200): best buy is A, best sell is C.
        ASSERT(std::strcmp(rd.cheapest_venue(true), "A") == 0, "mev_cheapest_buy_A");
        // A punishing taker fee on the best-priced venue widens the buy
        // dispersion (all-in, not raw): A's ask 10.02 + 0.20 = 10.22 all-in.
        rd.set_venue_fee("A", 0.20);
        ASSERT(close(rd.effective_price_dispersion(true), 10.22 - 10.05), "epd_fee_widens");
        // #472: the fee makes A the worst all-in venue to buy on.
        ASSERT(std::strcmp(rd.most_expensive_venue(true), "A") == 0, "mev_fee_flips_worst_to_A");
        // Empty router -> nullptr.
        SmartOrderRouter rme(RoutingStrategy::BEST_PRICE);
        ASSERT(rme.most_expensive_venue(true) == nullptr, "mev_empty_null");
    }

    // --- #162 reject_rate + avg_routing_latency ---
    {
        auto close = [](double a, double b) { const double d = a - b; return (d<0?-d:d) < 1e-9; };
        SmartOrderRouter r(RoutingStrategy::BEST_PRICE);
        r.add_venue(Venue("A", 100, 0.0));
        r.update_quote("A", 10.0, 11.0, 100, 100);
        r.route_order("BUY", 10); r.route_order("BUY", 10);   // 2 udane
        ASSERT(close(r.reject_rate(), 0.0), "rejrate_all_ok");
        ASSERT(r.avg_routing_latency_ns() >= 0.0, "rejrate_avg_lat_nonneg");
        SmartOrderRouter e;                                    // no venue -> reject
        e.route_order("BUY", 10);
        ASSERT(close(e.reject_rate(), 1.0), "rejrate_all_rejected");
    }

    // --- #170 remove_venue (decommission) ---
    {
        SmartOrderRouter r(RoutingStrategy::BEST_PRICE);
        r.add_venue(Venue("A", 100, 0.0));
        r.add_venue(Venue("B", 100, 0.0));
        ASSERT(r.venue_count() == 2, "remove_two_before");
        ASSERT(r.remove_venue("A"), "remove_A_ok");
        ASSERT(r.venue_count() == 1 && !r.venue_active("A"), "remove_A_gone");
        ASSERT(!r.remove_venue("GHOST"), "remove_unknown_false");
        r.update_quote("B", 10.0, 11.0, 100, 100);                 // B remains, active
        ASSERT(std::strcmp(r.route_order("BUY", 10).venue, "B") == 0, "remove_B_still_routes");
    }

    // --- #176 set_venue_fee (runtime fee change -> routing all-in) ---
    {
        SmartOrderRouter r(RoutingStrategy::BEST_PRICE);
        r.add_venue(Venue("A", 100, 0.02));            // pricier taker
        r.add_venue(Venue("B", 100, 0.01));            // cheaper
        r.update_quote("A", 10.0, 11.0, 100, 100);
        r.update_quote("B", 10.0, 11.0, 100, 100);     // ten sam quote
        ASSERT(std::strcmp(r.route_order("BUY", 10).venue, "B") == 0, "fee_default_B_cheaper");
        ASSERT(r.set_venue_fee("A", 0.0), "fee_set_A_zero");   // A now all-in 11.00
        ASSERT(std::strcmp(r.route_order("BUY", 10).venue, "A") == 0, "fee_reroute_A");
        ASSERT(!r.set_venue_fee("GHOST", 0.0), "fee_unknown_false");
    }

    // --- #184 is_marketable (pre-route guard for limit orders) ---
    {
        SmartOrderRouter r(RoutingStrategy::BEST_PRICE);
        r.add_venue(Venue("A", 100, 0.01));
        r.update_quote("A", 10.0, 11.0, 100, 100);     // bid 10, ask 11, fee 0.01
        ASSERT(r.is_marketable(true, 11.50), "mkt_buy_inside");    // all-in 11.01 <= 11.50
        ASSERT(!r.is_marketable(true, 11.00), "mkt_buy_outside");  // 11.01 > 11.00
        ASSERT(r.is_marketable(false, 9.50), "mkt_sell_inside");   // all-in 9.99 >= 9.50
        ASSERT(!r.is_marketable(false, 10.50), "mkt_sell_outside");// 9.99 < 10.50
        SmartOrderRouter empt(RoutingStrategy::BEST_PRICE);
        ASSERT(!empt.is_marketable(true, 100.0), "mkt_no_liquidity_false");
    }

    // --- #192 reset_session_stats (full TCA reset, venues remain) ---
    {
        SmartOrderRouter rss(RoutingStrategy::BEST_PRICE);
        rss.add_venue(Venue("A", 100, 0.01));
        rss.update_quote("A", 10.0, 11.0, 100, 100);
        rss.route_order("BUY", 50);
        ASSERT(rss.get_total_routes() == 1 && rss.total_fees_paid() > 0.0
               && rss.venue_routed_shares("A") > 0, "rss_stats_before");
        rss.reset_session_stats();
        ASSERT(rss.get_total_routes() == 0 && rss.total_fees_paid() == 0.0
               && rss.venue_routed_shares("A") == 0, "rss_stats_zeroed");
        ASSERT(rss.venue_count() == 1, "rss_venue_retained");
    }

    // --- #200 cheapest_venue (inspekcja routingu) ---
    {
        SmartOrderRouter r(RoutingStrategy::BEST_PRICE);
        r.add_venue(Venue("A", 100, 0.02));            // wyzszy fee
        r.add_venue(Venue("B", 100, 0.01));            // nizszy fee
        r.update_quote("A", 10.0, 11.0, 100, 100);
        r.update_quote("B", 10.0, 11.0, 100, 100);     // ten sam quote
        ASSERT(std::strcmp(r.cheapest_venue(true), "B") == 0, "cheapest_buy_B");   // all-in 11.01 < 11.02
        ASSERT(std::strcmp(r.cheapest_venue(false), "B") == 0, "cheapest_sell_B"); // all-in 9.99 > 9.98
        SmartOrderRouter empt(RoutingStrategy::BEST_PRICE);
        ASSERT(empt.cheapest_venue(true) == nullptr, "cheapest_empty_null");
    }

    // --- #255 deepest_venue (largest displayed size per side) ---
    {
        SmartOrderRouter r(RoutingStrategy::BEST_PRICE);
        r.add_venue(Venue("A", 100, 0.0));
        r.add_venue(Venue("B", 100, 0.0));
        r.update_quote("A", 10.0, 11.0, 100, 100);     // bid_size 100 / ask_size 100
        r.update_quote("B", 10.0, 11.0, 50, 300);      // bid_size 50  / ask_size 300
        ASSERT(std::strcmp(r.deepest_venue(true), "B") == 0, "deepest_buy_B");   // ask 300 > 100
        ASSERT(std::strcmp(r.deepest_venue(false), "A") == 0, "deepest_sell_A"); // bid 100 > 50
        SmartOrderRouter empt(RoutingStrategy::BEST_PRICE);
        ASSERT(empt.deepest_venue(true) == nullptr, "deepest_empty_null");
    }

    // --- #278 fastest_venue (lowest selection latency) ---
    {
        SmartOrderRouter r(RoutingStrategy::BEST_PRICE);
        r.add_venue(Venue("A", 500, 0.0));   // static latency 500
        r.add_venue(Venue("B", 200, 0.0));   // 200 (fastest)
        r.add_venue(Venue("C", 800, 0.0));   // 800
        ASSERT(std::strcmp(r.fastest_venue(), "B") == 0, "fastest_B");
        // #512 slowest_venue: C at 800 is the laggard venue_latency_spread_ns names.
        ASSERT(std::strcmp(r.slowest_venue(), "C") == 0, "slowest_C");
        r.set_venue_active("B", false);
        ASSERT(std::strcmp(r.fastest_venue(), "A") == 0, "fastest_A_after_disable");
        ASSERT(std::strcmp(r.slowest_venue(), "C") == 0, "slowest_C_after_disable");
        SmartOrderRouter empt(RoutingStrategy::BEST_PRICE);
        ASSERT(empt.fastest_venue() == nullptr, "fastest_empty_null");
        ASSERT(empt.slowest_venue() == nullptr, "slowest_empty_null");
        // #286 avg_venue_latency_ns: A 500, C 800 active (B disabled above) -> 650
        ASSERT(std::fabs(r.avg_venue_latency_ns() - 650.0) < 1e-9, "avglat_after_disable");
        // #351 venue_latency_spread_ns: A 500, C 800 active -> 800 - 500 = 300
        ASSERT(r.venue_latency_spread_ns() == 300, "latspread_after_disable");
        ASSERT(empt.venue_latency_spread_ns() == 0, "latspread_empty_zero");
    }

    // --- #286 avg_venue_latency_ns ---
    {
        SmartOrderRouter r(RoutingStrategy::BEST_PRICE);
        r.add_venue(Venue("A", 100, 0.0));   // latency 100
        r.add_venue(Venue("B", 300, 0.0));   // latency 300
        ASSERT(std::fabs(r.avg_venue_latency_ns() - 200.0) < 1e-9, "avglat_200");
        // #351 venue_latency_spread_ns: A 100, B 300 both active -> 300 - 100 = 200
        ASSERT(r.venue_latency_spread_ns() == 200, "latspread_200");
        r.set_venue_active("B", false);
        ASSERT(std::fabs(r.avg_venue_latency_ns() - 100.0) < 1e-9, "avglat_one_active");
        // only A active -> lo == hi -> spread 0
        ASSERT(r.venue_latency_spread_ns() == 0, "latspread_one_active_zero");
        // #512: with one active venue the slowest IS the fastest.
        ASSERT(std::strcmp(r.slowest_venue(), "A") == 0
               && std::strcmp(r.fastest_venue(), "A") == 0, "slowest_eq_fastest_one_active");
        SmartOrderRouter empt(RoutingStrategy::BEST_PRICE);
        ASSERT(empt.avg_venue_latency_ns() == 0.0, "avglat_empty_zero");
    }

    // --- #294 nbbo_bid_venue / nbbo_ask_venue ---
    {
        SmartOrderRouter r(RoutingStrategy::BEST_PRICE);
        r.add_venue(Venue("NYSE", 100, 0.0));
        r.add_venue(Venue("NSDQ", 100, 0.0));
        r.add_venue(Venue("BATS", 100, 0.0));
        r.update_quote("NYSE", 99.98, 100.04, 100, 100);
        r.update_quote("NSDQ", 100.00, 100.02, 100, 100);   // best bid AND best ask
        r.update_quote("BATS", 99.99, 100.03, 100, 100);
        ASSERT(std::strcmp(r.nbbo_bid_venue(), "NSDQ") == 0, "nbbo_bid_venue_nsdq");
        ASSERT(std::strcmp(r.nbbo_ask_venue(), "NSDQ") == 0, "nbbo_ask_venue_nsdq");
        r.update_quote("BATS", 100.01, 100.02, 100, 100);   // BATS now best bid
        ASSERT(std::strcmp(r.nbbo_bid_venue(), "BATS") == 0, "nbbo_bid_venue_bats");
        SmartOrderRouter empt(RoutingStrategy::BEST_PRICE);
        ASSERT(empt.nbbo_bid_venue() == nullptr && empt.nbbo_ask_venue() == nullptr,
               "nbbo_venue_empty_null");
    }

    // --- #302 internally_crossed_count ---
    {
        SmartOrderRouter r(RoutingStrategy::BEST_PRICE);
        r.add_venue(Venue("A", 100, 0.0));
        r.add_venue(Venue("B", 100, 0.0));
        r.update_quote("A", 100.00, 100.02, 100, 100);   // normal
        r.update_quote("B", 100.05, 100.03, 100, 100);   // self-crossed: bid > ask
        ASSERT(r.internally_crossed_count() == 1, "intcross_one");        // B only
        r.update_quote("B", 100.01, 100.03, 100, 100);   // repaired
        ASSERT(r.internally_crossed_count() == 0, "intcross_repaired");
    }

    // --- #310 available_liquidity_notional ---
    {
        SmartOrderRouter r(RoutingStrategy::BEST_PRICE);
        r.add_venue(Venue("A", 100, 0.0));
        r.add_venue(Venue("B", 100, 0.0));
        r.update_quote("A", 99.98, 100.02, 100, 200);    // bid 99.98x100, ask 100.02x200
        r.update_quote("B", 100.00, 100.04, 150, 100);   // bid 100.00x150, ask 100.04x100
        // buy hits asks: 100.02*200 + 100.04*100 = 20004 + 10004 = 30008
        ASSERT(std::fabs(r.available_liquidity_notional(true) - 30008.0) < 1e-6, "alnot_buy");
        // sell hits bids: 99.98*100 + 100.00*150 = 9998 + 15000 = 24998
        ASSERT(std::fabs(r.available_liquidity_notional(false) - 24998.0) < 1e-6, "alnot_sell");
        r.set_venue_active("B", false);
        ASSERT(std::fabs(r.available_liquidity_notional(true) - 20004.0) < 1e-6, "alnot_one_active");
        SmartOrderRouter empt(RoutingStrategy::BEST_PRICE);
        ASSERT(empt.available_liquidity_notional(true) == 0.0, "alnot_empty_zero");
    }

    // --- #262 venue_liquidity_share (current displayed concentration) ---
    {
        SmartOrderRouter r(RoutingStrategy::BEST_PRICE);
        r.add_venue(Venue("A", 100, 0.0));
        r.add_venue(Venue("B", 100, 0.0));
        r.update_quote("A", 10.0, 11.0, 100, 100);     // ask 100
        r.update_quote("B", 10.0, 11.0, 100, 300);     // ask 300 -> total 400
        ASSERT(std::fabs(r.venue_liquidity_share("A", true) - 25.0) < 1e-9, "vls_A_25");
        ASSERT(std::fabs(r.venue_liquidity_share("B", true) - 75.0) < 1e-9, "vls_B_75");
        ASSERT(r.venue_liquidity_share("GHOST", true) == 0.0, "vls_unknown_zero");
    }

    // --- #208 nbbo_spread / nbbo_spread_bps (consolidated market) ---
    {
        SmartOrderRouter r(RoutingStrategy::BEST_PRICE);
        r.add_venue(Venue("A", 100, 0.0));
        r.add_venue(Venue("B", 100, 0.0));
        r.update_quote("A", 99.98, 100.02, 100, 100);   // bid 99.98 / ask 100.02
        r.update_quote("B", 100.00, 100.04, 100, 100);  // bid 100.00 / ask 100.04
        // NBB = 100.00 (B), NBO = 100.02 (A) -> spread 0.02, mid 100.01
        ASSERT(std::fabs(r.nbbo_spread() - 0.02) < 1e-9, "nbbo_spread_tighter");
        ASSERT(std::fabs(r.nbbo_spread_bps() - 0.02/100.01*10000.0) < 1e-6, "nbbo_spread_bps");
        SmartOrderRouter empt(RoutingStrategy::BEST_PRICE);
        ASSERT(empt.nbbo_spread() == 0.0 && empt.nbbo_spread_bps() == 0.0, "nbbo_spread_empty");
        // #270 normal NBBO: NBB 100.00 (B) / NBO 100.02 (A) -> not locked/crossed
        ASSERT(!r.nbbo_locked() && !r.nbbo_crossed(), "nbbo_normal");
    }

    // --- #270 nbbo_locked / nbbo_crossed ---
    {
        SmartOrderRouter lk(RoutingStrategy::BEST_PRICE);
        lk.add_venue(Venue("A", 100, 0.0)); lk.add_venue(Venue("B", 100, 0.0));
        lk.update_quote("A", 99.98, 100.00, 100, 100);   // ask 100.00
        lk.update_quote("B", 100.00, 100.04, 100, 100);  // bid 100.00 == ask 100.00
        ASSERT(lk.nbbo_locked() && !lk.nbbo_crossed(), "nbbo_locked");
        SmartOrderRouter cr(RoutingStrategy::BEST_PRICE);
        cr.add_venue(Venue("A", 100, 0.0)); cr.add_venue(Venue("B", 100, 0.0));
        cr.update_quote("A", 99.98, 100.00, 100, 100);   // ask 100.00
        cr.update_quote("B", 100.05, 100.10, 100, 100);  // bid 100.05 > ask 100.00
        ASSERT(cr.nbbo_crossed() && !cr.nbbo_locked(), "nbbo_crossed");
    }

    // --- #240 effective_spread_bps (cost of crossing with fees) ---
    {
        SmartOrderRouter r(RoutingStrategy::BEST_PRICE);
        r.add_venue(Venue("A", 100, 0.01));            // taker fee 0.01
        r.update_quote("A", 99.98, 100.02, 100, 100);  // bid 99.98 / ask 100.02 (quote spread 0.04)
        // eff_ask = 100.03, eff_bid = 99.97, eff spread 0.06, mid 100.00 -> 6.0 bps
        ASSERT(std::fabs(r.effective_spread_bps() - 6.0) < 1e-6, "effspread_with_fees");
        // quote-only NBBO spread = 0.04/100.00*10000 = 4.0; effective > o round-trip fees
        ASSERT(r.effective_spread_bps() > r.nbbo_spread_bps(), "effspread_gt_quote");
        SmartOrderRouter empt(RoutingStrategy::BEST_PRICE);
        ASSERT(empt.effective_spread_bps() == 0.0, "effspread_empty_zero");
    }

    // --- #248 venue_effective_price (per-venue, directed order) ---
    {
        SmartOrderRouter r(RoutingStrategy::BEST_PRICE);
        r.add_venue(Venue("A", 100, 0.01));
        r.add_venue(Venue("B", 100, 0.02));
        r.update_quote("A", 10.0, 11.0, 100, 100);
        r.update_quote("B", 10.0, 11.0, 100, 100);
        ASSERT(std::fabs(r.venue_effective_price("A", true) - 11.01) < 1e-9, "vep_A_buy");
        ASSERT(std::fabs(r.venue_effective_price("B", true) - 11.02) < 1e-9, "vep_B_buy");
        ASSERT(std::fabs(r.venue_effective_price("A", false) - 9.99) < 1e-9, "vep_A_sell");
        ASSERT(r.venue_effective_price("GHOST", true) == 0.0, "vep_unknown_zero");
    }

    // --- #216 venue_share_pct (execution concentration) ---
    {
        SmartOrderRouter r(RoutingStrategy::BEST_PRICE);
        r.add_venue(Venue("A", 100, 0.0));             // cheaper (fee 0) -> routing goes here
        r.add_venue(Venue("B", 100, 0.01));
        r.update_quote("A", 10.0, 11.0, 1000, 1000);
        r.update_quote("B", 10.0, 11.0, 1000, 1000);
        r.route_order("BUY", 30);
        r.route_order("BUY", 70);                       // total 100, all on A
        ASSERT(std::fabs(r.venue_share_pct("A") - 100.0) < 1e-9, "vsp_A_full");
        ASSERT(std::fabs(r.venue_share_pct("B") - 0.0) < 1e-9, "vsp_B_zero");
        ASSERT(r.venue_share_pct("GHOST") == 0.0, "vsp_unknown_zero");
    }

    // --- #224 fill_shortfall / fillable_ratio (pre-route sizing) ---
    {
        SmartOrderRouter r(RoutingStrategy::BEST_PRICE);
        r.add_venue(Venue("A", 100, 0.0));
        r.add_venue(Venue("B", 100, 0.0));
        r.update_quote("A", 10.0, 11.0, 100, 100);     // ask size 100
        r.update_quote("B", 10.0, 11.0, 100, 200);     // ask size 200 -> total 300
        ASSERT(r.fill_shortfall(true, 250) == 0, "shortfall_covered");
        ASSERT(r.fill_shortfall(true, 400) == 100, "shortfall_100");      // 400 - 300
        ASSERT(r.fill_shortfall(true, 300) == 0, "shortfall_exact");
        ASSERT(std::fabs(r.fillable_ratio(true, 400) - 0.75) < 1e-9, "ratio_three_quarters");
        ASSERT(std::fabs(r.fillable_ratio(true, 250) - 1.0) < 1e-9, "ratio_full");
    }

    // --- #232 avg_fee_per_share (TCA: net taker/maker) ---
    {
        SmartOrderRouter r(RoutingStrategy::BEST_PRICE);
        r.add_venue(Venue("A", 100, 0.002));           // taker fee 0.002/share
        r.update_quote("A", 10.0, 11.0, 1000, 1000);
        r.route_order("BUY", 100);                      // fee 0.2
        r.route_order("BUY", 100);                      // fee 0.2, total 0.4 / 200 shares
        ASSERT(std::fabs(r.avg_fee_per_share() - 0.002) < 1e-9, "avgfee_taker");
        SmartOrderRouter empt(RoutingStrategy::BEST_PRICE);
        ASSERT(empt.avg_fee_per_share() == 0.0, "avgfee_empty_zero");
    }
}


// Backtester #80 — the core of result metrics (Sharpe/DD/hit-rate/fill-rate).
void test_backtester() {
    SECTION("Backtester (#80)");
    backtest::Backtester bt;
    bt.on_order(true);  bt.on_trade(+100.0);
    bt.on_order(true);  bt.on_trade(-40.0);
    bt.on_order(true);  bt.on_trade(+60.0);
    bt.on_order(false);                          // submitted, not filled
    const auto r = bt.compute();
    ASSERT(r.trades == 3, "bt_trades_3");
    ASSERT(r.wins == 2 && r.losses == 1, "bt_win_loss");
    ASSERT(std::fabs(r.total_pnl - 120.0) < 1e-9, "bt_total_pnl_120");
    ASSERT(std::fabs(r.hit_rate - 2.0/3.0) < 1e-9, "bt_hit_rate_2of3");
    ASSERT(std::fabs(r.profit_factor - 4.0) < 1e-9, "bt_profit_factor_4");  // 160/40
    ASSERT(std::fabs(r.fill_rate - 0.75) < 1e-9, "bt_fill_rate_3of4");

    backtest::Backtester dd;                     // max drawdown: 100→30 = 70
    dd.on_trade(+100.0); dd.on_trade(-70.0); dd.on_trade(+50.0);
    ASSERT(std::fabs(dd.compute().max_drawdown - 70.0) < 1e-9, "bt_max_dd_70");

    // #92 streaki, largest, expectancy: +10,+20,-5,-3,+8
    backtest::Backtester e;
    e.on_trade(10); e.on_trade(20); e.on_trade(-5); e.on_trade(-3); e.on_trade(8);
    const auto er = e.compute();
    ASSERT(er.max_consecutive_wins == 2, "bt_max_win_streak");      // 10,20
    ASSERT(er.max_consecutive_losses == 2, "bt_max_loss_streak");   // -5,-3
    ASSERT(std::fabs(er.largest_win - 20.0) < 1e-9, "bt_largest_win");
    ASSERT(std::fabs(er.largest_loss + 5.0) < 1e-9, "bt_largest_loss");
    ASSERT(std::fabs(er.expectancy - 6.0) < 1e-9, "bt_expectancy");  // 30/5

    // #102 per-tag attribution (e.g. per strategy).
    backtest::Backtester t;
    t.on_trade(100.0, "momentum");
    t.on_trade(-30.0, "meanrev");
    t.on_trade(50.0,  "momentum");
    ASSERT(std::fabs(t.pnl_for_tag("momentum") - 150.0) < 1e-9, "bt_tag_momentum");
    ASSERT(std::fabs(t.pnl_for_tag("meanrev") + 30.0) < 1e-9, "bt_tag_meanrev");
    ASSERT(std::fabs(t.pnl_for_tag("ghost")) < 1e-9, "bt_tag_unknown_zero");
    ASSERT(t.tag_count() == 2, "bt_tag_count");
    ASSERT(std::fabs(t.compute().total_pnl - 120.0) < 1e-9, "bt_tag_total_aggregates");

    // #108 krzywa equity: skumulowany P&L po kazdej transakcji.
    backtest::Backtester ec;
    ec.on_trade(100.0); ec.on_trade(-40.0); ec.on_trade(60.0);
    const auto& curve = ec.equity_curve();
    ASSERT(curve.size() == 3, "bt_equity_curve_len");
    ASSERT(std::fabs(curve[0] - 100.0) < 1e-9, "bt_equity_p0");
    ASSERT(std::fabs(curve[1] - 60.0) < 1e-9, "bt_equity_p1");   // 100-40
    ASSERT(std::fabs(curve[2] - 120.0) < 1e-9, "bt_equity_p2"); // 60+60
}


// Risk #79 — price-band (fat-finger) + atomic kill switch.
void test_risk_price_band() {
    SECTION("Risk Price Band + Atomic Kill (#79)");

    RiskLimits lim;
    lim.max_price_band_pct      = 20.0;
    lim.max_order_value         = 100000000;   // hojne — izolujemy band
    lim.max_position_per_symbol = 1000000;
    lim.max_portfolio_exposure  = 100000000;
    lim.max_orders_per_second   = 1000000;
    RiskManager r(lim);

    // Without a reference price the band is skipped — the first order passes.
    ASSERT(r.check_order("AAPL", Side::BUY, 150.00, 10).action == RiskAction::ALLOW,
           "band_no_ref_allows");

    r.update_reference_price("AAPL", 150.00);
    ASSERT(r.check_order("AAPL", Side::BUY, 165.00, 10).action == RiskAction::ALLOW,
           "band_within_10pct_allows");                 // +10% < 20%
    ASSERT(r.check_order("AAPL", Side::BUY, 1500.00, 10).action == RiskAction::REJECT,
           "band_fat_finger_rejects");                  // +900% — a gross mistake
    ASSERT(r.check_order("AAPL", Side::SELL, 100.00, 10).action == RiskAction::REJECT,
           "band_far_below_rejects");                   // -33% < -20%

    // Band disabled (≤0) → even a 10× price passes.
    RiskLimits off = lim; off.max_price_band_pct = 0.0;
    RiskManager r2(off);
    r2.update_reference_price("AAPL", 150.00);
    ASSERT(r2.check_order("AAPL", Side::BUY, 1500.00, 10).action == RiskAction::ALLOW,
           "band_disabled_allows");

    // Atomic kill switch — toggling works as before (atomic<bool> type).
    RiskManager r3(lim);
    ASSERT(!r3.is_kill_switch_active(), "kill_initially_off");
    r3.activate_kill_switch();
    ASSERT(r3.is_kill_switch_active(), "kill_activates");
    ASSERT(r3.check_order("AAPL", Side::BUY, 150.0, 1).action == RiskAction::REJECT,
           "kill_rejects_all");
    r3.deactivate_kill_switch();
    ASSERT(!r3.is_kill_switch_active(), "kill_deactivates");

    // #84 restricted symbol — halt/Reg SHO/freeze.
    RiskManager r4(lim);
    ASSERT(r4.check_order("TSLA", Side::BUY, 250.0, 1).action == RiskAction::ALLOW,
           "restrict_allows_before");
    r4.restrict_symbol("TSLA");
    ASSERT(r4.is_restricted("TSLA"), "restrict_flag_set");
    ASSERT(r4.check_order("TSLA", Side::BUY, 250.0, 1).action == RiskAction::REJECT,
           "restrict_rejects");
    ASSERT(r4.check_order("AAPL", Side::BUY, 150.0, 1).action == RiskAction::ALLOW,
           "restrict_other_symbol_ok");
    r4.allow_symbol("TSLA");
    ASSERT(r4.check_order("TSLA", Side::BUY, 250.0, 1).action == RiskAction::ALLOW,
           "restrict_lifted_allows");

    // #106 asymmetric limits: long 1000, short only 300.
    RiskLimits al;
    al.max_position_per_symbol = 1000;
    al.max_short_per_symbol    = 300;
    al.max_portfolio_exposure  = 100000000;
    al.max_order_value         = 100000000;
    al.max_orders_per_second   = 1000000;
    al.max_price_band_pct      = 0.0;
    RiskManager r6(al);
    ASSERT(r6.check_order("AAPL", Side::BUY,  10.0, 1000).action == RiskAction::ALLOW,
           "asym_long_at_cap_ok");                          // long 1000 == cap
    ASSERT(r6.check_order("AAPL", Side::SELL, 10.0, 300).action == RiskAction::ALLOW,
           "asym_short_at_cap_ok");                          // short 300 == short cap
    ASSERT(r6.check_order("AAPL", Side::SELL, 10.0, 301).action == RiskAction::REJECT,
           "asym_short_over_cap_rejects");                   // 301 > 300 short cap
    ASSERT(r6.check_order("AAPL", Side::BUY,  10.0, 500).action == RiskAction::ALLOW,
           "asym_long_500_ok");                              // long pod 1000 OK mimo short-capu

    // #114 loss-streak breaker: 3 losing in a row -> kill switch.
    RiskLimits cl;
    cl.max_consecutive_losses = 3;
    RiskManager r7(cl);
    r7.update_pnl(-10.0); r7.update_pnl(-10.0);
    ASSERT(!r7.is_kill_switch_active() && r7.get_consecutive_losses() == 2, "consec_2_ok");
    r7.update_pnl(-10.0);                                    // 3. -> trip
    ASSERT(r7.is_kill_switch_active(), "consec_3_trips_kill");
    // a profit in the middle resets the streak
    RiskManager r8(cl);
    r8.update_pnl(-10.0); r8.update_pnl(-10.0); r8.update_pnl(+5.0);
    ASSERT(r8.get_consecutive_losses() == 0, "consec_win_resets");
    r8.update_pnl(-10.0); r8.update_pnl(-10.0);
    ASSERT(!r8.is_kill_switch_active(), "consec_no_trip_after_reset");

    // #348 get_consecutive_wins — symmetric win-streak counter (no breaker on this side).
    RiskManager r9(cl);
    ASSERT(r9.get_consecutive_wins() == 0, "consec_wins_start_zero");
    r9.update_pnl(+5.0); r9.update_pnl(+5.0);
    ASSERT(r9.get_consecutive_wins() == 2, "consec_wins_2");
    r9.update_pnl(-10.0);                      // a loss resets the win streak
    ASSERT(r9.get_consecutive_wins() == 0, "consec_wins_loss_resets");
    r9.update_pnl(+5.0);
    ASSERT(r9.get_consecutive_wins() == 1, "consec_wins_after_reset");
    r9.reset_daily();
    ASSERT(r9.get_consecutive_wins() == 0, "consec_wins_reset_daily");

    // #364 max_consecutive_losses_seen — high-water mark of the losing streak.
    RiskLimits mcl; mcl.max_consecutive_losses = 0;   // breaker off, so streaks run free
    RiskManager r10(mcl);
    ASSERT(r10.max_consecutive_losses_seen() == 0, "mcls_start_zero");
    r10.update_pnl(-1.0); r10.update_pnl(-1.0);       // streak 2
    ASSERT(r10.max_consecutive_losses_seen() == 2, "mcls_two");
    r10.update_pnl(+5.0);                             // win resets the live streak...
    ASSERT(r10.get_consecutive_losses() == 0, "mcls_live_reset_by_win");
    ASSERT(r10.max_consecutive_losses_seen() == 2, "mcls_high_water_survives_win"); // ...not the peak
    r10.update_pnl(-1.0); r10.update_pnl(-1.0); r10.update_pnl(-1.0);   // streak 3 > 2
    ASSERT(r10.max_consecutive_losses_seen() == 3, "mcls_new_worse_streak");
    r10.reset_daily();
    ASSERT(r10.max_consecutive_losses_seen() == 0, "mcls_reset_daily");

    // #372 pnl_win_rate — hit rate on individual P&L updates (flat updates excluded).
    RiskManager rwr;
    ASSERT(rwr.pnl_win_rate() == 0.0, "pwr_empty_zero");
    rwr.update_pnl(+5.0); rwr.update_pnl(+3.0); rwr.update_pnl(-2.0);   // 2 wins, 1 loss
    rwr.update_pnl(0.0);                                                // flat -> excluded
    ASSERT(rwr.winning_pnl_updates() == 2 && rwr.losing_pnl_updates() == 1, "pwr_counts");
    ASSERT(std::fabs(rwr.pnl_win_rate() - 2.0/3.0) < 1e-9, "pwr_two_thirds");
    rwr.reset_daily();
    ASSERT(rwr.pnl_win_rate() == 0.0 && rwr.winning_pnl_updates() == 0, "pwr_reset_daily");

    // #381 avg_pnl_win / avg_pnl_loss / pnl_payoff_ratio — magnitude axis of #372.
    RiskManager rpw;
    ASSERT(rpw.avg_pnl_win() == 0.0 && rpw.avg_pnl_loss() == 0.0
           && rpw.pnl_payoff_ratio() == 0.0, "ppr_empty_zero");
    rpw.update_pnl(+10.0);
    ASSERT(std::fabs(rpw.avg_pnl_win() - 10.0) < 1e-9, "ppr_first_win");
    ASSERT(rpw.pnl_payoff_ratio() == 0.0, "ppr_no_loss_yet_zero");   // no ratio without a loss
    rpw.update_pnl(+30.0);                       // wins: 10, 30 -> avg 20
    rpw.update_pnl(-5.0);                        // losses: 5    -> avg 5 (positive magnitude)
    rpw.update_pnl(0.0);                         // flat -> excluded from both sides
    ASSERT(std::fabs(rpw.avg_pnl_win() - 20.0) < 1e-9, "ppr_avg_win_20");
    ASSERT(std::fabs(rpw.avg_pnl_loss() - 5.0) < 1e-9, "ppr_avg_loss_5_positive");
    ASSERT(std::fabs(rpw.pnl_payoff_ratio() - 4.0) < 1e-9, "ppr_payoff_4");
    // Expectancy cross-check: rate*avg_win - (1-rate)*avg_loss = 2/3*20 - 1/3*5 = 11.6667
    ASSERT(std::fabs(rpw.pnl_win_rate() * rpw.avg_pnl_win()
                     - (1.0 - rpw.pnl_win_rate()) * rpw.avg_pnl_loss()
                     - 35.0 / 3.0) < 1e-9, "ppr_expectancy_closed_form");
    // #461 pnl_expectancy — the fused edge, equal to that closed form.
    ASSERT(std::fabs(rpw.pnl_expectancy() - 35.0 / 3.0) < 1e-9, "pex_matches_closed_form");
    ASSERT(std::fabs(rpw.pnl_expectancy()
                     - (rpw.pnl_win_rate() * rpw.avg_pnl_win()
                        - (1.0 - rpw.pnl_win_rate()) * rpw.avg_pnl_loss())) < 1e-9,
           "pex_equals_rate_magnitude_form");
    // #469 pnl_profit_factor: gross win 40 / gross loss 5 = 8.0, and it
    // equals payoff_ratio * (winning/losing) = 4.0 * 2.
    ASSERT(std::fabs(rpw.pnl_profit_factor() - 8.0) < 1e-9, "ppf_eight");
    ASSERT(std::fabs(rpw.pnl_profit_factor()
                     - rpw.pnl_payoff_ratio() * 2.0) < 1e-9, "ppf_folds_in_frequency");
    rpw.reset_daily();
    ASSERT(rpw.avg_pnl_win() == 0.0 && rpw.avg_pnl_loss() == 0.0
           && rpw.pnl_payoff_ratio() == 0.0, "ppr_reset_daily");
    ASSERT(rpw.pnl_expectancy() == 0.0, "pex_reset_zero");
    ASSERT(rpw.pnl_profit_factor() == 0.0, "ppf_reset_zero");

    // #525 kelly_fraction — optimal risk fraction W - (1-W)/b from win rate + payoff.
    RiskManager rkf;
    ASSERT(rkf.kelly_fraction() == 0.0, "kelly_empty_zero");
    rkf.update_pnl(+5.0);                     // a win, but no loss yet -> b undefined
    ASSERT(rkf.kelly_fraction() == 0.0, "kelly_no_loss_zero");
    // W=0.5, payoff b=2 (avg win 2, avg loss 1) -> f = 0.5 - 0.5/2 = 0.25.
    RiskManager rkf2;
    rkf2.update_pnl(+2.0); rkf2.update_pnl(+2.0);
    rkf2.update_pnl(-1.0); rkf2.update_pnl(-1.0);
    ASSERT(std::fabs(rkf2.kelly_fraction() - 0.25) < 1e-9, "kelly_coinflip_2to1_quarter");
    // A losing edge reads NEGATIVE: W=0.5, b=0.5 -> f = 0.5 - 0.5/0.5 = -0.5.
    RiskManager rkf3;
    rkf3.update_pnl(+1.0); rkf3.update_pnl(+1.0);
    rkf3.update_pnl(-2.0); rkf3.update_pnl(-2.0);
    ASSERT(rkf3.kelly_fraction() < 0.0
           && std::fabs(rkf3.kelly_fraction() + 0.5) < 1e-9, "kelly_losing_edge_negative");
    rkf2.reset_daily();
    ASSERT(rkf2.kelly_fraction() == 0.0, "kelly_reset");

    // #461: a high WIN RATE can still be a NEGATIVE edge — three 1-dollar
    // wins and one 10-dollar loss is 75% wins but -1.75 per update.
    RiskManager rpx;
    rpx.update_pnl(+1.0); rpx.update_pnl(+1.0); rpx.update_pnl(+1.0);
    rpx.update_pnl(-10.0);
    ASSERT(std::fabs(rpx.pnl_win_rate() - 0.75) < 1e-9, "pex_high_win_rate");
    ASSERT(std::fabs(rpx.pnl_expectancy() + 1.75) < 1e-9, "pex_negative_edge");
    // #469: 75% wins but gross 3 / gross 10 = 0.3 profit factor (< 1 = bleeding).
    ASSERT(std::fabs(rpx.pnl_profit_factor() - 0.3) < 1e-9, "ppf_below_one_despite_win_rate");
    // No losing update yet -> 0 (no denominator).
    RiskManager rpf;
    rpf.update_pnl(+5.0); rpf.update_pnl(+3.0);
    ASSERT(rpf.pnl_profit_factor() == 0.0, "ppf_no_loss_zero");

    // #477 pnl_std_dev / pnl_sharpe — volatility of the P&L event stream.
    RiskManager rsh;
    ASSERT(rsh.pnl_std_dev() == 0.0 && rsh.pnl_sharpe() == 0.0, "psh_empty_zero");
    // {+3,-1,+3,-1}: mean 1, sumsq 20, var 20/4 - 1 = 4, std 2, sharpe 0.5.
    rsh.update_pnl(+3.0); rsh.update_pnl(-1.0); rsh.update_pnl(+3.0); rsh.update_pnl(-1.0);
    ASSERT(std::fabs(rsh.pnl_std_dev() - 2.0) < 1e-9, "psh_std_2");
    ASSERT(std::fabs(rsh.pnl_sharpe() - 0.5) < 1e-9, "psh_sharpe_half");
    // #485 downside deviation / Sortino: losses {-1,-1}, loss_sumsq 2 over
    // N=4 -> downside_dev sqrt(0.5) ~ 0.7071, sortino 1/0.7071 ~ 1.4142.
    ASSERT(std::fabs(rsh.pnl_downside_dev() - std::sqrt(0.5)) < 1e-9, "psh_downside_dev");
    ASSERT(std::fabs(rsh.pnl_sortino() - 1.0 / std::sqrt(0.5)) < 1e-9, "psh_sortino");
    // Sortino rewards the upside the Sharpe punishes -> strictly higher here.
    ASSERT(rsh.pnl_sortino() > rsh.pnl_sharpe(), "psh_sortino_above_sharpe");
    // Doubling every P&L doubles the std but leaves the Sharpe unchanged —
    // the risk-adjustment property (same consistency, twice the size).
    RiskManager rsh2;
    rsh2.update_pnl(+6.0); rsh2.update_pnl(-2.0); rsh2.update_pnl(+6.0); rsh2.update_pnl(-2.0);
    ASSERT(std::fabs(rsh2.pnl_std_dev() - 4.0) < 1e-9, "psh_scale_doubles_std");
    ASSERT(std::fabs(rsh2.pnl_sharpe() - rsh.pnl_sharpe()) < 1e-9, "psh_sharpe_scale_invariant");
    // A constant P&L stream has zero volatility -> Sharpe 0 (undefined).
    RiskManager rsc;
    rsc.update_pnl(+5.0); rsc.update_pnl(+5.0); rsc.update_pnl(+5.0);
    ASSERT(rsc.pnl_std_dev() == 0.0 && rsc.pnl_sharpe() == 0.0, "psh_constant_zero_vol");
    // #485: a series with NO losses has zero downside -> Sortino 0 (not inf).
    RiskManager rso;
    rso.update_pnl(+2.0); rso.update_pnl(+3.0); rso.update_pnl(+1.0);
    ASSERT(rso.pnl_downside_dev() == 0.0 && rso.pnl_sortino() == 0.0, "psh_no_downside_zero");
    rsh.reset_daily();
    ASSERT(rsh.pnl_std_dev() == 0.0 && rsh.pnl_sharpe() == 0.0, "psh_reset");
    ASSERT(rsh.pnl_downside_dev() == 0.0 && rsh.pnl_sortino() == 0.0, "psh_downside_reset");

    // #501 largest_pnl_gain / largest_pnl_loss — the tail events.
    RiskManager rtx;
    ASSERT(rtx.largest_pnl_gain() == 0.0 && rtx.largest_pnl_loss() == 0.0, "ptx_empty_zero");
    rtx.update_pnl(+3.0); rtx.update_pnl(-1.0); rtx.update_pnl(+10.0); rtx.update_pnl(-5.0);
    ASSERT(std::fabs(rtx.largest_pnl_gain() - 10.0) < 1e-9, "ptx_biggest_gain_10");
    ASSERT(std::fabs(rtx.largest_pnl_loss() - 5.0) < 1e-9, "ptx_biggest_loss_5_positive");
    // Smaller subsequent moves never lower the high-water marks; a flat
    // update touches neither.
    rtx.update_pnl(+2.0); rtx.update_pnl(-1.0); rtx.update_pnl(0.0);
    ASSERT(std::fabs(rtx.largest_pnl_gain() - 10.0) < 1e-9
           && std::fabs(rtx.largest_pnl_loss() - 5.0) < 1e-9, "ptx_extremes_survive");
    rtx.reset_daily();
    ASSERT(rtx.largest_pnl_gain() == 0.0 && rtx.largest_pnl_loss() == 0.0, "ptx_reset");

    // #509 pnl_tail_ratio — the ratio of the EXTREMES (best gain / worst loss),
    // the tail companion to pnl_payoff_ratio (#381, averages) and
    // pnl_profit_factor (#469, sums).
    RiskManager rtl;
    ASSERT(rtl.pnl_tail_ratio() == 0.0, "ptr_empty_zero");
    rtl.update_pnl(+8.0);                                // a gain but no loss yet -> no ratio
    ASSERT(rtl.pnl_tail_ratio() == 0.0, "ptr_no_loss_zero");
    rtl.update_pnl(-4.0); rtl.update_pnl(+2.0); rtl.update_pnl(-1.0);
    // biggest gain 8, biggest loss 4 -> 2.0, a convex tail (> 1)
    ASSERT(std::fabs(rtl.pnl_tail_ratio() - 2.0) < 1e-9, "ptr_convex_2");
    // negative-skew signature: two small wins then one loss larger than any
    // gain -> ratio < 1, the blow-up shape a high win rate would hide.
    RiskManager rtl2;
    rtl2.update_pnl(+3.0); rtl2.update_pnl(+3.0); rtl2.update_pnl(-9.0);
    ASSERT(rtl2.pnl_tail_ratio() < 1.0, "ptr_negative_skew_below_one");
    rtl.reset_daily();
    ASSERT(rtl.pnl_tail_ratio() == 0.0, "ptr_reset_zero");

    // #397 underwater_updates / max_underwater_updates — drawdown DURATION,
    // the time axis to max_drawdown_dollars' (#340) depth.
    RiskManager ruw;
    ASSERT(ruw.underwater_updates() == 0 && ruw.max_underwater_updates() == 0,
           "uw_fresh_zero");
    ASSERT(ruw.total_underwater_updates() == 0 && ruw.underwater_fraction() == 0.0,
           "uw_total_fresh_zero");   // #517
    ruw.update_pnl(+10.0);                       // new peak -> at the surface
    ASSERT(ruw.underwater_updates() == 0, "uw_at_peak_zero");
    ruw.update_pnl(-2.0);
    ruw.update_pnl(-1.0);
    ruw.update_pnl(0.0);                         // FLAT extends the spell
    ASSERT(ruw.underwater_updates() == 3, "uw_flat_extends_spell");
    ruw.update_pnl(+20.0);                       // 7 -> 27: new high ends it
    ASSERT(ruw.underwater_updates() == 0 && ruw.max_underwater_updates() == 3,
           "uw_new_peak_ends_spell");
    // A WIN that does not reach the old peak still counts as underwater.
    ruw.update_pnl(-5.0);
    ruw.update_pnl(+4.0);                        // 26 < peak 27 -> still under
    ASSERT(ruw.underwater_updates() == 2, "uw_partial_recovery_still_under");
    ruw.update_pnl(+2.0);                        // 28 > 27 -> surfaced
    ASSERT(ruw.underwater_updates() == 0, "uw_surfaced");
    // A longer dive raises the session high-water mark.
    ruw.update_pnl(-1.0); ruw.update_pnl(-1.0);
    ruw.update_pnl(-1.0); ruw.update_pnl(-1.0);
    ASSERT(ruw.max_underwater_updates() == 4, "uw_max_grows");
    // #517: cumulative time underwater — 9 of the 12 updates were below the
    // running peak (only the 3 that set/held a new high were at the surface).
    ASSERT(ruw.total_underwater_updates() == 9, "uw_total_9");
    ASSERT(std::fabs(ruw.underwater_fraction() - 9.0 / 12.0) < 1e-9, "uw_fraction_075");
    ruw.reset_daily();
    ASSERT(ruw.underwater_updates() == 0 && ruw.max_underwater_updates() == 0,
           "uw_reset_daily");
    ASSERT(ruw.total_underwater_updates() == 0 && ruw.underwater_fraction() == 0.0,
           "uw_total_reset");   // #517
    // #517: a monotonically rising equity curve never dips below its peak.
    RiskManager rmono;
    rmono.update_pnl(+1.0); rmono.update_pnl(+2.0); rmono.update_pnl(+3.0);
    ASSERT(rmono.total_underwater_updates() == 0 && rmono.underwater_fraction() == 0.0,
           "uw_monotone_zero_fraction");

    // #405 max_consecutive_wins_seen — the win-side high-water mark (#364's twin).
    RiskManager rmw;
    ASSERT(rmw.max_consecutive_wins_seen() == 0, "mcw_fresh_zero");
    rmw.update_pnl(+1.0); rmw.update_pnl(+1.0); rmw.update_pnl(+1.0);   // streak 3
    ASSERT(rmw.get_consecutive_wins() == 3 && rmw.max_consecutive_wins_seen() == 3,
           "mcw_streak_tracked");
    rmw.update_pnl(-1.0);                        // a loss resets the LIVE streak...
    ASSERT(rmw.get_consecutive_wins() == 0 && rmw.max_consecutive_wins_seen() == 3,
           "mcw_survives_loss_reset");
    rmw.update_pnl(+1.0); rmw.update_pnl(+1.0);  // a shorter streak does not lower it
    ASSERT(rmw.max_consecutive_wins_seen() == 3, "mcw_shorter_streak_ignored");
    rmw.update_pnl(0.0);                         // flat breaks neither counter (#372 semantics)
    rmw.update_pnl(+1.0); rmw.update_pnl(+1.0);  // ...streak continues to 4
    ASSERT(rmw.max_consecutive_wins_seen() == 4, "mcw_longer_streak_raises");
    rmw.reset_daily();
    ASSERT(rmw.max_consecutive_wins_seen() == 0, "mcw_reset_daily");

    // #421 kill_switch_activations — fresh latches only, across triggers.
    RiskLimits kal;
    kal.max_position_per_symbol = 100000;
    kal.max_portfolio_exposure  = 100000000;
    kal.max_order_value         = 100000000;
    kal.max_orders_per_second   = 1000000;
    kal.max_price_band_pct      = 0.0;
    kal.max_consecutive_losses  = 2;
    RiskManager rka(kal);
    ASSERT(rka.kill_switch_activations() == 0, "ksa_fresh_zero");
    rka.activate_kill_switch();
    ASSERT(rka.kill_switch_activations() == 1, "ksa_manual_counts");
    rka.activate_kill_switch();                  // already on — re-assert only
    ASSERT(rka.kill_switch_activations() == 1, "ksa_reassert_not_counted");
    rka.deactivate_kill_switch();
    rka.activate_kill_switch();                  // a second real halt
    ASSERT(rka.kill_switch_activations() == 2, "ksa_second_halt_counts");
    rka.deactivate_kill_switch();
    // A breaker latch (loss streak of 2) is a fresh activation; the third
    // loss re-asserts the already-tripped switch without re-counting.
    rka.update_pnl(-1.0);
    rka.update_pnl(-1.0);
    ASSERT(rka.is_kill_switch_active() && rka.kill_switch_activations() == 3,
           "ksa_streak_latch_counts");
    rka.update_pnl(-1.0);
    ASSERT(rka.kill_switch_activations() == 3, "ksa_tripped_reassert_ignored");
    rka.reset_daily();
    ASSERT(rka.kill_switch_activations() == 0, "ksa_reset_daily");

    // #121 reason the kill switch latched.
    RiskManager rk(lim);
    ASSERT(rk.get_kill_reason() == KillReason::NONE, "killreason_none");
    rk.activate_kill_switch();
    ASSERT(rk.get_kill_reason() == KillReason::MANUAL, "killreason_manual");
    rk.deactivate_kill_switch();
    ASSERT(rk.get_kill_reason() == KillReason::NONE, "killreason_cleared");
    RiskLimits cl2; cl2.max_consecutive_losses = 2;
    RiskManager rcr(cl2);
    rcr.update_pnl(-10.0); rcr.update_pnl(-10.0);
    ASSERT(rcr.get_kill_reason() == KillReason::CONSECUTIVE_LOSSES, "killreason_consec");
    RiskLimits cb; cb.max_daily_loss = 100;
    RiskManager rbk(cb);
    rbk.update_pnl(-200.0);                                   // above the daily limit
    rbk.check_order("AAPL", Side::BUY, 10.0, 1);              // circuit breaker w check
    ASSERT(rbk.get_kill_reason() == KillReason::CIRCUIT_BREAKER, "killreason_circuit");

    // #129 runtime limit update — tighten position limits intraday.
    RiskManager rl(lim);
    ASSERT(rl.check_order("AAPL", Side::BUY, 1.0, 500).action == RiskAction::ALLOW,
           "setlim_before_ok");                       // 500 < lim (1000000)
    RiskLimits tighter = rl.get_limits();
    tighter.max_position_per_symbol = 100;
    rl.set_limits(tighter);
    ASSERT(rl.get_limits().max_position_per_symbol == 100, "setlim_applied");
    ASSERT(rl.check_order("AAPL", Side::BUY, 1.0, 500).action == RiskAction::REJECT,
           "setlim_after_rejects");                    // 500 > 100 now

    // #137 exposure queries (|pos+pend|, total = O(1) invariant).
    RiskManager re(lim);
    re.on_order_sent("AAPL", Side::BUY, 100);          // pending +100
    re.on_order_sent("MSFT", Side::SELL, 50);          // pending -50
    ASSERT(re.get_exposure("AAPL") == 100, "exp_aapl_100");
    ASSERT(re.get_exposure("MSFT") == 50, "exp_msft_50");
    ASSERT(re.get_total_exposure() == 150, "exp_total_150");
    re.update_position("AAPL", Side::BUY, 60);          // pos+60 pend-60 -> bez zmian
    ASSERT(re.get_exposure("AAPL") == 100 && re.get_total_exposure() == 150,
           "exp_invariant_after_fill");

    // #331 largest_symbol_exposure / exposure_concentration
    ASSERT(re.largest_symbol_exposure() == 100, "exp_largest_symbol_aapl");   // AAPL 100 > MSFT 50
    ASSERT(std::fabs(re.exposure_concentration() - 100.0/150.0) < 1e-9, "exp_concentration_two_thirds");
    RiskManager rflat(lim);                              // flat book -> 0
    ASSERT(rflat.largest_symbol_exposure() == 0 && rflat.exposure_concentration() == 0.0,
           "exp_concentration_flat");
    RiskManager rconc(lim);                              // single name -> fully concentrated
    rconc.on_order_sent("AAPL", Side::BUY, 300);
    ASSERT(std::fabs(rconc.exposure_concentration() - 1.0) < 1e-9, "exp_concentration_single_name");

    // #389 key_to_sym + largest_exposure_symbol — the actionable WHICH.
    {
        char les[9];
        // key_to_sym is the exact inverse of sym_to_key (lossless packing).
        key_to_sym(sym_to_key("AAPL"), les);
        ASSERT(std::strcmp(les, "AAPL") == 0, "k2s_roundtrip_short");
        key_to_sym(sym_to_key("ABCDEFGH"), les);         // full 8 chars
        ASSERT(std::strcmp(les, "ABCDEFGH") == 0, "k2s_roundtrip_full8");
        key_to_sym(0, les);
        ASSERT(les[0] == '\0', "k2s_zero_key_empty");
        // re: AAPL |pos+pend| 100 vs MSFT 50 -> AAPL, value equals #331.
        ASSERT(re.largest_exposure_symbol(les) == re.largest_symbol_exposure()
               && std::strcmp(les, "AAPL") == 0, "les_names_aapl");
        // Flat book: 0 and an empty name.
        RiskManager rlesf(lim);
        les[0] = 'X';
        ASSERT(rlesf.largest_exposure_symbol(les) == 0 && les[0] == '\0', "les_flat_empty");
        // A pending-only symbol (no position entry) can win the walk.
        RiskManager rlesp(lim);
        rlesp.update_position("MSFT", Side::BUY, 40);    // position-backed 40
        rlesp.on_order_sent("TSLA", Side::SELL, 90);     // pending-only 90
        ASSERT(rlesp.largest_exposure_symbol(les) == 90 && std::strcmp(les, "TSLA") == 0,
               "les_pending_only_wins");
    }

    // #144 daily turnover limit.
    RiskLimits tl;
    tl.max_daily_traded_notional = 1000.0;
    RiskManager rt(tl);
    ASSERT(rt.check_order("AAPL", Side::BUY, 10.0, 10).action == RiskAction::ALLOW,
           "turnover_under_ok");
    rt.add_traded_notional(1500.0);                     // turnover exceeded
    ASSERT(std::fabs(rt.get_traded_notional() - 1500.0) < 1e-9, "turnover_tracked");
    ASSERT(rt.check_order("AAPL", Side::BUY, 10.0, 10).action == RiskAction::REJECT,
           "turnover_over_rejects");

    // #153 notional exposure in $.
    RiskManager rn(lim);
    rn.update_reference_price("AAPL", 150.0);
    rn.on_order_sent("AAPL", Side::BUY, 100);          // 100 shares
    ASSERT(std::fabs(rn.get_position_notional("AAPL") - 15000.0) < 1e-6, "notional_15000");
    ASSERT(rn.get_position_notional("MSFT") == 0.0, "notional_no_ref_zero");

    // #161 per-symbol position-limit override.
    RiskLimits pl; pl.max_position_per_symbol = 1000;
    RiskManager rp(pl);
    ASSERT(rp.check_order("TSLA", Side::BUY, 1.0, 500).action == RiskAction::ALLOW,
           "symlim_global_ok");                       // 500 < 1000
    rp.set_symbol_position_limit("TSLA", 100);        // tighter for TSLA
    ASSERT(rp.check_order("TSLA", Side::BUY, 1.0, 500).action == RiskAction::REJECT,
           "symlim_override_rejects");                 // 500 > 100
    ASSERT(rp.check_order("AAPL", Side::BUY, 1.0, 500).action == RiskAction::ALLOW,
           "symlim_other_uses_global");                // AAPL: globalny 1000
    rp.set_symbol_position_limit("TSLA", 0);          // zdejmij override
    ASSERT(rp.check_order("TSLA", Side::BUY, 1.0, 500).action == RiskAction::ALLOW,
           "symlim_override_removed");

    // #167 early limit warning (cap 1000).
    RiskManager rw(pl);
    rw.on_order_sent("AAPL", Side::BUY, 700);          // exposure 700 = 70%
    ASSERT(!rw.is_near_position_limit("AAPL", 80.0), "warn_below_80pct");   // 700 < 800
    ASSERT(rw.is_near_position_limit("AAPL", 60.0), "warn_above_60pct");    // 700 >= 600

    // #181 portfolio exposure budget utilization (limit 1000).
    RiskLimits el; el.max_portfolio_exposure = 1000;
    RiskManager reu(el);
    reu.on_order_sent("AAA", Side::BUY, 300);                               // |300| = 30%
    ASSERT(std::fabs(reu.exposure_utilization_pct() - 30.0) < 1e-6, "exput_30pct");
    reu.on_order_sent("BBB", Side::SELL, 200);                              // +|200| = 50%
    ASSERT(std::fabs(reu.exposure_utilization_pct() - 50.0) < 1e-6, "exput_50pct");

    // #252 exposure_headroom (portfolio cap 1000).
    RiskLimits ehl; ehl.max_portfolio_exposure = 1000;
    RiskManager reh(ehl);
    reh.on_order_sent("AAA", Side::BUY, 300);                       // |300|
    ASSERT(reh.exposure_headroom() == 700, "exph_700");            // 1000 - 300
    reh.on_order_sent("BBB", Side::SELL, 500);                      // +|500| = 800
    ASSERT(reh.exposure_headroom() == 200, "exph_200");
    reh.on_order_sent("CCC", Side::BUY, 400);                       // 1200 > cap
    ASSERT(reh.exposure_headroom() == 0, "exph_clamped_zero");

    // #283 total_pending_exposure (working-order exposure).
    RiskManager rpe2;
    rpe2.on_order_sent("AAA", Side::BUY, 300);                      // pending +300
    rpe2.on_order_sent("BBB", Side::SELL, 200);                     // pending -200
    ASSERT(rpe2.total_pending_exposure() == 500, "pending_exp_sum");  // 300 + 200

    // #299 net_exposure (signed tilt) vs get_total_exposure (gross).
    RiskManager nex;
    nex.on_order_sent("AAPL", Side::BUY, 300);                      // +300
    nex.on_order_sent("MSFT", Side::SELL, 200);                     // -200
    ASSERT(nex.net_exposure() == 100, "netexp_long_tilt");          // +300 -200 = net long 100
    ASSERT(nex.get_total_exposure() == 500, "netexp_gross_500");    // |300| + |200|
    nex.on_order_sent("AAPL", Side::SELL, 600);                     // AAPL pending 300-600 = -300
    ASSERT(nex.net_exposure() == -500, "netexp_short_tilt");        // -300 -200
    ASSERT(nex.get_total_exposure() == 500, "netexp_gross_still_500"); // |-300| + |-200|

    // #315 long_exposure / short_exposure (derived from gross & net).
    RiskManager lse;
    lse.on_order_sent("AAPL", Side::BUY, 300);                      // +300
    lse.on_order_sent("MSFT", Side::SELL, 200);                     // -200
    // gross 500, net 100 -> long (500+100)/2=300, short (500-100)/2=200
    ASSERT(lse.long_exposure() == 300 && lse.short_exposure() == 200, "lse_split_long_tilt");
    lse.on_order_sent("AAPL", Side::SELL, 600);                     // AAPL -> -300
    // gross 500, net -500 -> long 0, short 500
    ASSERT(lse.long_exposure() == 0 && lse.short_exposure() == 500, "lse_all_short");

    // #323 portfolio_skew = net / gross in [-1, 1]
    RiskLimits skl; skl.max_portfolio_exposure = 10000;
    RiskManager rskew(skl);
    // flat book -> skew = 0
    ASSERT(std::fabs(rskew.portfolio_skew()) < 1e-9, "skew_flat");
    // fully long 300: gross 300, net 300 -> skew = 1.0
    rskew.on_order_sent("AAPL", Side::BUY, 300);
    ASSERT(std::fabs(rskew.portfolio_skew() - 1.0) < 1e-9, "skew_fully_long");
    // add offsetting short 200: MSFT -200; gross 500, net 100 -> skew = 0.2
    rskew.on_order_sent("MSFT", Side::SELL, 200);
    ASSERT(std::fabs(rskew.portfolio_skew() - 100.0/500.0) < 1e-9, "skew_net_long");
    // flip to net short: AAPL -> sell 600 more, AAPL net -300; gross 500, net -500 -> skew = -1
    rskew.on_order_sent("AAPL", Side::SELL, 600);
    ASSERT(std::fabs(rskew.portfolio_skew() - (-1.0)) < 1e-9, "skew_fully_short");

    // #237 position_utilization_pct (per symbol, cap 1000).
    RiskLimits pul; pul.max_position_per_symbol = 1000;
    RiskManager rpu(pul);
    rpu.on_order_sent("AAPL", Side::BUY, 700);                              // 700/1000 = 70%
    ASSERT(std::fabs(rpu.position_utilization_pct("AAPL") - 70.0) < 1e-6, "posutil_70pct");
    rpu.set_symbol_position_limit("AAPL", 2000);                           // override cap 2000
    ASSERT(std::fabs(rpu.position_utilization_pct("AAPL") - 35.0) < 1e-6, "posutil_override_35pct");
    ASSERT(std::fabs(rpu.position_utilization_pct("MSFT") - 0.0) < 1e-6, "posutil_no_exposure");

    // #245 headroom_shares (cap 1000, symetryczny short).
    RiskLimits hrl; hrl.max_position_per_symbol = 1000;
    RiskManager rhr(hrl);
    rhr.on_order_sent("AAPL", Side::BUY, 300);                              // position +300
    ASSERT(rhr.headroom_shares("AAPL", Side::BUY) == 700, "headroom_buy");  // 1000 - 300
    ASSERT(rhr.headroom_shares("AAPL", Side::SELL) == 1300, "headroom_sell"); // 1000 + 300 (flip)
    ASSERT(rhr.headroom_shares("MSFT", Side::BUY) == 1000, "headroom_fresh"); // full cap

    // #259 projected_exposure (resulting |position| after a hypothetical fill).
    RiskManager rpe(hrl);
    rpe.on_order_sent("AAPL", Side::BUY, 300);                              // position +300
    ASSERT(rpe.projected_exposure("AAPL", Side::BUY, 200) == 500, "projexp_buy");   // |300+200|
    ASSERT(rpe.projected_exposure("AAPL", Side::SELL, 100) == 200, "projexp_sell"); // |300-100|
    ASSERT(rpe.projected_exposure("AAPL", Side::SELL, 500) == 200, "projexp_flip"); // |300-500|
    ASSERT(rpe.projected_exposure("MSFT", Side::BUY, 100) == 100, "projexp_fresh");

    // #291 would_breach_position (cap 1000; AAPL already +300 from #259 setup).
    ASSERT(!rpe.would_breach_position("AAPL", Side::BUY, 700), "breach_at_cap_ok");   // 300+700=1000
    ASSERT(rpe.would_breach_position("AAPL", Side::BUY, 701), "breach_over_cap");     // 1001 > 1000
    ASSERT(!rpe.would_breach_position("AAPL", Side::SELL, 100), "breach_sell_ok");    // 200
    ASSERT(!rpe.would_breach_position("MSFT", Side::BUY, 1000), "breach_fresh_at_cap"); // exactly cap
    ASSERT(rpe.would_breach_position("MSFT", Side::BUY, 1001), "breach_fresh_over");

    // #413 would_breach_exposure — the PORTFOLIO-level what-if (#291's twin).
    RiskLimits wbl;
    wbl.max_position_per_symbol = 100000;
    wbl.max_portfolio_exposure  = 1000;
    wbl.max_order_value         = 100000000;
    wbl.max_orders_per_second   = 1000000;
    wbl.max_price_band_pct      = 0.0;
    RiskManager rwb(wbl);
    rwb.on_order_sent("AAPL", Side::BUY, 600);            // gross 600
    rwb.on_order_sent("MSFT", Side::SELL, 300);           // gross 900
    ASSERT(!rwb.would_breach_exposure("TSLA", Side::BUY, 100), "wbe_at_cap_ok");   // 1000
    ASSERT(rwb.would_breach_exposure("TSLA", Side::BUY, 101), "wbe_over_cap");     // 1001
    // Reducing an existing name FREES exposure — never a breach.
    ASSERT(!rwb.would_breach_exposure("AAPL", Side::SELL, 400), "wbe_reduction_ok"); // gross 500
    // Flipping through zero can still breach: 600 -> -1101 => gross 1401.
    ASSERT(rwb.would_breach_exposure("AAPL", Side::SELL, 1701), "wbe_flip_breaches");
    // Probing is side-effect-free — the real check counters stay untouched.
    const uint64_t wbe_checks = rwb.get_total_checks();
    (void)rwb.would_breach_exposure("TSLA", Side::BUY, 50);
    ASSERT(rwb.get_total_checks() == wbe_checks, "wbe_no_side_effects");
    // A disabled cap (<= 0) never breaches.
    RiskLimits wbl0 = wbl;
    wbl0.max_portfolio_exposure = 0;
    RiskManager rwb0(wbl0);
    ASSERT(!rwb0.would_breach_exposure("AAPL", Side::BUY, 99999999), "wbe_disabled_never");

    // #429 rate_limit_headroom — the pre-trade probe for check #7.
    // now=0 keeps every real (large, positive) mono timestamp in the window,
    // so the counts below are deterministic.
    RiskLimits rlh;
    rlh.max_position_per_symbol = 100000;
    rlh.max_portfolio_exposure  = 100000000;
    rlh.max_order_value         = 100000000;
    rlh.max_orders_per_second   = 5;
    rlh.max_price_band_pct      = 0.0;
    RiskManager rrl(rlh);
    ASSERT(rrl.rate_limit_headroom(0) == 5, "rlh_fresh_full");
    rrl.check_order("AAPL", Side::BUY, 10.0, 1);
    rrl.check_order("AAPL", Side::BUY, 10.0, 1);
    ASSERT(rrl.rate_limit_headroom(0) == 3, "rlh_two_used");
    // Probing does not consume a slot: ask twice, still 3.
    ASSERT(rrl.rate_limit_headroom(0) == 3, "rlh_probe_free");
    rrl.check_order("AAPL", Side::BUY, 10.0, 1);
    rrl.check_order("AAPL", Side::BUY, 10.0, 1);
    rrl.check_order("AAPL", Side::BUY, 10.0, 1);
    ASSERT(rrl.rate_limit_headroom(0) == 0, "rlh_window_exhausted");
    // The 6th order in the window is what check #7 rejects.
    ASSERT(rrl.check_order("AAPL", Side::BUY, 10.0, 1).action == RiskAction::REJECT,
           "rlh_sixth_rejected");
    ASSERT(rrl.rate_limit_headroom(0) == 0, "rlh_floored_at_zero");
    // Disabled limit -> -1 sentinel.
    RiskLimits rlh0 = rlh;
    rlh0.max_orders_per_second = 0;
    RiskManager rrl0(rlh0);
    ASSERT(rrl0.rate_limit_headroom(0) == -1, "rlh_disabled_minus1");

    // #437 would_breach_symbol_notional — the per-name $ what-if (#189's probe).
    RiskLimits wsn;
    wsn.max_position_per_symbol = 100000;
    wsn.max_portfolio_exposure  = 100000000;
    wsn.max_order_value         = 100000000;
    wsn.max_orders_per_second   = 1000000;
    wsn.max_price_band_pct      = 0.0;
    wsn.max_symbol_notional     = 10000.0;        // $10k per name
    RiskManager rsnw(wsn);
    // 100 shares at $99: projected $9900 <= $10k; at $101: $10100 breaches.
    ASSERT(!rsnw.would_breach_symbol_notional("AAPL", Side::BUY, 100, 99.0),
           "wsn_under_cap_ok");
    ASSERT(rsnw.would_breach_symbol_notional("AAPL", Side::BUY, 100, 101.0),
           "wsn_over_cap_breaches");
    // Existing exposure counts: 80 held + 30 more at $100 = $11000.
    rsnw.on_order_sent("AAPL", Side::BUY, 80);
    ASSERT(rsnw.would_breach_symbol_notional("AAPL", Side::BUY, 30, 100.0),
           "wsn_existing_counts");
    // Reducing the name frees notional — never a breach on the way down.
    ASSERT(!rsnw.would_breach_symbol_notional("AAPL", Side::SELL, 50, 100.0),
           "wsn_reduction_ok");
    // Probing is side-effect-free.
    const uint64_t wsn_checks = rsnw.get_total_checks();
    (void)rsnw.would_breach_symbol_notional("AAPL", Side::BUY, 1, 1.0);
    ASSERT(rsnw.get_total_checks() == wsn_checks, "wsn_no_side_effects");
    // Disabled cap never breaches.
    RiskLimits wsn0 = wsn;
    wsn0.max_symbol_notional = 0.0;
    RiskManager rsn0w(wsn0);
    ASSERT(!rsn0w.would_breach_symbol_notional("AAPL", Side::BUY, 99999, 999.0),
           "wsn_disabled_never");

    // #445 would_breach_price_band — the fat-finger what-if (check #2b's probe).
    RiskLimits wpb;
    wpb.max_position_per_symbol = 100000;
    wpb.max_portfolio_exposure  = 100000000;
    wpb.max_order_value         = 100000000;
    wpb.max_orders_per_second   = 1000000;
    wpb.max_price_band_pct      = 10.0;            // ±10% around the reference
    RiskManager rpb(wpb);
    // No reference yet: the real check SKIPS, so the probe must say false.
    ASSERT(!rpb.would_breach_price_band("AAPL", 999999.0), "wpb_no_ref_skips");
    rpb.update_reference_price("AAPL", 100.0);
    ASSERT(!rpb.would_breach_price_band("AAPL", 109.0), "wpb_inside_band_ok");
    ASSERT(!rpb.would_breach_price_band("AAPL", 91.0), "wpb_inside_low_side_ok");
    ASSERT(rpb.would_breach_price_band("AAPL", 111.0), "wpb_high_side_breaches");
    // The classic fat finger: 1500.00 instead of 150-ish.
    ASSERT(rpb.would_breach_price_band("AAPL", 1500.0), "wpb_gross_mistake");
    // Probing is side-effect-free and agrees with the real check.
    const uint64_t wpb_checks = rpb.get_total_checks();
    (void)rpb.would_breach_price_band("AAPL", 111.0);
    ASSERT(rpb.get_total_checks() == wpb_checks, "wpb_no_side_effects");
    ASSERT(rpb.check_order("AAPL", Side::BUY, 111.0, 10).action == RiskAction::REJECT,
           "wpb_agrees_with_check_order");
    // Disabled band never breaches.
    RiskLimits wpb0 = wpb;
    wpb0.max_price_band_pct = 0.0;
    RiskManager rpb0(wpb0);
    rpb0.update_reference_price("AAPL", 100.0);
    ASSERT(!rpb0.would_breach_price_band("AAPL", 999999.0), "wpb_disabled_never");

    // #189 per-symbol position value limit ($10k; shares generous).
    RiskLimits snl;
    snl.max_symbol_notional    = 10000.0;
    snl.max_position_per_symbol = 100000;
    snl.max_portfolio_exposure  = 100000000;
    snl.max_order_value         = 100000000;
    snl.max_orders_per_second   = 1000000;
    snl.max_price_band_pct      = 0.0;
    RiskManager rsn(snl);
    ASSERT(rsn.check_order("AAPL", Side::BUY, 50.0, 100).action == RiskAction::ALLOW,
           "symnotional_under_ok");    // 100*50 = 5000
    ASSERT(rsn.check_order("TSLA", Side::BUY, 50.0, 300).action == RiskAction::REJECT,
           "symnotional_over_rejects"); // 300*50 = 15000 > 10000

    // #356 symbol_notional_utilization_pct — $ analog of position_utilization_pct (#237).
    RiskManager rsu(snl);
    rsu.update_reference_price("AAPL", 50.0);
    rsu.on_order_sent("AAPL", Side::BUY, 100);   // 100 * 50 = 5000 notional / 10000 limit
    ASSERT(std::fabs(rsu.symbol_notional_utilization_pct("AAPL") - 50.0) < 1e-6, "symnotutil_50pct");
    ASSERT(rsu.symbol_notional_utilization_pct("MSFT") == 0.0, "symnotutil_no_position_zero");
    RiskManager rsu0;   // default limits: max_symbol_notional off (0.0)
    rsu0.update_reference_price("AAPL", 50.0);
    rsu0.on_order_sent("AAPL", Side::BUY, 100);
    ASSERT(rsu0.symbol_notional_utilization_pct("AAPL") == 0.0, "symnotutil_limit_off_zero");

    // #197 current_drawdown_pct (high-water mark).
    RiskManager rdd;                       // default limits
    rdd.update_pnl(1000.0);                 // peak 1000, daily 1000
    ASSERT(std::fabs(rdd.current_drawdown_pct() - 0.0) < 1e-6, "dd_at_peak_zero");
    rdd.update_pnl(-300.0);                 // daily 700 -> dd = 300/1000 = 30%
    ASSERT(std::fabs(rdd.current_drawdown_pct() - 30.0) < 1e-6, "dd_30pct");
    ASSERT(std::fabs(rdd.get_peak_pnl() - 1000.0) < 1e-6, "dd_peak_1000");
    RiskManager rzz;
    rzz.update_pnl(-500.0);                 // peak 0 -> no reference
    ASSERT(std::fabs(rzz.current_drawdown_pct() - 0.0) < 1e-6, "dd_no_peak_zero");

    // #267 drawdown_headroom_pct (limit 10%).
    RiskLimits ddl; ddl.max_drawdown_pct = 10.0;
    RiskManager rdh(ddl);
    rdh.update_pnl(1000.0);                  // peak 1000
    rdh.update_pnl(-50.0);                   // dd = 50/1000 = 5%
    ASSERT(std::fabs(rdh.drawdown_headroom_pct() - 5.0) < 1e-6, "ddh_5pct");   // 10 - 5
    rdh.update_pnl(-60.0);                   // dd = 110/1000 = 11% > 10
    ASSERT(std::fabs(rdh.drawdown_headroom_pct() - 0.0) < 1e-6, "ddh_clamped");

    // #275 current_drawdown_dollars (absolute $ drawdown).
    RiskManager rdd2;
    rdd2.update_pnl(1000.0);                  // peak 1000
    rdd2.update_pnl(-300.0);                  // daily 700 -> dd $300
    ASSERT(std::fabs(rdd2.current_drawdown_dollars() - 300.0) < 1e-6, "dd_dollars_300");
    rdd2.update_pnl(500.0);                   // daily 1200 -> new high -> dd 0
    ASSERT(std::fabs(rdd2.current_drawdown_dollars() - 0.0) < 1e-6, "dd_dollars_new_high");

    // #340 max_drawdown_dollars (worst peak-to-trough $ decline this session;
    // unlike current_drawdown_dollars it survives a full recovery).
    RiskManager rmdd;
    rmdd.update_pnl(1000.0);                  // peak 1000
    rmdd.update_pnl(-300.0);                  // daily 700 -> dd $300, max_dd $300
    ASSERT(std::fabs(rmdd.max_drawdown_dollars() - 300.0) < 1e-6, "mdd_first_300");
    rmdd.update_pnl(500.0);                   // daily 1200 -> new high, current dd 0, max_dd still 300
    ASSERT(std::fabs(rmdd.max_drawdown_dollars() - 300.0) < 1e-6, "mdd_survives_recovery");
    // #493 pnl_recovery_factor: daily 1200 over max_dd 300 -> 4.0 (fully
    // recovered and then some).
    ASSERT(std::fabs(rmdd.pnl_recovery_factor() - 4.0) < 1e-9, "rf_four");
    rmdd.update_pnl(-800.0);                  // daily 400 -> dd from peak 1200 = 800 > 300
    ASSERT(std::fabs(rmdd.max_drawdown_dollars() - 800.0) < 1e-6, "mdd_new_worse_trough");
    // #493: daily 400 over max_dd 800 -> 0.5 (the hole still exceeds profit).
    ASSERT(std::fabs(rmdd.pnl_recovery_factor() - 0.5) < 1e-9, "rf_half");
    // #493: an only-up session has no drawdown -> recovery factor 0 (not inf).
    RiskManager rfu;
    rfu.update_pnl(+100.0); rfu.update_pnl(+50.0);
    ASSERT(rfu.pnl_recovery_factor() == 0.0, "rf_no_drawdown_zero");
    rmdd.reset_daily();
    ASSERT(std::fabs(rmdd.max_drawdown_dollars() - 0.0) < 1e-6, "mdd_reset_daily");
    ASSERT(rmdd.pnl_recovery_factor() == 0.0, "rf_reset_zero");

    // #205 consecutive_losses_remaining (loss-streak breaker, threshold 3).
    RiskLimits cll; cll.max_consecutive_losses = 3;
    RiskManager rcl(cll);
    ASSERT(rcl.consecutive_losses_remaining() == 3, "clr_full_at_start");
    rcl.update_pnl(-10.0);                  // loss 1
    ASSERT(rcl.consecutive_losses_remaining() == 2, "clr_after_one_loss");
    rcl.update_pnl(-10.0);                  // loss 2
    ASSERT(rcl.consecutive_losses_remaining() == 1, "clr_after_two_losses");
    rcl.update_pnl(5.0);                    // a profit resets the streak
    ASSERT(rcl.consecutive_losses_remaining() == 3, "clr_reset_on_win");
    RiskManager rcd;                        // disabled by default (0)
    ASSERT(rcd.consecutive_losses_remaining() == -1, "clr_disabled");

    // #213 remaining_loss_budget (max_daily_loss 1000).
    RiskLimits lbl; lbl.max_daily_loss = 1000;
    RiskManager rlb(lbl);
    ASSERT(std::fabs(rlb.remaining_loss_budget() - 1000.0) < 1e-6, "rlb_full_at_start");
    rlb.update_pnl(-300.0);                 // budget 700
    ASSERT(std::fabs(rlb.remaining_loss_budget() - 700.0) < 1e-6, "rlb_after_loss");
    rlb.update_pnl(-800.0);                 // past the threshold -> clamp 0
    ASSERT(std::fabs(rlb.remaining_loss_budget() - 0.0) < 1e-6, "rlb_clamped_zero");
    RiskManager rlp(lbl);
    rlp.update_pnl(500.0);                  // a profit increases the budget -> 1500
    ASSERT(std::fabs(rlp.remaining_loss_budget() - 1500.0) < 1e-6, "rlb_profit_extends");

    // #307 loss_budget_utilization_pct (same 1000 limit).
    ASSERT(std::fabs(rlb.loss_budget_utilization_pct() - 100.0) < 1e-6, "lbu_clamped_full"); // -1100/1000
    ASSERT(rlp.loss_budget_utilization_pct() == 0.0, "lbu_profit_zero");        // in profit
    RiskManager rlu(lbl);
    rlu.update_pnl(-250.0);                  // 250 of 1000 -> 25%
    ASSERT(std::fabs(rlu.loss_budget_utilization_pct() - 25.0) < 1e-6, "lbu_quarter");

    // #221 daily_turnover_pct (turnover limit 100000).
    RiskLimits tnl; tnl.max_daily_traded_notional = 100000.0;
    RiskManager rtn(tnl);
    rtn.add_traded_notional(30000.0);
    ASSERT(std::fabs(rtn.daily_turnover_pct() - 30.0) < 1e-6, "turnover_30pct");
    rtn.add_traded_notional(20000.0);       // total 50000
    ASSERT(std::fabs(rtn.daily_turnover_pct() - 50.0) < 1e-6, "turnover_50pct");
    RiskManager rtd;                        // limit disabled (0)
    rtd.add_traded_notional(50000.0);
    ASSERT(std::fabs(rtd.daily_turnover_pct() - 0.0) < 1e-6, "turnover_disabled");

    // #453 remaining_turnover_budget — the $ headroom behind #221's percent.
    // rtn stands at 50000 of 100000 -> 50000 left.
    ASSERT(std::fabs(rtn.remaining_turnover_budget() - 50000.0) < 1e-6, "tob_half_left");
    rtn.add_traded_notional(49999.0);
    ASSERT(std::fabs(rtn.remaining_turnover_budget() - 1.0) < 1e-6, "tob_one_dollar_left");
    // Exhaustion floors at 0 — and check 2c starts rejecting exactly there.
    rtn.add_traded_notional(1.0);
    ASSERT(rtn.remaining_turnover_budget() == 0.0, "tob_exhausted_zero");
    ASSERT(rtn.check_order("AAPL", Side::BUY, 1.0, 1).action == RiskAction::REJECT,
           "tob_agrees_with_check_2c");
    // Overshoot stays floored; the disabled limit reads -1 (unlimited).
    rtn.add_traded_notional(5000.0);
    ASSERT(rtn.remaining_turnover_budget() == 0.0, "tob_overshoot_floored");
    ASSERT(rtd.remaining_turnover_budget() == -1.0, "tob_disabled_minus1");
    // reset_daily refills the whole budget.
    rtn.reset_daily();
    ASSERT(std::fabs(rtn.remaining_turnover_budget() - 100000.0) < 1e-6, "tob_reset_refills");

    // #229 check_reject_rate (small max_order_value -> some checks rejected).
    RiskLimits crl;
    crl.max_order_value        = 500;
    crl.max_position_per_symbol = 100000000;
    crl.max_portfolio_exposure  = 100000000;
    crl.max_orders_per_second   = 1000000;
    crl.max_price_band_pct      = 0.0;
    RiskManager rckr(crl);
    rckr.check_order("AAA", Side::BUY, 10.0, 10);    // value 100 <= 500 -> pass
    rckr.check_order("AAA", Side::BUY, 10.0, 100);   // value 1000 > 500 -> reject
    rckr.check_order("AAA", Side::BUY, 10.0, 100);   // reject
    ASSERT(rckr.get_total_checks() == 3 && rckr.get_total_rejects() == 2, "crr_counts");
    ASSERT(std::fabs(rckr.check_reject_rate() - 2.0/3.0) < 1e-9, "crr_rate_two_thirds");

    // #94 fat-finger on quantity — a qty cap independent of notional.
    RiskLimits ql;
    ql.max_shares_per_order = 1000;
    ql.max_order_value = 100000000;  // hojny notional, izolujemy qty
    ql.max_position_per_symbol = 100000000;
    ql.max_portfolio_exposure  = 100000000;
    ql.max_orders_per_second   = 1000000;
    ql.max_price_band_pct      = 0.0;
    RiskManager r5(ql);
    ASSERT(r5.check_order("PENY", Side::BUY, 0.01, 1000).action == RiskAction::ALLOW,
           "qtycap_at_limit_ok");
    ASSERT(r5.check_order("PENY", Side::BUY, 0.01, 1001).action == RiskAction::REJECT,
           "qtycap_over_rejects");   // cheap notional ($10), but 1001 shares > limit

    // #175 minimum price threshold (penny-stock filter).
    RiskLimits ml;
    ml.min_price = 1.0;
    RiskManager rmp(ml);
    ASSERT(rmp.check_order("PENY", Side::BUY, 0.50, 100).action == RiskAction::REJECT,
           "minprice_below_rejects");
    ASSERT(rmp.check_order("AAPL", Side::BUY, 150.0, 100).action == RiskAction::ALLOW,
           "minprice_above_ok");
}


// FIX session #78 — persystencja seq + buildery admin messages.
void test_fix_session() {
    SECTION("FIX Session (#78)");

    // --- Admin builder: Logon parses as a valid FIX with 8/9/10 ---
    {
        fix::FIXSession s;
        s.set_comp_ids("TRADER1", "EXCH");
        char buf[256];
        const int n = s.build_logon(buf, sizeof(buf), 30, '|');  // '|' = human-readable
        ASSERT(n > 0, "fix_logon_built");
        FIXMessage m;
        m.parse(buf);
        ASSERT(m.is_valid(), "fix_logon_valid_checksum_bodylen");
        ASSERT(m.get_msg_type()[0] == 'A', "fix_logon_msgtype_A");
        const char* seq = m.get_field(34);
        ASSERT(seq && std::atoi(seq) == 1, "fix_logon_seq_1");
        ASSERT(s.peek_outbound_seq() == 2, "fix_seq_incremented");
    }

    // --- #119 inbound SequenceReset: set the expected seq, ignore a backward one ---
    {
        fix::FIXSession si;
        si.observe_inbound(1, 100);
        ASSERT(si.expected_inbound_seq() == 2, "seqreset_expected_2");
        const auto g = si.observe_inbound(5, 200);                 // gap 2..4
        ASSERT(g.valid && si.expected_inbound_seq() == 6, "seqreset_after_gap_6");
        si.apply_inbound_sequence_reset(10);                       // NewSeqNo=10
        ASSERT(si.expected_inbound_seq() == 10, "seqreset_applied_10");
        si.apply_inbound_sequence_reset(3);                        // wsteczny -> ignoruj
        ASSERT(si.expected_inbound_seq() == 10, "seqreset_ignores_backwards");
    }

    // --- ResendRequest carries BeginSeqNo(7)/EndSeqNo(16) and bumps the counter ---
    {
        fix::FIXSession s;
        char buf[256];
        const int n = s.build_resend_request(buf, sizeof(buf), 5, 0, '|');
        ASSERT(n > 0, "fix_resend_built");
        FIXMessage m; m.parse(buf);
        ASSERT(m.is_valid(), "fix_resend_valid");
        ASSERT(m.get_msg_type()[0] == '2', "fix_resend_msgtype_2");
        ASSERT(std::atoi(m.get_field(7)) == 5, "fix_resend_begin_5");
        ASSERT(s.resends_requested() == 1, "fix_resend_counted");
    }

    // --- SequenceReset GapFill: 36=NewSeqNo, 123=Y ---
    {
        fix::FIXSession s;
        char buf[256];
        s.build_sequence_reset(buf, sizeof(buf), 42, true, '|');
        FIXMessage m; m.parse(buf);
        ASSERT(m.is_valid(), "fix_seqreset_valid");
        ASSERT(std::atoi(m.get_field(36)) == 42, "fix_seqreset_newseq_42");
        ASSERT(m.get_field(123)[0] == 'Y', "fix_seqreset_gapfill_Y");
    }

    // --- Seq persistence: send a few, persist, new session, load, continue ---
    {
        const char* path = "/tmp/fix_seq_test.dat";
        std::remove(path);
        fix::FIXSession s1;
        s1.set_persist_path(path);
        char buf[256];
        s1.build_logon(buf, sizeof(buf), 30, '|');         // seq 1 → next 2
        s1.build_heartbeat(buf, sizeof(buf), nullptr, '|'); // seq 2 → next 3
        s1.observe_inbound(1, 1000);                        // expected_in → 2
        s1.persist_seq();

        fix::FIXSession s2;
        s2.set_persist_path(path);
        ASSERT(s2.load_persisted_seq(), "fix_persist_loaded");
        ASSERT(s2.peek_outbound_seq() == 3, "fix_persist_out_seq_continues");
        // After a restart the next build uses seq 3 — does not reuse 1/2.
        s2.build_heartbeat(buf, sizeof(buf), nullptr, '|');
        FIXMessage m; m.parse(buf);
        ASSERT(std::atoi(m.get_field(34)) == 3, "fix_persist_no_seq_reuse");
        std::remove(path);
    }

    // --- #90 order-entry builders (35=D/F/G) ---
    {
        fix::FIXSession s; s.set_comp_ids("ME", "EX");
        char buf[256];
        int n = s.build_new_order(buf, sizeof(buf), "ORD1", "AAPL", Side::BUY, 100, 150.25, '|');
        ASSERT(n > 0, "fix_neworder_built");
        FIXMessage m; m.parse(buf);
        ASSERT(m.is_valid() && m.get_msg_type()[0] == 'D', "fix_neworder_D_valid");
        ASSERT(std::strcmp(m.get_symbol(), "AAPL") == 0, "fix_neworder_symbol");
        ASSERT(std::strcmp(m.get_side(), "BUY") == 0, "fix_neworder_side_buy");
        ASSERT(m.get_quantity() == 100, "fix_neworder_qty_100");
        // #168 typed accessors
        ASSERT(m.get_int(38) == 100, "fix_get_int_qty");
        ASSERT(std::fabs(m.get_double(44) - 150.25) < 1e-6, "fix_get_double_price");
        ASSERT(m.get_int(99999) == 0, "fix_get_int_missing_zero");
        // #360 parse_new_order — typed acceptor-side decode of 35=D (MILESTONE 360).
        const auto no = fix::FIXSession::parse_new_order(m);
        ASSERT(no.valid && std::strcmp(no.cl_ord_id, "ORD1") == 0
               && std::strcmp(no.symbol, "AAPL") == 0, "fix_parse_nos_id_symbol");
        ASSERT(no.side == '1' && no.qty == 100
               && std::fabs(no.price - 150.25) < 1e-6, "fix_parse_nos_side_qty_px");
        ASSERT(no.ord_type == '2', "fix_parse_nos_limit_ordtype");   // build_new_order sets 40=2
        FIXMessage not_d; not_d.parse("35=8|11=X|37=Y|");
        ASSERT(!fix::FIXSession::parse_new_order(not_d).valid, "fix_parse_nos_non_D_invalid");
        // #177 admin vs application classification
        ASSERT(!m.is_admin(), "fix_neworder_is_application");
        ASSERT(FIXMessage::is_admin_msg_type("0"), "fix_admin_heartbeat");
        ASSERT(FIXMessage::is_admin_msg_type("A"), "fix_admin_logon");
        ASSERT(!FIXMessage::is_admin_msg_type("D"), "fix_app_neworder");
        ASSERT(!FIXMessage::is_admin_msg_type("AE"), "fix_multichar_not_admin");
        s.build_heartbeat(buf, sizeof(buf), nullptr, '|');
        FIXMessage hbeat; hbeat.parse(buf);
        ASSERT(hbeat.is_admin(), "fix_heartbeat_is_admin");

        // #185 OrderStatusRequest (35=H) — reconciliation after a reconnect.
        s.build_order_status_request(buf, sizeof(buf), "ORD1", "AAPL", Side::BUY, '|');
        FIXMessage osr; osr.parse(buf);
        ASSERT(osr.is_valid() && osr.get_msg_type()[0] == 'H', "fix_osr_H_valid");
        ASSERT(std::strcmp(osr.get_field(11), "ORD1") == 0, "fix_osr_clordid");
        ASSERT(std::strcmp(osr.get_symbol(), "AAPL") == 0, "fix_osr_symbol");
        ASSERT(std::strcmp(osr.get_side(), "BUY") == 0, "fix_osr_side");
        ASSERT(!osr.is_admin(), "fix_osr_is_application");

        // #417 parse_order_status_request — typed acceptor-side decode of
        // 35=H; closes the reconciliation round-trip with #185.
        const auto osrq = fix::FIXSession::parse_order_status_request(osr);
        ASSERT(osrq.valid && std::strcmp(osrq.cl_ord_id, "ORD1") == 0, "fix_parse_osr_id");
        ASSERT(std::strcmp(osrq.symbol, "AAPL") == 0 && osrq.side == '1',
               "fix_parse_osr_symbol_side");
        // A bare 35=H without tag 54 decodes with an empty side, still valid.
        FIXMessage osrb; osrb.parse("35=H|11=ORD2|55=MSFT|");
        const auto osrq2 = fix::FIXSession::parse_order_status_request(osrb);
        ASSERT(osrq2.valid && osrq2.side == '\0'
               && std::strcmp(osrq2.symbol, "MSFT") == 0, "fix_parse_osr_missing_side");
        FIXMessage not_h; not_h.parse("35=D|11=X|55=Y|");
        ASSERT(!fix::FIXSession::parse_order_status_request(not_h).valid,
               "fix_parse_osr_non_H_invalid");

        // #193 OrderMassCancelRequest (35=q) — panic button.
        s.build_mass_cancel(buf, sizeof(buf), "MC1", '1', "AAPL", '|');   // po symbolu
        FIXMessage mc1; mc1.parse(buf);
        ASSERT(mc1.is_valid() && mc1.get_msg_type()[0] == 'q', "fix_masscancel_q_valid");
        ASSERT(std::strcmp(mc1.get_field(530), "1") == 0, "fix_masscancel_by_symbol");
        ASSERT(std::strcmp(mc1.get_symbol(), "AAPL") == 0, "fix_masscancel_symbol");
        s.build_mass_cancel(buf, sizeof(buf), "MC2", '7', nullptr, '|'); // all
        FIXMessage mc2; mc2.parse(buf);
        ASSERT(std::strcmp(mc2.get_field(530), "7") == 0, "fix_masscancel_all");

        // #433 parse_mass_cancel — the panic button decoded acceptor-side;
        // a misread scope is a desk left exposed or a book wiped by mistake.
        const auto mcq1 = fix::FIXSession::parse_mass_cancel(mc1);
        ASSERT(mcq1.valid && std::strcmp(mcq1.cl_ord_id, "MC1") == 0, "fix_parse_mc_id");
        ASSERT(mcq1.request_type == '1' && std::strcmp(mcq1.symbol, "AAPL") == 0,
               "fix_parse_mc_by_symbol_scope");
        const auto mcq2 = fix::FIXSession::parse_mass_cancel(mc2);
        // The ALL scope carries no symbol — the empty field is the contract.
        ASSERT(mcq2.valid && mcq2.request_type == '7' && mcq2.symbol[0] == '\0',
               "fix_parse_mc_all_scope_no_symbol");
        FIXMessage not_q; not_q.parse("35=F|11=X|41=Y|");
        ASSERT(!fix::FIXSession::parse_mass_cancel(not_q).valid, "fix_parse_mc_non_q_invalid");

        // #201 OrderMassCancelReport (35=r) — the exchange's response.
        s.build_mass_cancel_report(buf, sizeof(buf), "MC2", '7', 5, '|');
        FIXMessage mcr; mcr.parse(buf);
        ASSERT(mcr.is_valid() && mcr.get_msg_type()[0] == 'r', "fix_mcr_r_valid");
        ASSERT(std::strcmp(mcr.get_field(531), "7") == 0, "fix_mcr_response_all");
        ASSERT(mcr.get_int(533) == 5, "fix_mcr_affected_count");

        // #441 parse_mass_cancel_report — the panic button's ACK, typed;
        // closes the mass-cancel loop symmetrically with #433.
        const auto mcrr = fix::FIXSession::parse_mass_cancel_report(mcr);
        ASSERT(mcrr.valid && std::strcmp(mcrr.cl_ord_id, "MC2") == 0, "fix_parse_mcr_id");
        ASSERT(mcrr.response == '7' && mcrr.affected == 5, "fix_parse_mcr_scope_affected");
        // A REJECTED report with zero affected: 0 is a REAL answer (nothing
        // died and the desk still carries everything), -1 is absence.
        s.build_mass_cancel_report(buf, sizeof(buf), "MC3", '0', 0, '|');
        FIXMessage mcr0; mcr0.parse(buf);
        const auto mcrr0 = fix::FIXSession::parse_mass_cancel_report(mcr0);
        ASSERT(mcrr0.valid && mcrr0.response == '0' && mcrr0.affected == 0,
               "fix_parse_mcr_rejected_zero_real");
        FIXMessage bare_r; bare_r.parse("35=r|11=MC4|");
        ASSERT(fix::FIXSession::parse_mass_cancel_report(bare_r).valid
               && fix::FIXSession::parse_mass_cancel_report(bare_r).affected == -1,
               "fix_parse_mcr_absent_minus1");
        FIXMessage not_r; not_r.parse("35=q|11=X|");
        ASSERT(!fix::FIXSession::parse_mass_cancel_report(not_r).valid,
               "fix_parse_mcr_non_r_invalid");

        // #209 MarketDataRequest (35=V) — subskrypcja danych rynkowych.
        s.build_market_data_request(buf, sizeof(buf), "MDR1", '1', 1, "AAPL", '|');
        FIXMessage mdr; mdr.parse(buf);
        ASSERT(mdr.is_valid() && mdr.get_msg_type()[0] == 'V', "fix_mdr_V_valid");
        ASSERT(std::strcmp(mdr.get_field(262), "MDR1") == 0, "fix_mdr_reqid");
        ASSERT(std::strcmp(mdr.get_field(263), "1") == 0, "fix_mdr_subtype");
        ASSERT(mdr.get_int(264) == 1 && std::strcmp(mdr.get_symbol(), "AAPL") == 0, "fix_mdr_depth_symbol");

        // #457 parse_market_data_request — typed acceptor-side decode of 35=V.
        const auto mdrq = fix::FIXSession::parse_market_data_request(mdr);
        ASSERT(mdrq.valid && std::strcmp(mdrq.md_req_id, "MDR1") == 0, "fix_parse_mdr_id");
        ASSERT(mdrq.sub_type == '1' && mdrq.depth == 1
               && std::strcmp(mdrq.symbol, "AAPL") == 0, "fix_parse_mdr_subtype_depth_symbol");
        // A full-book request (depth 0) is distinct from an absent tag (-1).
        s.build_market_data_request(buf, sizeof(buf), "MDR2", '0', 0, "MSFT", '|');
        FIXMessage mdr0; mdr0.parse(buf);
        const auto mdrq0 = fix::FIXSession::parse_market_data_request(mdr0);
        ASSERT(mdrq0.valid && mdrq0.depth == 0 && mdrq0.sub_type == '0', "fix_parse_mdr_full_book_zero");
        FIXMessage mdr_bare; mdr_bare.parse("35=V|262=MDR3|55=IBM|");
        ASSERT(fix::FIXSession::parse_market_data_request(mdr_bare).depth == -1,
               "fix_parse_mdr_absent_depth_minus1");
        FIXMessage not_v; not_v.parse("35=W|262=X|");
        ASSERT(!fix::FIXSession::parse_market_data_request(not_v).valid, "fix_parse_mdr_non_V_invalid");

        // #217 MarketDataSnapshotFullRefresh (35=W) — response to 35=V.
        s.build_md_snapshot(buf, sizeof(buf), "MDR1", "AAPL", 99.98, 100, 100.02, 200, '|');
        FIXMessage mdw; mdw.parse(buf);
        ASSERT(mdw.is_valid() && mdw.get_msg_type()[0] == 'W', "fix_mdw_W_valid");
        ASSERT(std::strcmp(mdw.get_field(262), "MDR1") == 0
               && std::strcmp(mdw.get_symbol(), "AAPL") == 0, "fix_mdw_reqid_symbol");
        ASSERT(mdw.get_int(268) == 2, "fix_mdw_two_entries");
        ASSERT(std::strcmp(mdw.get_field(269), "0") == 0, "fix_mdw_first_is_bid"); // pierwszy wpis = bid
        ASSERT(std::fabs(mdw.get_double(270) - 99.98) < 1e-6, "fix_mdw_bid_px");

        // #225 MarketDataIncrementalRefresh (35=X) — incremental bid change.
        s.build_md_incremental(buf, sizeof(buf), "MDR1", '1', '0', "AAPL", 100.05, 500, '|');
        FIXMessage mdx; mdx.parse(buf);
        ASSERT(mdx.is_valid() && mdx.get_msg_type()[0] == 'X', "fix_mdx_X_valid");
        ASSERT(std::strcmp(mdx.get_field(279), "1") == 0, "fix_mdx_update_change"); // change
        ASSERT(std::strcmp(mdx.get_field(269), "0") == 0, "fix_mdx_bid_entry");     // bid
        ASSERT(std::fabs(mdx.get_double(270) - 100.05) < 1e-6 && mdx.get_int(271) == 500,
               "fix_mdx_px_size");

        // #233 MarketDataRequestReject (35=Y) — subscription rejection.
        s.build_md_request_reject(buf, sizeof(buf), "MDR1", '0', '|');  // unknown symbol
        FIXMessage mdy; mdy.parse(buf);
        ASSERT(mdy.is_valid() && mdy.get_msg_type()[0] == 'Y', "fix_mdy_Y_valid");
        ASSERT(std::strcmp(mdy.get_field(262), "MDR1") == 0, "fix_mdy_reqid_echo");
        ASSERT(std::strcmp(mdy.get_field(281), "0") == 0, "fix_mdy_reject_reason");

        // #465 parse_md_request_reject — typed CLIENT-side decode of 35=Y;
        // closes the V->Y handshake with parse_market_data_request (#457).
        const auto mdyr = fix::FIXSession::parse_md_request_reject(mdy);
        ASSERT(mdyr.valid && std::strcmp(mdyr.md_req_id, "MDR1") == 0, "fix_parse_mdy_id_echo");
        ASSERT(mdyr.reason == '0', "fix_parse_mdy_reason_unknown_symbol");
        // A different reason code decodes verbatim (unsupported depth).
        s.build_md_request_reject(buf, sizeof(buf), "MDR2", '4', '|');
        FIXMessage mdy4; mdy4.parse(buf);
        const auto mdyr4 = fix::FIXSession::parse_md_request_reject(mdy4);
        ASSERT(mdyr4.valid && mdyr4.reason == '4' && std::strcmp(mdyr4.md_req_id, "MDR2") == 0,
               "fix_parse_mdy_unsupported_depth");
        FIXMessage not_y; not_y.parse("35=V|262=X|");
        ASSERT(!fix::FIXSession::parse_md_request_reject(not_y).valid, "fix_parse_mdy_non_Y_invalid");

        // #241 parse_exec_report — ekstrakcja pol 35=8 do struktury.
        s.build_exec_report(buf, sizeof(buf), "ORD1", "EX1", "E1", '2', '2',  // Fill/Fill
                            "AAPL", Side::BUY, 100, 150.25, 100, 0, '|');
        FIXMessage erm; erm.parse(buf);
        const fix::FIXSession::ExecReport exr = fix::FIXSession::parse_exec_report(erm);
        ASSERT(exr.valid && exr.exec_type == '2' && exr.ord_status == '2', "fix_execrep_type_status");
        ASSERT(exr.last_qty == 100 && std::fabs(exr.last_px - 150.25) < 1e-6, "fix_execrep_last");
        ASSERT(exr.cum_qty == 100 && exr.leaves_qty == 0, "fix_execrep_cum_leaves");
        // a non-8 message -> invalid
        s.build_new_order(buf, sizeof(buf), "ORDX", "AAPL", Side::BUY, 50, 10.0, '|');
        FIXMessage notexec; notexec.parse(buf);
        ASSERT(!fix::FIXSession::parse_exec_report(notexec).valid, "fix_execrep_non8_invalid");

        // #249 Quote (35=S) — two-sided market maker quote.
        s.build_quote(buf, sizeof(buf), "Q1", "AAPL", 99.98, 100.02, 500, 300, '|');
        FIXMessage qm; qm.parse(buf);
        ASSERT(qm.is_valid() && qm.get_msg_type()[0] == 'S', "fix_quote_S_valid");
        ASSERT(std::strcmp(qm.get_field(117), "Q1") == 0
               && std::strcmp(qm.get_symbol(), "AAPL") == 0, "fix_quote_id_symbol");
        ASSERT(std::fabs(qm.get_double(132) - 99.98) < 1e-6
               && std::fabs(qm.get_double(133) - 100.02) < 1e-6, "fix_quote_prices");
        ASSERT(qm.get_int(134) == 500 && qm.get_int(135) == 300, "fix_quote_sizes");

        // #473 parse_quote — typed CLIENT-side decode of 35=S; closes the
        // round-trip with build_quote (#249).
        const auto qd = fix::FIXSession::parse_quote(qm);
        ASSERT(qd.valid && std::strcmp(qd.quote_id, "Q1") == 0
               && std::strcmp(qd.symbol, "AAPL") == 0, "fix_parse_quote_id_symbol");
        ASSERT(std::fabs(qd.bid_px - 99.98) < 1e-6 && std::fabs(qd.offer_px - 100.02) < 1e-6,
               "fix_parse_quote_prices");
        ASSERT(qd.bid_size == 500 && qd.offer_size == 300, "fix_parse_quote_sizes");
        // The decoded quote is a usable market: offer above bid.
        ASSERT(qd.offer_px > qd.bid_px, "fix_parse_quote_two_sided");
        FIXMessage not_s; not_s.parse("35=8|117=X|");
        ASSERT(!fix::FIXSession::parse_quote(not_s).valid, "fix_parse_quote_non_S_invalid");

        // #256 QuoteRequest (35=R) — RFQ request side.
        s.build_quote_request(buf, sizeof(buf), "QR1", "AAPL", '|');
        FIXMessage qrq; qrq.parse(buf);
        ASSERT(qrq.is_valid() && qrq.get_msg_type()[0] == 'R', "fix_qreq_R_valid");
        ASSERT(std::strcmp(qrq.get_field(131), "QR1") == 0
               && std::strcmp(qrq.get_symbol(), "AAPL") == 0, "fix_qreq_id_symbol");

        // #481 parse_quote_request — typed decode of 35=R; the RFQ request
        // side, closing the R->S round-trip with parse_quote (#473).
        const auto qreqd = fix::FIXSession::parse_quote_request(qrq);
        ASSERT(qreqd.valid && std::strcmp(qreqd.quote_req_id, "QR1") == 0
               && std::strcmp(qreqd.symbol, "AAPL") == 0, "fix_parse_qreq_id_symbol");
        FIXMessage not_r_qr; not_r_qr.parse("35=S|131=X|");
        ASSERT(!fix::FIXSession::parse_quote_request(not_r_qr).valid, "fix_parse_qreq_non_R_invalid");

        // #263 repeating-group readers on a MarketDataSnapshot (35=W): bid + ask.
        s.build_md_snapshot(buf, sizeof(buf), "MDR1", "AAPL", 99.98, 100, 100.02, 200, '|');
        FIXMessage rgm; rgm.parse(buf);
        ASSERT(rgm.count_field(269) == 2, "fix_rg_count_two");                 // two MD entries
        ASSERT(std::strcmp(rgm.get_field_nth(269, 0), "0") == 0
               && std::strcmp(rgm.get_field_nth(269, 1), "1") == 0, "fix_rg_entry_types"); // bid, ask
        ASSERT(std::fabs(rgm.get_double_nth(270, 0) - 99.98) < 1e-6
               && std::fabs(rgm.get_double_nth(270, 1) - 100.02) < 1e-6, "fix_rg_prices");
        ASSERT(rgm.get_int_nth(271, 0) == 100 && rgm.get_int_nth(271, 1) == 200, "fix_rg_sizes");
        ASSERT(rgm.get_field_nth(269, 5) == nullptr, "fix_rg_out_of_range");
        ASSERT(rgm.count_field(99999) == 0, "fix_rg_count_absent");

        // #271 QuoteCancel (35=Z) — MM pulls a quote.
        s.build_quote_cancel(buf, sizeof(buf), "Q1", '1', "AAPL", '|');
        FIXMessage qcm; qcm.parse(buf);
        ASSERT(qcm.is_valid() && qcm.get_msg_type()[0] == 'Z', "fix_qcxl_Z_valid");
        ASSERT(std::strcmp(qcm.get_field(117), "Q1") == 0
               && std::strcmp(qcm.get_field(298), "1") == 0, "fix_qcxl_id_type");
        ASSERT(std::strcmp(qcm.get_symbol(), "AAPL") == 0, "fix_qcxl_symbol");

        // #489 parse_quote_cancel — typed decode of 35=Z; completes the
        // typed quote lifecycle R (#481) -> S (#473) -> Z here.
        const auto qcd = fix::FIXSession::parse_quote_cancel(qcm);
        ASSERT(qcd.valid && std::strcmp(qcd.quote_id, "Q1") == 0, "fix_parse_qcxl_id");
        ASSERT(qcd.cancel_type == '1' && std::strcmp(qcd.symbol, "AAPL") == 0,
               "fix_parse_qcxl_one_symbol_scope");
        // Cancel-all (type '4') carries no symbol -> empty field is the contract.
        s.build_quote_cancel(buf, sizeof(buf), "Q2", '4', "", '|');
        FIXMessage qcm4; qcm4.parse(buf);
        const auto qcd4 = fix::FIXSession::parse_quote_cancel(qcm4);
        ASSERT(qcd4.valid && qcd4.cancel_type == '4' && qcd4.symbol[0] == '\0',
               "fix_parse_qcxl_cancel_all_no_symbol");
        FIXMessage not_z; not_z.parse("35=S|117=X|");
        ASSERT(!fix::FIXSession::parse_quote_cancel(not_z).valid, "fix_parse_qcxl_non_Z_invalid");

        // #279 MassQuote (35=i) — 2-symbol quote set read via repeating-group accessors.
        s.build_mass_quote(buf, sizeof(buf), "MQ1", "AAPL", 99.98, 100.02,
                           "MSFT", 200.00, 200.10, '|');
        FIXMessage mq; mq.parse(buf);
        ASSERT(mq.is_valid() && mq.get_msg_type()[0] == 'i', "fix_massq_i_valid");
        ASSERT(mq.get_int(295) == 2 && mq.count_field(55) == 2, "fix_massq_two_entries");
        ASSERT(std::strcmp(mq.get_field_nth(55, 0), "AAPL") == 0
               && std::strcmp(mq.get_field_nth(55, 1), "MSFT") == 0, "fix_massq_symbols");
        ASSERT(std::fabs(mq.get_double_nth(132, 0) - 99.98) < 1e-6
               && std::fabs(mq.get_double_nth(132, 1) - 200.00) < 1e-6, "fix_massq_bids");
        ASSERT(std::fabs(mq.get_double_nth(133, 1) - 200.10) < 1e-6, "fix_massq_ask2");

        // #287 TradeCaptureReport (35=AE) — post-trade record (multi-char msg type).
        s.build_trade_capture_report(buf, sizeof(buf), "TR1", "AAPL", Side::BUY,
                                     100, 150.25, "20260622", '|');
        FIXMessage tcr; tcr.parse(buf);
        ASSERT(tcr.is_valid() && std::strcmp(tcr.get_msg_type(), "AE") == 0, "fix_tcr_AE_valid");
        ASSERT(std::strcmp(tcr.get_field(571), "TR1") == 0
               && std::strcmp(tcr.get_symbol(), "AAPL") == 0, "fix_tcr_id_symbol");
        ASSERT(tcr.get_int(32) == 100 && std::fabs(tcr.get_double(31) - 150.25) < 1e-6,
               "fix_tcr_qty_px");
        ASSERT(std::strcmp(tcr.get_field(75), "20260622") == 0, "fix_tcr_trade_date");
        ASSERT(!tcr.is_admin(), "fix_tcr_application");   // AE is multi-char -> application

        // #336 parse_trade_capture_report — typed round-trip for 35=AE.
        const auto tcrd = fix::FIXSession::parse_trade_capture_report(tcr);
        ASSERT(tcrd.valid && std::strcmp(tcrd.trade_report_id, "TR1") == 0
               && std::strcmp(tcrd.symbol, "AAPL") == 0, "fix_tcr_parsed_id_symbol");
        ASSERT(tcrd.side == '1' && tcrd.last_qty == 100
               && std::fabs(tcrd.last_px - 150.25) < 1e-6, "fix_tcr_parsed_side_qty_px");
        ASSERT(std::strcmp(tcrd.trade_date, "20260622") == 0, "fix_tcr_parsed_date");
        FIXMessage not_ae; not_ae.parse("35=8|11=X|37=Y|");
        ASSERT(!fix::FIXSession::parse_trade_capture_report(not_ae).valid, "fix_tcr_non_AE_invalid");

        // #295 BusinessMessageReject (35=j) — application-level rejection.
        s.build_business_reject(buf, sizeof(buf), 42, "D", 2, "Unknown symbol", '|');
        FIXMessage bmr; bmr.parse(buf);
        ASSERT(bmr.is_valid() && bmr.get_msg_type()[0] == 'j', "fix_bizrej_j_valid");
        ASSERT(bmr.get_int(45) == 42 && std::strcmp(bmr.get_field(372), "D") == 0,
               "fix_bizrej_ref");
        ASSERT(bmr.get_int(380) == 2 && std::strcmp(bmr.get_field(58), "Unknown symbol") == 0,
               "fix_bizrej_reason_text");
        ASSERT(!bmr.is_admin(), "fix_bizrej_application");  // business reject is application

        // #352 TradingSessionStatusRequest (35=g) — client asks for the session phase.
        s.build_trading_session_status_request(buf, sizeof(buf), "TSR1", "REG", '1', '|');
        FIXMessage tsr; tsr.parse(buf);
        ASSERT(tsr.is_valid() && tsr.get_msg_type()[0] == 'g', "fix_tsr_g_valid");
        ASSERT(std::strcmp(tsr.get_field(335), "TSR1") == 0
               && std::strcmp(tsr.get_field(336), "REG") == 0, "fix_tsr_id_session");
        ASSERT(std::strcmp(tsr.get_field(263), "1") == 0, "fix_tsr_subtype");
        ASSERT(!tsr.is_admin(), "fix_tsr_application");

        // #505 parse_trading_session_status_request — typed decode of 35=g;
        // closes the g->h pair with parse_trading_session_status (#497).
        const auto tsrq = fix::FIXSession::parse_trading_session_status_request(tsr);
        ASSERT(tsrq.valid && std::strcmp(tsrq.req_id, "TSR1") == 0
               && std::strcmp(tsrq.session_id, "REG") == 0, "fix_parse_tsr_id_session");
        ASSERT(tsrq.sub_type == '1', "fix_parse_tsr_subtype_updates");
        // A snapshot-only request (sub_type '0') decodes verbatim.
        s.build_trading_session_status_request(buf, sizeof(buf), "TSR2", "AH", '0', '|');
        FIXMessage tsr0; tsr0.parse(buf);
        const auto tsrq0 = fix::FIXSession::parse_trading_session_status_request(tsr0);
        ASSERT(tsrq0.valid && tsrq0.sub_type == '0'
               && std::strcmp(tsrq0.session_id, "AH") == 0, "fix_parse_tsr_snapshot");
        FIXMessage not_g_tsr; not_g_tsr.parse("35=h|336=REG|");
        ASSERT(!fix::FIXSession::parse_trading_session_status_request(not_g_tsr).valid,
               "fix_parse_tsr_non_g_invalid");

        // #303 TradingSessionStatus (35=h) — venue session phase (answers 35=g above).
        s.build_trading_session_status(buf, sizeof(buf), "REG", 2, '|');  // 2 = Open
        FIXMessage tss; tss.parse(buf);
        ASSERT(tss.is_valid() && tss.get_msg_type()[0] == 'h', "fix_tss_h_valid");
        ASSERT(std::strcmp(tss.get_field(336), "REG") == 0 && tss.get_int(340) == 2,
               "fix_tss_id_status");
        ASSERT(!tss.is_admin(), "fix_tss_application");

        // #497 parse_trading_session_status — typed decode of 35=h; closes
        // the g->h handshake with build_trading_session_status (#303).
        const auto tssd = fix::FIXSession::parse_trading_session_status(tss);
        ASSERT(tssd.valid && std::strcmp(tssd.session_id, "REG") == 0, "fix_parse_tss_id");
        ASSERT(tssd.status == 2 && tssd.is_open() && !tssd.is_halted(), "fix_parse_tss_open");
        // A Halted (1) session decodes with is_halted() true, is_open() false.
        s.build_trading_session_status(buf, sizeof(buf), "REG", 1, '|');
        FIXMessage tsh; tsh.parse(buf);
        const auto tshd = fix::FIXSession::parse_trading_session_status(tsh);
        ASSERT(tshd.valid && tshd.is_halted() && !tshd.is_open(), "fix_parse_tss_halted");
        // An absent status tag -> -1 sentinel.
        FIXMessage tsb; tsb.parse("35=h|336=REG|");
        ASSERT(fix::FIXSession::parse_trading_session_status(tsb).status == -1,
               "fix_parse_tss_absent_status");
        FIXMessage not_h_tss; not_h_tss.parse("35=g|335=X|");
        ASSERT(!fix::FIXSession::parse_trading_session_status(not_h_tss).valid,
               "fix_parse_tss_non_h_invalid");

        // #311 OrderMassStatusRequest (35=AF) — bulk order-status query.
        s.build_mass_status_request(buf, sizeof(buf), "MS1", 7, "AAPL", '|');  // 7 = by symbol
        FIXMessage msr; msr.parse(buf);
        ASSERT(msr.is_valid() && std::strcmp(msr.get_msg_type(), "AF") == 0, "fix_massstat_AF_valid");
        ASSERT(std::strcmp(msr.get_field(584), "MS1") == 0 && msr.get_int(585) == 7,
               "fix_massstat_id_type");
        ASSERT(std::strcmp(msr.get_symbol(), "AAPL") == 0, "fix_massstat_symbol");
        ASSERT(!msr.is_admin(), "fix_massstat_application");

        // #521 parse_mass_status_request — typed acceptor-side decode of 35=AF;
        // closes the round-trip with build_mass_status_request (#311).
        const auto msrq = fix::FIXSession::parse_mass_status_request(msr);
        ASSERT(msrq.valid && std::strcmp(msrq.req_id, "MS1") == 0 && msrq.req_type == 7,
               "fix_parse_massstat_id_type");
        ASSERT(std::strcmp(msrq.symbol, "AAPL") == 0 && !msrq.is_all_orders(),
               "fix_parse_massstat_symbol_scoped");
        // An AllOrders (type 1) request needs no symbol.
        s.build_mass_status_request(buf, sizeof(buf), "MS2", 1, "", '|');
        FIXMessage msr2; msr2.parse(buf);
        const auto msrq2 = fix::FIXSession::parse_mass_status_request(msr2);
        ASSERT(msrq2.valid && msrq2.is_all_orders() && msrq2.symbol[0] == '\0',
               "fix_parse_massstat_all_orders");
        // Absent 585 -> req_type falls back to the -1 sentinel.
        FIXMessage msr_bare; msr_bare.parse("35=AF|584=MS3|");
        ASSERT(fix::FIXSession::parse_mass_status_request(msr_bare).req_type == -1,
               "fix_parse_massstat_absent_type_minus1");
        FIXMessage not_af; not_af.parse("35=8|11=X|37=Y|");
        ASSERT(!fix::FIXSession::parse_mass_status_request(not_af).valid, "fix_parse_massstat_nonAF_invalid");

        // #344 IOI (35=6) — Indication of Interest, non-binding advertised liquidity.
        s.build_ioi(buf, sizeof(buf), "IOI1", 'N', "AAPL", Side::SELL, 500, 151.50, '|');
        FIXMessage ioi; ioi.parse(buf);
        ASSERT(ioi.is_valid() && ioi.get_msg_type()[0] == '6', "fix_ioi_6_valid");
        ASSERT(std::strcmp(ioi.get_field(23), "IOI1") == 0
               && std::strcmp(ioi.get_field(28), "N") == 0, "fix_ioi_id_transtype");
        ASSERT(std::strcmp(ioi.get_symbol(), "AAPL") == 0 && ioi.get_int(27) == 500,
               "fix_ioi_symbol_qty");
        ASSERT(std::fabs(ioi.get_double(44) - 151.50) < 1e-6, "fix_ioi_price");
        ASSERT(!ioi.is_admin(), "fix_ioi_application");   // 6 is not in the admin set

        // #344 parse_ioi — typed round-trip for 35=6.
        const auto ioid = fix::FIXSession::parse_ioi(ioi);
        ASSERT(ioid.valid && ioid.trans_type == 'N' && ioid.side == '2', "fix_ioi_parsed_transtype_side");
        ASSERT(ioid.qty == 500 && std::fabs(ioid.price - 151.50) < 1e-6, "fix_ioi_parsed_qty_price");
        FIXMessage not_ioi; not_ioi.parse("35=8|11=X|37=Y|");
        ASSERT(!fix::FIXSession::parse_ioi(not_ioi).valid, "fix_ioi_non_6_invalid");

        s.build_cancel_replace(buf, sizeof(buf), "ORD2", "ORD1", "AAPL", Side::SELL, 80, 151.00, '|');
        FIXMessage g; g.parse(buf);
        ASSERT(g.is_valid() && g.get_msg_type()[0] == 'G', "fix_replace_G_valid");
        ASSERT(std::strcmp(g.get_field(41), "ORD1") == 0, "fix_replace_origclordid");

        // #377 parse_replace_request — typed acceptor-side decode of 35=G;
        // completes the typed D (#360) / F (#368) / G order-entry triple.
        const auto grq = fix::FIXSession::parse_replace_request(g);
        ASSERT(grq.valid && std::strcmp(grq.cl_ord_id, "ORD2") == 0
               && std::strcmp(grq.orig_cl_ord_id, "ORD1") == 0, "fix_parse_rpl_ids");
        ASSERT(std::strcmp(grq.symbol, "AAPL") == 0 && grq.side == '2', "fix_parse_rpl_symbol_side");
        // 38/44 carry the NEW quantity/price of the amended order.
        ASSERT(grq.qty == 80 && std::fabs(grq.price - 151.00) < 1e-6, "fix_parse_rpl_new_qty_price");
        ASSERT(grq.ord_type == '2', "fix_parse_rpl_ordtype_limit");
        FIXMessage not_g; not_g.parse("35=F|11=X|41=Y|");
        ASSERT(!fix::FIXSession::parse_replace_request(not_g).valid, "fix_parse_rpl_non_G_invalid");

        // #368 parse_cancel_request — typed acceptor-side decode of 35=F.
        s.build_cancel_order(buf, sizeof(buf), "CXL1", "ORD1", "AAPL", Side::BUY, 100, '|');
        FIXMessage fm; fm.parse(buf);
        ASSERT(fm.is_valid() && fm.get_msg_type()[0] == 'F', "fix_cancel_F_valid");
        const auto cxr = fix::FIXSession::parse_cancel_request(fm);
        ASSERT(cxr.valid && std::strcmp(cxr.cl_ord_id, "CXL1") == 0
               && std::strcmp(cxr.orig_cl_ord_id, "ORD1") == 0, "fix_parse_cxl_ids");
        ASSERT(std::strcmp(cxr.symbol, "AAPL") == 0 && cxr.side == '1' && cxr.qty == 100,
               "fix_parse_cxl_symbol_side_qty");
        FIXMessage not_f; not_f.parse("35=D|11=X|55=Y|");
        ASSERT(!fix::FIXSession::parse_cancel_request(not_f).valid, "fix_parse_cxl_non_F_invalid");

        // #150 process_inbound — dispatcher Logon/SeqReset/Logout.
        fix::FIXSession ps; ps.set_comp_ids("ME", "EX");
        char pb[256];
        ps.build_logon(pb, sizeof(pb), 45, '|');         // 34=1, 108=45
        FIXMessage lm; lm.parse(pb);
        ps.process_inbound(lm, 1000);
        ASSERT(ps.is_logged_in() && ps.heartbeat_interval_sec() == 45, "fix_pi_logon_logged_in");
        ASSERT(ps.expected_inbound_seq() == 2, "fix_pi_seq_advanced");
        ps.build_sequence_reset(pb, sizeof(pb), 100, true, '|');
        FIXMessage sr; sr.parse(pb);
        ps.process_inbound(sr, 2000);
        ASSERT(ps.expected_inbound_seq() == 100, "fix_pi_seqreset_applied");
        ps.build_logout(pb, sizeof(pb), nullptr, '|');
        FIXMessage lo; lo.parse(pb);
        ps.process_inbound(lo, 3000);
        ASSERT(ps.state() == fix::SessionState::LOGOUT, "fix_pi_logout_state");

        // #158 inbound TestRequest -> remember 112, respond with a Heartbeat.
        fix::FIXSession ts; ts.set_comp_ids("ME", "EX");
        char tb[256];
        ts.build_test_request(tb, sizeof(tb), "TR123", '|');
        FIXMessage trm; trm.parse(tb);
        ts.process_inbound(trm, 4000);
        ASSERT(std::strcmp(ts.pending_test_req_id(), "TR123") == 0, "fix_pi_testreq_captured");
        ts.build_heartbeat(tb, sizeof(tb), ts.pending_test_req_id(), '|');
        FIXMessage hbm; hbm.parse(tb);
        ASSERT(std::strcmp(hbm.get_field(112), "TR123") == 0, "fix_pi_hb_echoes_112");
        ts.clear_pending_test_req();
        ASSERT(ts.pending_test_req_id()[0] == '\0', "fix_pi_testreq_cleared");

        // #116 walidacja NewOrderSingle (acceptor-side).
        s.build_new_order(buf, sizeof(buf), "X", "AAPL", Side::BUY, 100, 150.0, '|');
        FIXMessage ok; ok.parse(buf);
        ASSERT(ok.validate_new_order() == nullptr, "fixval_valid_ok");
        FIXMessage hb; hb.parse("35=0|49=A|56=B|34=1|");
        ASSERT(hb.validate_new_order() != nullptr, "fixval_not_D");
        FIXMessage bad; bad.parse("35=D|11=X|55=AAPL|54=9|38=100|40=2|44=10|");
        ASSERT(std::strcmp(bad.validate_new_order(), "invalid Side (54)") == 0, "fixval_bad_side");
        FIXMessage q0; q0.parse("35=D|11=X|55=AAPL|54=1|38=0|40=2|44=10|");
        ASSERT(q0.validate_new_order() != nullptr, "fixval_qty_zero");
        FIXMessage np; np.parse("35=D|11=X|55=AAPL|54=1|38=100|40=2|44=0|");
        ASSERT(np.validate_new_order() != nullptr, "fixval_limit_no_price");

        // #101 ExecutionReport (35=8) — partial fill: 40 z 100, leaves 60.
        s.build_exec_report(buf, sizeof(buf), "ORD1", "EXG-1", "E-1", '1', '1',
                            "AAPL", Side::BUY, 40, 150.25, 40, 60, '|');
        FIXMessage er; er.parse(buf);
        ASSERT(er.is_valid() && er.get_msg_type()[0] == '8', "fix_exec_report_valid");
        ASSERT(er.get_field(150)[0] == '1', "fix_exec_type_partial");   // ExecType=Partial
        ASSERT(std::atoi(er.get_field(32)) == 40, "fix_exec_last_qty");  // LastQty
        ASSERT(std::atoi(er.get_field(151)) == 60, "fix_exec_leaves_qty"); // LeavesQty

        // #126 Session-level Reject (35=3) — e.g. after a negative validate_new_order.
        s.build_reject(buf, sizeof(buf), 42, "D", 1, "Required tag missing", '|');
        FIXMessage rj; rj.parse(buf);
        ASSERT(rj.is_valid() && rj.get_msg_type()[0] == '3', "fix_reject_valid");
        ASSERT(std::atoi(rj.get_field(45)) == 42, "fix_reject_refseqnum");
        ASSERT(std::atoi(rj.get_field(373)) == 1, "fix_reject_reason_code");
        ASSERT(std::strcmp(rj.get_field(372), "D") == 0, "fix_reject_refmsgtype");

        // #133 Business Message Reject (35=j) — e.g. an unknown symbol.
        s.build_business_reject(buf, sizeof(buf), "D", "ORD9", 2, "Unknown symbol", '|');
        FIXMessage bj; bj.parse(buf);
        ASSERT(bj.is_valid() && bj.get_msg_type()[0] == 'j', "fix_busreject_valid");
        ASSERT(std::strcmp(bj.get_field(379), "ORD9") == 0, "fix_busreject_refid");
        ASSERT(std::atoi(bj.get_field(380)) == 2, "fix_busreject_reason");

        // #143 OrderCancelReject (35=9) — rejection of cancel/replace (e.g. too late).
        s.build_cancel_reject(buf, sizeof(buf), "CXL1", "ORD1", "EXG1", '2', '2', 0,
                              "Too late to cancel", '|');
        FIXMessage cr; cr.parse(buf);
        ASSERT(cr.is_valid() && cr.get_msg_type()[0] == '9', "fix_cxlreject_valid");
        ASSERT(cr.get_field(434)[0] == '2', "fix_cxlreject_response_to");  // wobec Replace
        ASSERT(std::atoi(cr.get_field(102)) == 0, "fix_cxlreject_reason_too_late");
        ASSERT(std::strcmp(cr.get_field(41), "ORD1") == 0, "fix_cxlreject_origclordid");

        // #385 parse_cancel_reject — typed CLIENT-side decode of 35=9; closes
        // the round-trip with build_cancel_reject (#143). FIX counterpart of
        // the OUCH tracker's cancel-lifecycle handling (#378).
        const auto crj = fix::FIXSession::parse_cancel_reject(cr);
        ASSERT(crj.valid && std::strcmp(crj.cl_ord_id, "CXL1") == 0
               && std::strcmp(crj.orig_cl_ord_id, "ORD1") == 0
               && std::strcmp(crj.order_id, "EXG1") == 0, "fix_parse_cxlrej_ids");
        ASSERT(crj.ord_status == '2' && crj.response_to == '2', "fix_parse_cxlrej_status_respto");
        // Reason 0 (= too late) is PRESENT here, so it must decode as 0, not
        // the -1 absent sentinel.
        ASSERT(crj.reason == 0, "fix_parse_cxlrej_reason_zero_present");
        ASSERT(std::strcmp(crj.text, "Too late to cancel") == 0, "fix_parse_cxlrej_text");
        // A 35=9 without tag 102 -> reason falls back to the -1 sentinel.
        FIXMessage nine_bare; nine_bare.parse("35=9|11=CXL2|41=ORD2|");
        const auto crjb = fix::FIXSession::parse_cancel_reject(nine_bare);
        ASSERT(crjb.valid && crjb.reason == -1, "fix_parse_cxlrej_absent_reason_minus1");
        FIXMessage not_nine; not_nine.parse("35=8|11=X|37=Y|");
        ASSERT(!fix::FIXSession::parse_cancel_reject(not_nine).valid, "fix_parse_cxlrej_non9_invalid");

        // #513 parse_business_reject — typed CLIENT-side decode of 35=j; closes
        // the round-trip with BOTH build_business_reject overloads.
        // The #295 form identifies the offending message by 45=RefSeqNum.
        s.build_business_reject(buf, sizeof(buf), 42u, "D", 2, "Unknown symbol", '|');
        FIXMessage bzr; bzr.parse(buf);
        const auto brj = fix::FIXSession::parse_business_reject(bzr);
        ASSERT(brj.valid && brj.ref_seq == 42 && std::strcmp(brj.ref_msg_type, "D") == 0,
               "fix_parse_bizrej_refseq");
        ASSERT(brj.reason == 2 && brj.is_unknown_security()
               && std::strcmp(brj.text, "Unknown symbol") == 0, "fix_parse_bizrej_reason_text");
        ASSERT(brj.ref_id[0] == '\0', "fix_parse_bizrej_refid_absent");   // #295 form carries no 379
        // The #133 form identifies it by 379=BusinessRejectRefID (no 45).
        s.build_business_reject(buf, sizeof(buf), "D", "ORD9", 5, "Missing field", '|');
        FIXMessage bzr2; bzr2.parse(buf);
        const auto brj2 = fix::FIXSession::parse_business_reject(bzr2);
        ASSERT(brj2.valid && std::strcmp(brj2.ref_id, "ORD9") == 0
               && std::strcmp(brj2.ref_msg_type, "D") == 0, "fix_parse_bizrej_refid");
        ASSERT(brj2.ref_seq == 0 && brj2.reason == 5 && !brj2.is_unknown_security(),
               "fix_parse_bizrej_refseq_absent");
        // A 35=j without tag 380 -> reason falls back to the -1 sentinel.
        FIXMessage bzr_bare; bzr_bare.parse("35=j|372=D|379=ORD1|");
        ASSERT(fix::FIXSession::parse_business_reject(bzr_bare).reason == -1,
               "fix_parse_bizrej_absent_reason_minus1");
        FIXMessage bzr_notj; bzr_notj.parse("35=8|11=X|37=Y|");
        ASSERT(!fix::FIXSession::parse_business_reject(bzr_notj).valid, "fix_parse_bizrej_nonj_invalid");

        // #319 QuoteStatusReport (35=AI) — venue ack/reject of a Quote.
        // Full RFQ lifecycle: 35=R (request) -> 35=S (quote) -> 35=AI (status) -> 35=Z (cancel).
        s.build_quote_status_report(buf, sizeof(buf), "Q1", "AAPL", 0, nullptr, '|'); // Accepted
        FIXMessage qsr; qsr.parse(buf);
        ASSERT(qsr.is_valid() && std::strcmp(qsr.get_msg_type(), "AI") == 0, "fix_qsr_AI_valid");
        ASSERT(std::strcmp(qsr.get_field(117), "Q1") == 0
               && std::strcmp(qsr.get_symbol(), "AAPL") == 0, "fix_qsr_id_symbol");
        ASSERT(qsr.get_int(297) == 0, "fix_qsr_status_accepted");
        {
            const auto qsrd = fix::FIXSession::parse_quote_status(qsr);
            ASSERT(qsrd.valid && qsrd.status == 0, "fix_qsr_parsed_accepted");
        }
        s.build_quote_status_report(buf, sizeof(buf), "Q2", "MSFT", 4, "Price out of band", '|');
        FIXMessage qsrj; qsrj.parse(buf);
        ASSERT(qsrj.is_valid() && qsrj.get_int(297) == 4, "fix_qsr_status_rejected");
        ASSERT(std::strcmp(qsrj.get_field(58), "Price out of band") == 0, "fix_qsr_rejection_text");
        {
            const auto qsrjd = fix::FIXSession::parse_quote_status(qsrj);
            ASSERT(qsrjd.valid && qsrjd.status == 4, "fix_qsr_parsed_rejected");
        }
        // A non-AI message must produce valid=false.
        {
            FIXMessage not_ai; not_ai.parse("35=S|117=X|55=AAPL|132=10.0|133=10.1|");
            ASSERT(!fix::FIXSession::parse_quote_status(not_ai).valid, "fix_qsr_non_AI_invalid");
        }

        // #327 DontKnowTrade (35=Q) — client repudiates an unreconcilable ExecReport.
        {
            s.build_dont_know_trade(buf, sizeof(buf), "ORD9", "EXEC9", 'D', "AAPL", '1', 500,
                                    nullptr, '|');   // D = No matching order
            FIXMessage dk; dk.parse(buf);
            ASSERT(dk.is_valid() && std::strcmp(dk.get_msg_type(), "Q") == 0, "fix_dk_Q_valid");
            ASSERT(std::strcmp(dk.get_field(37), "ORD9") == 0
                   && std::strcmp(dk.get_field(17), "EXEC9") == 0, "fix_dk_ids");
            ASSERT(dk.get_field(127)[0] == 'D' && dk.get_int(38) == 500, "fix_dk_reason_qty");
            ASSERT(dk.get_field(54)[0] == '1' && std::strcmp(dk.get_symbol(), "AAPL") == 0,
                   "fix_dk_side_symbol");
            const auto dkd = fix::FIXSession::parse_dont_know_trade(dk);
            ASSERT(dkd.valid && dkd.dk_reason == 'D', "fix_dk_parsed");

            // with optional 58=Text
            s.build_dont_know_trade(buf, sizeof(buf), "ORD10", "EXEC10", 'C', "MSFT", '2', 100,
                                    "Qty exceeds order", '|');
            FIXMessage dk2; dk2.parse(buf);
            ASSERT(dk2.is_valid() && std::strcmp(dk2.get_field(58), "Qty exceeds order") == 0,
                   "fix_dk_text");

            // a non-Q message must produce valid=false
            FIXMessage not_q; not_q.parse("35=8|11=X|37=Y|17=Z|");
            ASSERT(!fix::FIXSession::parse_dont_know_trade(not_q).valid, "fix_dk_non_Q_invalid");
        }
    }
}


// FIX order state #111 — a client-side state machine from ExecutionReports (35=8).
void test_fix_order_state() {
    SECTION("FIX Order State (#111)");
    fix::FIXSession s; s.set_comp_ids("ME", "EX");
    fix::FIXOrderTracker tr;
    char buf[256];

    tr.on_new("ORD1", 100);
    ASSERT(tr.state("ORD1") == fix::OrdState::NEW, "fixstate_new");

    s.build_exec_report(buf, sizeof(buf), "ORD1", "EXG", "E1", '1', '1',
                        "AAPL", Side::BUY, 40, 150.0, 40, 60, '|');
    FIXMessage m1; m1.parse(buf);
    ASSERT(tr.on_exec_report(m1) == fix::OrdState::PARTIAL, "fixstate_partial");
    ASSERT(tr.cum_qty("ORD1") == 40 && tr.leaves_qty("ORD1") == 60, "fixstate_cum_leaves");

    s.build_exec_report(buf, sizeof(buf), "ORD1", "EXG", "E2", '2', '2',
                        "AAPL", Side::BUY, 60, 150.0, 100, 0, '|');
    FIXMessage m2; m2.parse(buf);
    ASSERT(tr.on_exec_report(m2) == fix::OrdState::FILLED, "fixstate_filled");
    ASSERT(tr.fills() == 1, "fixstate_fill_count");

    s.build_exec_report(buf, sizeof(buf), "GHOST", "EXG", "E3", '2', '2',
                        "AAPL", Side::BUY, 10, 1.0, 10, 0, '|');
    FIXMessage m3; m3.parse(buf);
    ASSERT(tr.on_exec_report(m3) == fix::OrdState::UNKNOWN, "fixstate_unknown_clordid");

    // #393 cancel lifecycle: pending flag + OrderCancelReject application
    // (the tracker half of parse_cancel_reject #385, mirroring OUCH #378).
    tr.on_new("ORD5", 80);
    tr.on_cancel_sent("ORD5");
    ASSERT(tr.is_pending_cancel("ORD5"), "fixstate_pending_cancel_armed");
    // The cancel is refused (too late): the 35=9 names the order via tag 41.
    s.build_cancel_reject(buf, sizeof(buf), "CXL5", "ORD5", "EXG5", '0', '1', 0,
                          "Too late to cancel", '|');
    FIXMessage m9; m9.parse(buf);
    ASSERT(tr.on_cancel_reject(m9) == fix::OrdState::NEW, "fixstate_cxlrej_keeps_state");
    ASSERT(!tr.is_pending_cancel("ORD5"), "fixstate_cxlrej_disarms");
    ASSERT(tr.cancel_rejects() == 1 && tr.rejects() == 0, "fixstate_cxlrej_own_counter");
    // The order still works: it fills after the failed cancel.
    s.build_exec_report(buf, sizeof(buf), "ORD5", "EXG", "E5", '2', '2',
                        "AAPL", Side::BUY, 80, 150.0, 80, 0, '|');
    FIXMessage m5f; m5f.parse(buf);
    ASSERT(tr.on_exec_report(m5f) == fix::OrdState::FILLED, "fixstate_fill_after_cxlrej");
    // A confirmed cancel (35=8 with OrdStatus=4) clears the pending flag.
    tr.on_new("ORD6", 50);
    tr.on_cancel_sent("ORD6");
    ASSERT(tr.is_pending_cancel("ORD6"), "fixstate_ord6_armed");
    s.build_exec_report(buf, sizeof(buf), "ORD6", "EXG", "E6", '4', '4',
                        "AAPL", Side::SELL, 0, 150.0, 0, 0, '|');
    FIXMessage m6; m6.parse(buf);
    ASSERT(tr.on_exec_report(m6) == fix::OrdState::CANCELED, "fixstate_cancel_confirmed");
    ASSERT(!tr.is_pending_cancel("ORD6"), "fixstate_confirm_clears_flag");
    // Desync: a 35=9 whose tag 41 names an unregistered order.
    s.build_cancel_reject(buf, sizeof(buf), "CXLX", "GHOST7", "EXGX", '0', '1', 1,
                          "Unknown order", '|');
    FIXMessage m9x; m9x.parse(buf);
    ASSERT(tr.on_cancel_reject(m9x) == fix::OrdState::UNKNOWN, "fixstate_cxlrej_unknown_desync");
    // on_cancel_sent on a terminal order does not arm the flag.
    tr.on_cancel_sent("ORD6");   // already CANCELED
    ASSERT(!tr.is_pending_cancel("ORD6"), "fixstate_terminal_not_armed");

    // #401 OrdStatus=5 (Replaced): ClOrdID migration — the FIX mirror of the
    // OUCH tracker's token migration (#386). Raw messages: build_exec_report
    // does not emit tag 41.
    tr.on_new("ORD7", 100);
    s.build_exec_report(buf, sizeof(buf), "ORD7", "EXG", "E7", '1', '1',
                        "AAPL", Side::BUY, 30, 150.0, 30, 70, '|');
    FIXMessage m7p; m7p.parse(buf);
    tr.on_exec_report(m7p);                       // PARTIAL: 30 done, 70 leave
    tr.on_cancel_sent("ORD7");                    // an in-flight request to reset
    FIXMessage m7r; m7r.parse("35=8|11=ORD8|41=ORD7|39=5|14=30|151=170|");
    ASSERT(tr.on_exec_report(m7r) == fix::OrdState::PARTIAL, "fixstate_rpl_partial_migrated");
    ASSERT(tr.state("ORD7") == fix::OrdState::UNKNOWN, "fixstate_rpl_old_id_gone");
    ASSERT(tr.cum_qty("ORD8") == 30 && tr.leaves_qty("ORD8") == 170, "fixstate_rpl_chain_qty");
    ASSERT(!tr.is_pending_cancel("ORD8"), "fixstate_rpl_flag_reset");
    ASSERT(tr.replaces() == 1, "fixstate_rpl_counter");
    // An unfilled replace lands NEW; an unknown tag 41 stays a desync.
    tr.on_new("ORD9", 40);
    FIXMessage m9r; m9r.parse("35=8|11=ORDA|41=ORD9|39=5|151=60|");
    ASSERT(tr.on_exec_report(m9r) == fix::OrdState::NEW, "fixstate_rpl_unfilled_new");
    FIXMessage mgr; mgr.parse("35=8|11=ORDB|41=GHOST8|39=5|151=10|");
    ASSERT(tr.on_exec_report(mgr) == fix::OrdState::UNKNOWN, "fixstate_rpl_unknown_prev");
    // The migrated order keeps working: fill it to done under the NEW id.
    s.build_exec_report(buf, sizeof(buf), "ORD8", "EXG", "E8", '2', '2',
                        "AAPL", Side::BUY, 170, 150.0, 200, 0, '|');
    FIXMessage m8f; m8f.parse(buf);
    ASSERT(tr.on_exec_report(m8f) == fix::OrdState::FILLED, "fixstate_rpl_fill_under_new_id");

    // #409 working_orders / working_qty — FIX parity of OUCH #272/#304.
    fix::FIXOrderTracker wtr;
    ASSERT(wtr.working_orders() == 0 && wtr.working_qty() == 0, "fixstate_wq_empty");
    wtr.on_new("W1", 100);                        // NEW, leaves 100
    wtr.on_new("W2", 50);                         // NEW, leaves 50
    ASSERT(wtr.working_orders() == 2 && wtr.working_qty() == 150, "fixstate_wq_two_new");
    // A partial fill keeps the order working with reduced leaves.
    s.build_exec_report(buf, sizeof(buf), "W1", "EXG", "EW1", '1', '1',
                        "AAPL", Side::BUY, 40, 150.0, 40, 60, '|');
    FIXMessage mw1; mw1.parse(buf);
    wtr.on_exec_report(mw1);
    ASSERT(wtr.working_orders() == 2 && wtr.working_qty() == 110, "fixstate_wq_partial");
    // A cancel is terminal — its remainder leaves the working aggregate
    // even though the record keeps its last leaves_qty snapshot.
    s.build_exec_report(buf, sizeof(buf), "W2", "EXG", "EW2", '4', '4',
                        "AAPL", Side::SELL, 0, 150.0, 0, 50, '|');
    FIXMessage mw2; mw2.parse(buf);
    wtr.on_exec_report(mw2);
    ASSERT(wtr.working_orders() == 1 && wtr.working_qty() == 60, "fixstate_wq_cancel_excluded");
    ASSERT(wtr.order_count() == 2, "fixstate_wq_order_count_keeps_terminal");
    // Filling the last one empties the working view.
    s.build_exec_report(buf, sizeof(buf), "W1", "EXG", "EW3", '2', '2',
                        "AAPL", Side::BUY, 60, 150.0, 100, 0, '|');
    FIXMessage mw3; mw3.parse(buf);
    wtr.on_exec_report(mw3);
    ASSERT(wtr.working_orders() == 0 && wtr.working_qty() == 0, "fixstate_wq_all_done");

    // #425 pending_cancel_qty / pending_cancel_fraction — condemned exposure
    // on the FIX side (parity of OUCH #402).
    fix::FIXOrderTracker ptr_;
    ASSERT(ptr_.pending_cancel_qty() == 0 && ptr_.pending_cancel_fraction() == 0.0,
           "fixstate_pcq_empty");
    ptr_.on_new("PC1", 100);
    ptr_.on_new("PC2", 300);
    ptr_.on_cancel_sent("PC2");
    ASSERT(ptr_.pending_cancel_qty() == 300, "fixstate_pcq_armed");
    // 300 condemned of 400 working -> 0.75 of the book is an illusion.
    ASSERT(std::fabs(ptr_.pending_cancel_fraction() - 0.75) < 1e-9, "fixstate_pcq_fraction");
    // A fill racing the cancel shrinks the condemned leaves too.
    s.build_exec_report(buf, sizeof(buf), "PC2", "EXG", "EP1", '1', '1',
                        "AAPL", Side::BUY, 100, 150.0, 100, 200, '|');
    FIXMessage mp1; mp1.parse(buf);
    ptr_.on_exec_report(mp1);
    ASSERT(ptr_.pending_cancel_qty() == 200, "fixstate_pcq_fill_shrinks");
    // A cancel reject disarms — the shares stop being condemned.
    s.build_cancel_reject(buf, sizeof(buf), "CXLP", "PC2", "EXGP", '1', '1', 0,
                          "Too late", '|');
    FIXMessage mp2; mp2.parse(buf);
    ptr_.on_cancel_reject(mp2);
    ASSERT(ptr_.pending_cancel_qty() == 0 && ptr_.pending_cancel_fraction() == 0.0,
           "fixstate_pcq_reject_disarms");

    // #449 reset_session — the FIX tracker's new-day wipe (#442's parity).
    // ptr_ carries live records and a cancel_rejects count from above.
    ASSERT(ptr_.order_count() == 2 && ptr_.cancel_rejects() == 1, "fixstate_rs_populated");
    ptr_.reset_session();
    ASSERT(ptr_.order_count() == 0 && ptr_.working_orders() == 0
           && ptr_.working_qty() == 0, "fixstate_rs_map_wiped");
    ASSERT(ptr_.fills() == 0 && ptr_.cancels() == 0 && ptr_.rejects() == 0
           && ptr_.cancel_rejects() == 0 && ptr_.replaces() == 0,
           "fixstate_rs_counters_zero");
    // Yesterday's ClOrdID is reusable today without a stale record answering.
    ptr_.on_new("PC1", 40);
    ASSERT(ptr_.state("PC1") == fix::OrdState::NEW && ptr_.leaves_qty("PC1") == 40,
           "fixstate_rs_id_reuse_clean");
}

// OUCH order state #89 — a client-side state machine (token → live/partial/filled).
void test_ouch_order_state() {
    SECTION("OUCH Order State (#89)");
    ouch::OUCHOrderTracker t;
    uint8_t buf[64];

    t.on_new("TOK1", 100);
    ASSERT(t.state("TOK1") == ouch::OrderState::NEW, "ouchstate_new");

    int n = OUCHMessage::encode_accepted(buf, "TOK1", 'B', 100, "AAPL", 150.0, 555);
    t.on_response(OUCHMessage::parse_response(buf, n));
    ASSERT(t.state("TOK1") == ouch::OrderState::LIVE, "ouchstate_live");

    n = OUCHMessage::encode_executed(buf, "TOK1", 40, 150.0, 9001);
    t.on_response(OUCHMessage::parse_response(buf, n));
    ASSERT(t.state("TOK1") == ouch::OrderState::PARTIAL, "ouchstate_partial");
    ASSERT(t.remaining("TOK1") == 60 && t.filled("TOK1") == 40, "ouchstate_remaining_60");

    n = OUCHMessage::encode_executed(buf, "TOK1", 60, 150.0, 9002);
    t.on_response(OUCHMessage::parse_response(buf, n));
    ASSERT(t.state("TOK1") == ouch::OrderState::FILLED, "ouchstate_filled");
    ASSERT(t.fills() == 1, "ouchstate_fill_count");

    t.on_new("TOK2", 50);
    // #159 pending-cancel: Accept -> LIVE, on_cancel_sent -> pending, 'C' -> clear.
    n = OUCHMessage::encode_accepted(buf, "TOK2", 'B', 50, "AAPL", 100.0, 777);
    t.on_response(OUCHMessage::parse_response(buf, n));
    t.on_cancel_sent("TOK2");
    ASSERT(t.is_pending_cancel("TOK2"), "ouchstate_pending_cancel");
    n = OUCHMessage::encode_cancelled(buf, "TOK2", 50, 'U');
    t.on_response(OUCHMessage::parse_response(buf, n));
    ASSERT(t.state("TOK2") == ouch::OrderState::CANCELLED, "ouchstate_cancelled");
    ASSERT(!t.is_pending_cancel("TOK2"), "ouchstate_pending_cancel_cleared");

    n = OUCHMessage::encode_accepted(buf, "GHOST", 'B', 10, "X", 1.0, 1);
    ASSERT(t.on_response(OUCHMessage::parse_response(buf, n)) == ouch::OrderState::REJECTED,
           "ouchstate_unknown_rejected");

    // #103 Order Rejected ('J'): encode → decode → tracker REJECTED.
    t.on_new("TOK3", 100);
    n = OUCHMessage::encode_rejected(buf, "TOK3", 'X');
    const OUCHResponse rr = OUCHMessage::parse_response(buf, n);
    ASSERT(std::strcmp(rr.type, "REJECTED") == 0, "ouch_rejected_parsed");
    ASSERT(rr.reason[0] == 'X', "ouch_rejected_reason");
    ASSERT(t.on_response(rr) == ouch::OrderState::REJECTED, "ouchstate_J_rejected");

    // #134 Broken Trade odwraca fill: TOK1 byl FILLED (100) -> bust 100 -> LIVE.
    n = OUCHMessage::encode_broken_trade(buf, "TOK1", 100, 99005, 'E');
    ASSERT(t.on_response(OUCHMessage::parse_response(buf, n)) == ouch::OrderState::LIVE,
           "ouchstate_broken_reverts_to_live");
    ASSERT(t.remaining("TOK1") == 100 && t.filled("TOK1") == 0, "ouchstate_broken_unfilled");
    ASSERT(t.brokens() == 1, "ouchstate_broken_count");

    // #112 Order Replaced ('U'): encode → decode (new + previous token).
    n = OUCHMessage::encode_replaced(buf, "NEWTOK", "TOK1", 80, 151.00, 4242);
    const OUCHResponse rp = OUCHMessage::parse_response(buf, n);
    ASSERT(std::strcmp(rp.type, "REPLACED") == 0, "ouch_replaced_parsed");
    ASSERT(std::strcmp(rp.token, "NEWTOK") == 0, "ouch_replaced_new_token");
    ASSERT(std::strcmp(rp.prev_token, "TOK1") == 0, "ouch_replaced_prev_token");
    ASSERT(rp.shares == 80 && rp.order_ref == 4242, "ouch_replaced_fields");

    // #127 Broken Trade ('B'): encode -> decode (bust wczesniejszego fillu).
    n = OUCHMessage::encode_broken_trade(buf, "TOK1", 50, 99001, 'E');
    const OUCHResponse rb = OUCHMessage::parse_response(buf, n);
    ASSERT(std::strcmp(rb.type, "BROKEN") == 0, "ouch_broken_parsed");
    ASSERT(std::strcmp(rb.token, "TOK1") == 0, "ouch_broken_token");
    ASSERT(rb.shares == 50 && rb.match_number == 99001, "ouch_broken_fields");
    ASSERT(rb.reason[0] == 'E', "ouch_broken_reason");

    // #178 Cancel Reject ('I'): the exchange rejects the cancel attempt.
    n = OUCHMessage::encode_cancel_reject(buf, "TOK9", 'T');   // T = too late (already executed)
    const OUCHResponse cr = OUCHMessage::parse_response(buf, n);
    ASSERT(std::strcmp(cr.type, "CXL_REJECT") == 0, "ouch_cxlrej_parsed");
    ASSERT(std::strcmp(cr.token, "TOK9") == 0, "ouch_cxlrej_token");
    ASSERT(cr.reason[0] == 'T', "ouch_cxlrej_reason");
    ASSERT(std::strcmp(OUCHMessage::parse_response(buf, 15).type, "ERROR") == 0,
           "ouch_cxlrej_short_error");

    // #186 Cancel Pending ('P'): cancellation accepted, not yet final.
    n = OUCHMessage::encode_cancel_pending(buf, "TOK5");
    const OUCHResponse cp = OUCHMessage::parse_response(buf, n);
    ASSERT(std::strcmp(cp.type, "CXL_PEND") == 0, "ouch_cxlpend_parsed");
    ASSERT(std::strcmp(cp.token, "TOK5") == 0, "ouch_cxlpend_token");
    ASSERT(std::strcmp(OUCHMessage::parse_response(buf, 14).type, "ERROR") == 0,
           "ouch_cxlpend_short_error");

    // #194 AIQ Canceled ('D'): self-match prevention removes part of the order.
    n = OUCHMessage::encode_aiq_canceled(buf, "TOK7", 40, 'Q');
    const OUCHResponse aq = OUCHMessage::parse_response(buf, n);
    ASSERT(std::strcmp(aq.type, "AIQ_CXL") == 0, "ouch_aiq_parsed");
    ASSERT(std::strcmp(aq.token, "TOK7") == 0 && aq.shares == 40, "ouch_aiq_fields");
    ASSERT(aq.reason[0] == 'Q', "ouch_aiq_reason");
    ASSERT(std::strcmp(OUCHMessage::parse_response(buf, 19).type, "ERROR") == 0,
           "ouch_aiq_short_error");

    // #202 System Event ('S'): session event (market open).
    n = OUCHMessage::encode_system_event(buf, 123456789, 'O');
    const OUCHResponse se = OUCHMessage::parse_response(buf, n);
    ASSERT(std::strcmp(se.type, "SYS_EVENT") == 0, "ouch_sysevent_parsed");
    ASSERT(se.match_number == 123456789 && se.reason[0] == 'O', "ouch_sysevent_fields");
    ASSERT(std::strcmp(OUCHMessage::parse_response(buf, 9).type, "ERROR") == 0,
           "ouch_sysevent_short_error");

    // #210 Restated ('R'): the exchange changes parameters without a client request.
    auto closepr = [](double a, double b) { const double d = a - b; return (d<0?-d:d) < 1e-6; };
    n = OUCHMessage::encode_restated(buf, "TOK8", 80, 100.05, 'P');
    const OUCHResponse rs = OUCHMessage::parse_response(buf, n);
    ASSERT(std::strcmp(rs.type, "RESTATED") == 0, "ouch_restated_parsed");
    ASSERT(std::strcmp(rs.token, "TOK8") == 0 && rs.shares == 80, "ouch_restated_token_shares");
    ASSERT(closepr(rs.price, 100.05) && rs.reason[0] == 'P', "ouch_restated_price_reason");
    ASSERT(std::strcmp(OUCHMessage::parse_response(buf, 23).type, "ERROR") == 0,
           "ouch_restated_short_error");

    // #218 expected_length — framing strumienia po typie.
    ASSERT(OUCHMessage::expected_length('A') == 41, "ouch_explen_accepted");
    ASSERT(OUCHMessage::expected_length('E') == 31, "ouch_explen_executed");
    ASSERT(OUCHMessage::expected_length('S') == 10, "ouch_explen_sysevent");
    ASSERT(OUCHMessage::expected_length('R') == 24, "ouch_explen_restated");
    ASSERT(OUCHMessage::expected_length('Z') == 0, "ouch_explen_unknown");

    // #152 parse_order — the exchange side decodes the client's O/X/U orders.
    auto closep = [](double a, double b) { const double d = a - b; return (d<0?-d:d) < 1e-6; };
    n = OUCHMessage::enter_order(buf, "TOK1", 'B', 100, "AAPL", 150.25);
    const OUCHOrder eo = OUCHMessage::parse_order(buf, n);
    ASSERT(eo.valid && eo.type == 'O' && std::strcmp(eo.token, "TOK1") == 0, "ouch_parse_enter");
    ASSERT(eo.side == 'B' && eo.shares == 100 && std::strcmp(eo.stock, "AAPL") == 0
           && closep(eo.price, 150.25) && eo.tif == 'D', "ouch_parse_enter_fields");
    n = OUCHMessage::cancel_order(buf, "TOK1", 30);
    const OUCHOrder co = OUCHMessage::parse_order(buf, n);
    ASSERT(co.valid && co.type == 'X' && co.shares == 30, "ouch_parse_cancel");
    n = OUCHMessage::replace_order(buf, "TOK1", "TOK2", 80, 151.0);
    const OUCHOrder ro = OUCHMessage::parse_order(buf, n);
    ASSERT(ro.valid && ro.type == 'U' && std::strcmp(ro.token, "TOK1") == 0
           && std::strcmp(ro.new_token, "TOK2") == 0 && ro.shares == 80, "ouch_parse_replace");

    // #226 Modify Order ('M') — volume reduction (decrease-only).
    n = OUCHMessage::modify_order(buf, "TOK1", 50);
    const OUCHOrder mo = OUCHMessage::parse_order(buf, n);
    ASSERT(mo.valid && mo.type == 'M' && std::strcmp(mo.token, "TOK1") == 0
           && mo.shares == 50, "ouch_parse_modify");
    ASSERT(OUCHMessage::validate_order(mo) == nullptr, "ouch_modify_valid");
    n = OUCHMessage::modify_order(buf, "TOK1", 0);                      // 0 = nieprawidlowe
    ASSERT(std::strcmp(OUCHMessage::validate_order(OUCHMessage::parse_order(buf, n)),
                       "non-positive shares") == 0, "ouch_modify_zero_rejected");

    // #169 validate_order — the exchange gateway validates the client's order.
    n = OUCHMessage::enter_order(buf, "TOK1", 'B', 100, "AAPL", 150.25);
    ASSERT(OUCHMessage::validate_order(OUCHMessage::parse_order(buf, n)) == nullptr,
           "ouch_validate_ok");
    n = OUCHMessage::enter_order(buf, "TOK1", 'B', 0, "AAPL", 150.25);   // shares 0
    ASSERT(std::strcmp(OUCHMessage::validate_order(OUCHMessage::parse_order(buf, n)),
                       "non-positive shares") == 0, "ouch_validate_zero_shares");
    n = OUCHMessage::enter_order(buf, "TOK1", 'X', 100, "AAPL", 150.25); // wrong side
    ASSERT(std::strcmp(OUCHMessage::validate_order(OUCHMessage::parse_order(buf, n)),
                       "invalid side") == 0, "ouch_validate_bad_side");

    // #418 protocol-conformance caps: 1,000,000 shares / $199,999.99.
    n = OUCHMessage::enter_order(buf, "TOK1", 'B', 1000000, "AAPL", 150.25);  // at the cap
    ASSERT(OUCHMessage::validate_order(OUCHMessage::parse_order(buf, n)) == nullptr,
           "ouch_cap_shares_at_max_ok");
    n = OUCHMessage::enter_order(buf, "TOK1", 'B', 1000001, "AAPL", 150.25);  // fat finger
    ASSERT(std::strcmp(OUCHMessage::validate_order(OUCHMessage::parse_order(buf, n)),
                       "shares exceed maximum") == 0, "ouch_cap_shares_over");
    n = OUCHMessage::enter_order(buf, "TOK1", 'B', 100, "AAPL", 250000.0);    // beyond the field cap
    ASSERT(std::strcmp(OUCHMessage::validate_order(OUCHMessage::parse_order(buf, n)),
                       "price exceeds maximum") == 0, "ouch_cap_price_over");
    n = OUCHMessage::replace_order(buf, "TOK1", "TOK2", 2000000, 150.25);     // replace upsized too far
    ASSERT(std::strcmp(OUCHMessage::validate_order(OUCHMessage::parse_order(buf, n)),
                       "shares exceed maximum") == 0, "ouch_cap_replace_shares");
    n = OUCHMessage::modify_order(buf, "TOK1", 5000000);                      // modify is capped too
    ASSERT(std::strcmp(OUCHMessage::validate_order(OUCHMessage::parse_order(buf, n)),
                       "shares exceed maximum") == 0, "ouch_cap_modify_shares");

    // #242 tracker volume aggregates (fresh tracker).
    ouch::OUCHOrderTracker tt;
    tt.on_new("TOKA", 100);
    tt.on_new("TOKB", 200);                                 // remaining 300 lacznie
    n = OUCHMessage::encode_executed(buf, "TOKA", 60, 10.0, 1);
    tt.on_response(OUCHMessage::parse_response(buf, n));     // TOKA: filled 60, remaining 40
    ASSERT(tt.total_filled_shares() == 60, "tracker_total_filled");
    ASSERT(tt.total_remaining_shares() == 240, "tracker_total_remaining");  // 40 + 200
    // #250 fill_rate = filled / ordered
    ASSERT(tt.total_ordered_shares() == 300, "tracker_total_ordered");      // 100 + 200
    ASSERT(std::fabs(tt.fill_rate() - 60.0/300.0) < 1e-9, "tracker_fill_rate"); // 60/300 = 0.2

    // #264 parse_stream — frame + decode concatenated OUCH messages.
    uint8_t sbuf[256];
    int soff = 0;
    soff += OUCHMessage::encode_accepted(sbuf + soff, "TOK1", 'B', 100, "AAPL", 150.25, 99001); // 41
    soff += OUCHMessage::encode_executed(sbuf + soff, "TOK1", 60, 150.25, 5001);                // 31
    soff += OUCHMessage::encode_cancelled(sbuf + soff, "TOK1", 40, 'U');                        // 20
    std::string seen;
    int scount = 0;
    const int consumed = OUCHMessage::parse_stream(sbuf, soff,
        [&](const OUCHResponse& r) { ++scount; seen += r.type[0]; });
    ASSERT(consumed == soff && scount == 3, "ouch_stream_consumed_all");
    ASSERT(seen == "AEC", "ouch_stream_types");   // Accepted, Executed, Cancelled
    // partial trailing message: drop 5 bytes of the last -> only first two decode
    int pcount = 0;
    const int consumed2 = OUCHMessage::parse_stream(sbuf, soff - 5,
        [&](const OUCHResponse&) { ++pcount; });
    ASSERT(consumed2 == 41 + 31 && pcount == 2, "ouch_stream_partial_tail");

    // #272 active_count — non-terminal (working) orders.
    ouch::OUCHOrderTracker ac;
    ac.on_new("A", 100); ac.on_new("B", 100);                 // both NEW (active)
    ASSERT(ac.active_count() == 2, "ouch_active_two_new");
    n = OUCHMessage::encode_executed(buf, "A", 100, 10.0, 1);
    ac.on_response(OUCHMessage::parse_response(buf, n));       // A FILLED
    ASSERT(ac.active_count() == 1, "ouch_active_after_fill");  // only B
    n = OUCHMessage::encode_executed(buf, "B", 50, 10.0, 2);
    ac.on_response(OUCHMessage::parse_response(buf, n));       // B PARTIAL (still active)
    ASSERT(ac.active_count() == 1, "ouch_active_partial");
    n = OUCHMessage::encode_cancelled(buf, "B", 50, 'U');
    ac.on_response(OUCHMessage::parse_response(buf, n));       // B CANCELLED
    ASSERT(ac.active_count() == 0, "ouch_active_after_cancel");

    // #280 cancel_pending_count — cancels sent, awaiting confirmation.
    ouch::OUCHOrderTracker cpt;
    cpt.on_new("A", 100); cpt.on_new("B", 100);
    n = OUCHMessage::encode_accepted(buf, "A", 'B', 100, "AAPL", 150.25, 99001);
    cpt.on_response(OUCHMessage::parse_response(buf, n));      // A -> LIVE
    cpt.on_cancel_sent("A");                                   // cancel sent, awaiting C
    ASSERT(cpt.cancel_pending_count() == 1 && cpt.is_pending_cancel("A"), "ouch_cxlpend_one");
    n = OUCHMessage::encode_cancelled(buf, "A", 100, 'U');
    cpt.on_response(OUCHMessage::parse_response(buf, n));      // C confirms -> flag cleared
    ASSERT(cpt.cancel_pending_count() == 0, "ouch_cxlpend_cleared");

    // #378 cancel-lifecycle responses: CXL_PEND ('P') arms the pending flag,
    // CXL_REJECT ('I') disarms it — neither may kill the live order as
    // REJECTED (the pre-#378 else-branch behaviour).
    {
        ouch::OUCHOrderTracker clt;
        uint8_t clbuf[64];
        clt.on_new("CL1", 100);
        int cln = OUCHMessage::encode_accepted(clbuf, "CL1", 'B', 100, "AAPL", 100.0, 555);
        clt.on_response(OUCHMessage::parse_response(clbuf, cln));
        // Exchange-acked pending cancel arms the flag even without on_cancel_sent.
        cln = OUCHMessage::encode_cancel_pending(clbuf, "CL1");
        ASSERT(clt.on_response(OUCHMessage::parse_response(clbuf, cln)) == ouch::OrderState::LIVE,
               "ouch_clc_pend_keeps_live");
        ASSERT(clt.is_pending_cancel("CL1"), "ouch_clc_pend_arms_flag");
        ASSERT(clt.rejects() == 0, "ouch_clc_pend_not_counted_rejected");
        // Cancel Reject: the CANCEL died, the ORDER did not.
        cln = OUCHMessage::encode_cancel_reject(clbuf, "CL1", 'T');   // 'T' = too late
        ASSERT(clt.on_response(OUCHMessage::parse_response(clbuf, cln)) == ouch::OrderState::LIVE,
               "ouch_clc_rej_keeps_live");
        ASSERT(!clt.is_pending_cancel("CL1"), "ouch_clc_rej_disarms_flag");
        ASSERT(clt.rejects() == 0 && clt.cancel_rejects() == 1, "ouch_clc_rej_own_counter");
        // The order stays workable: it can still fill after the failed cancel.
        cln = OUCHMessage::encode_executed(clbuf, "CL1", 100, 100.0, 9001);
        ASSERT(clt.on_response(OUCHMessage::parse_response(clbuf, cln)) == ouch::OrderState::FILLED,
               "ouch_clc_fill_after_cxl_reject");
        ASSERT(clt.active_count() == 0, "ouch_clc_filled_not_active");
    }

    // #386 non-reject report routing: REPLACED migrates the record to the new
    // token, RESTATED adopts the exchange-stated quantity, AIQ_CXL decrements
    // — none of them may fall into the REJECTED else branch.
    {
        ouch::OUCHOrderTracker rmt;
        uint8_t rmb[64];
        // REPLACED after a partial fill: fills stay with the chain,
        // remaining becomes the replacement size, state lands PARTIAL.
        rmt.on_new("OLD1", 100);
        int rmn = OUCHMessage::encode_accepted(rmb, "OLD1", 'B', 100, "AAPL", 50.0, 111);
        rmt.on_response(OUCHMessage::parse_response(rmb, rmn));
        rmn = OUCHMessage::encode_executed(rmb, "OLD1", 30, 50.0, 8001);
        rmt.on_response(OUCHMessage::parse_response(rmb, rmn));
        rmn = OUCHMessage::encode_replaced(rmb, "NEW1", "OLD1", 120, 50.5, 222);
        ASSERT(rmt.on_response(OUCHMessage::parse_response(rmb, rmn)) == ouch::OrderState::PARTIAL,
               "ouch_rpl_partial_after_migration");
        ASSERT(rmt.state("OLD1") == ouch::OrderState::REJECTED, "ouch_rpl_old_token_gone");
        ASSERT(rmt.remaining("NEW1") == 120 && rmt.filled("NEW1") == 30, "ouch_rpl_chain_accounting");
        ASSERT(rmt.replaces() == 1 && rmt.rejects() == 0, "ouch_rpl_own_counter");
        // Replacing an unfilled order lands LIVE.
        rmt.on_new("OLD2", 50);
        rmn = OUCHMessage::encode_accepted(rmb, "OLD2", 'S', 50, "MSFT", 20.0, 333);
        rmt.on_response(OUCHMessage::parse_response(rmb, rmn));
        rmn = OUCHMessage::encode_replaced(rmb, "NEW2", "OLD2", 75, 21.0, 444);
        ASSERT(rmt.on_response(OUCHMessage::parse_response(rmb, rmn)) == ouch::OrderState::LIVE,
               "ouch_rpl_unfilled_live");
        // Unknown PREVIOUS token stays the desync path (REJECTED + counter).
        rmn = OUCHMessage::encode_replaced(rmb, "NEWX", "GHOST9", 10, 1.0, 555);
        ASSERT(rmt.on_response(OUCHMessage::parse_response(rmb, rmn)) == ouch::OrderState::REJECTED,
               "ouch_rpl_unknown_prev_rejected");
        ASSERT(rmt.rejects() == 1, "ouch_rpl_desync_counted");

        // RESTATED: adopt the new quantity, order keeps working.
        ouch::OUCHOrderTracker rst;
        uint8_t rsb[64];
        rst.on_new("RS1", 200);
        int rqn = OUCHMessage::encode_accepted(rsb, "RS1", 'B', 200, "AAPL", 10.0, 666);
        rst.on_response(OUCHMessage::parse_response(rsb, rqn));
        rqn = OUCHMessage::encode_restated(rsb, "RS1", 150, 10.05, 'P');
        ASSERT(rst.on_response(OUCHMessage::parse_response(rsb, rqn)) == ouch::OrderState::LIVE,
               "ouch_restated_keeps_live");
        ASSERT(rst.remaining("RS1") == 150 && rst.rejects() == 0, "ouch_restated_new_qty");

        // AIQ_CXL: a partial decrement lives on; a decrement to zero cancels.
        rqn = OUCHMessage::encode_aiq_canceled(rsb, "RS1", 50, 'Q');
        ASSERT(rst.on_response(OUCHMessage::parse_response(rsb, rqn)) == ouch::OrderState::LIVE,
               "ouch_aiq_partial_keeps_live");
        ASSERT(rst.remaining("RS1") == 100, "ouch_aiq_decrement_applied");
        rqn = OUCHMessage::encode_aiq_canceled(rsb, "RS1", 100, 'Q');
        ASSERT(rst.on_response(OUCHMessage::parse_response(rsb, rqn)) == ouch::OrderState::CANCELLED,
               "ouch_aiq_to_zero_cancels");
        ASSERT(rst.cancels() == 1 && rst.rejects() == 0, "ouch_aiq_zero_counts_cancel");
    }

    // #394 largest_remaining_token — the actionable WHICH for largest_remaining (#312).
    {
        ouch::OUCHOrderTracker lrt;
        uint8_t lrb[64];
        char lrtok[15];
        lrtok[0] = 'X';
        ASSERT(lrt.largest_remaining_token(lrtok) == 0 && lrtok[0] == '\0', "ouch_lrt_empty");
        lrt.on_new("AAA", 100);
        // NEW (sent, not yet acked) is not working -> still nothing to name.
        ASSERT(lrt.largest_remaining_token(lrtok) == 0, "ouch_lrt_new_not_working");
        int lrn = OUCHMessage::encode_accepted(lrb, "AAA", 'B', 100, "AAPL", 10.0, 1);
        lrt.on_response(OUCHMessage::parse_response(lrb, lrn));
        lrt.on_new("BBB", 400);
        lrn = OUCHMessage::encode_accepted(lrb, "BBB", 'B', 400, "AAPL", 10.0, 2);
        lrt.on_response(OUCHMessage::parse_response(lrb, lrn));
        ASSERT(lrt.largest_remaining_token(lrtok) == 400
               && std::strcmp(lrtok, "BBB") == 0, "ouch_lrt_names_largest");
        ASSERT(lrt.largest_remaining_token(lrtok) == lrt.largest_remaining(),
               "ouch_lrt_value_matches_312");
        // A partial fill shrinks BBB (400 -> 50) below AAA -> the crown moves.
        lrn = OUCHMessage::encode_executed(lrb, "BBB", 350, 10.0, 900);
        lrt.on_response(OUCHMessage::parse_response(lrb, lrn));
        ASSERT(lrt.largest_remaining_token(lrtok) == 100
               && std::strcmp(lrtok, "AAA") == 0, "ouch_lrt_crown_moves");
        // Cancelling the winner hands it to the runner-up.
        lrn = OUCHMessage::encode_cancelled(lrb, "AAA", 100, 'U');
        lrt.on_response(OUCHMessage::parse_response(lrb, lrn));
        ASSERT(lrt.largest_remaining_token(lrtok) == 50
               && std::strcmp(lrtok, "BBB") == 0, "ouch_lrt_after_cancel");
    }

    // #402 pending_cancel_shares / pending_cancel_fraction — condemned exposure.
    {
        ouch::OUCHOrderTracker pcs;
        uint8_t pcb[64];
        ASSERT(pcs.pending_cancel_shares() == 0 && pcs.pending_cancel_fraction() == 0.0,
               "ouch_pcs_empty");
        pcs.on_new("P1", 100);
        int pcn = OUCHMessage::encode_accepted(pcb, "P1", 'B', 100, "AAPL", 10.0, 1);
        pcs.on_response(OUCHMessage::parse_response(pcb, pcn));
        pcs.on_new("P2", 300);
        pcn = OUCHMessage::encode_accepted(pcb, "P2", 'B', 300, "AAPL", 10.0, 2);
        pcs.on_response(OUCHMessage::parse_response(pcb, pcn));
        ASSERT(pcs.pending_cancel_shares() == 0, "ouch_pcs_none_armed");
        pcs.on_cancel_sent("P2");
        ASSERT(pcs.pending_cancel_shares() == 300, "ouch_pcs_armed_shares");
        // 300 condemned of 400 working -> 0.75 of the book is an illusion.
        ASSERT(std::fabs(pcs.pending_cancel_fraction() - 0.75) < 1e-9, "ouch_pcs_fraction_3_4");
        // A fill racing the cancel shrinks the condemned shares too.
        pcn = OUCHMessage::encode_executed(pcb, "P2", 100, 10.0, 900);
        pcs.on_response(OUCHMessage::parse_response(pcb, pcn));
        ASSERT(pcs.pending_cancel_shares() == 200, "ouch_pcs_fill_shrinks");
        ASSERT(std::fabs(pcs.pending_cancel_fraction() - 200.0 / 300.0) < 1e-9,
               "ouch_pcs_fraction_2_3");
        // The Cancelled ack frees the exposure.
        pcn = OUCHMessage::encode_cancelled(pcb, "P2", 200, 'U');
        pcs.on_response(OUCHMessage::parse_response(pcb, pcn));
        ASSERT(pcs.pending_cancel_shares() == 0 && pcs.pending_cancel_fraction() == 0.0,
               "ouch_pcs_ack_frees");
    }

    // #410 avg_fill_price / fill_vwap — pricing the executions (MILESTONE 410).
    {
        ouch::OUCHOrderTracker fvt;
        uint8_t fvb[64];
        ASSERT(fvt.avg_fill_price("NONE") == 0.0 && fvt.fill_vwap() == 0.0, "ouch_fv_empty");
        fvt.on_new("F1", 100);
        int fvn = OUCHMessage::encode_accepted(fvb, "F1", 'B', 100, "AAPL", 10.0, 1);
        fvt.on_response(OUCHMessage::parse_response(fvb, fvn));
        fvn = OUCHMessage::encode_executed(fvb, "F1", 40, 10.00, 901);
        fvt.on_response(OUCHMessage::parse_response(fvb, fvn));
        fvn = OUCHMessage::encode_executed(fvb, "F1", 60, 20.00, 902);
        fvt.on_response(OUCHMessage::parse_response(fvb, fvn));
        // (40*10 + 60*20) / 100 = 16.00 — per order AND session-wide so far.
        ASSERT(std::fabs(fvt.avg_fill_price("F1") - 16.0) < 1e-9, "ouch_fv_order_avg_16");
        ASSERT(std::fabs(fvt.fill_vwap() - 16.0) < 1e-9, "ouch_fv_session_16");
        // A bust unwinds shares at the AVERAGE (no price on 'B'): the
        // per-order average survives; the gross session tape does not move.
        fvn = OUCHMessage::encode_broken_trade(fvb, "F1", 50, 902, 'E');
        fvt.on_response(OUCHMessage::parse_response(fvb, fvn));
        ASSERT(fvt.filled("F1") == 50, "ouch_fv_bust_shares");
        ASSERT(std::fabs(fvt.avg_fill_price("F1") - 16.0) < 1e-9, "ouch_fv_bust_avg_survives");
        ASSERT(std::fabs(fvt.fill_vwap() - 16.0) < 1e-9, "ouch_fv_session_gross");
        // A second order at a different price moves the session VWAP:
        // gross tape = (1600 + 100*40) / (200 as-executed shares) = 28.
        fvt.on_new("F2", 100);
        fvn = OUCHMessage::encode_accepted(fvb, "F2", 'S', 100, "MSFT", 40.0, 2);
        fvt.on_response(OUCHMessage::parse_response(fvb, fvn));
        fvn = OUCHMessage::encode_executed(fvb, "F2", 100, 40.00, 903);
        fvt.on_response(OUCHMessage::parse_response(fvb, fvn));
        ASSERT(std::fabs(fvt.avg_fill_price("F2") - 40.0) < 1e-9, "ouch_fv_second_order_avg");
        ASSERT(std::fabs(fvt.fill_vwap() - 28.0) < 1e-9, "ouch_fv_session_blended_28");
        // Cancels never price anything.
        fvt.on_new("F3", 10);
        fvn = OUCHMessage::encode_accepted(fvb, "F3", 'B', 10, "AAPL", 5.0, 3);
        fvt.on_response(OUCHMessage::parse_response(fvb, fvn));
        fvn = OUCHMessage::encode_cancelled(fvb, "F3", 10, 'U');
        fvt.on_response(OUCHMessage::parse_response(fvb, fvn));
        ASSERT(fvt.avg_fill_price("F3") == 0.0 && std::fabs(fvt.fill_vwap() - 28.0) < 1e-9,
               "ouch_fv_cancel_prices_nothing");
    }

    // #426 desyncs — unknown-token responses split out of rejects().
    {
        ouch::OUCHOrderTracker dst;
        uint8_t dsb[64];
        ASSERT(dst.desyncs() == 0, "ouch_dsy_fresh_zero");
        // A REAL order rejection: rejects() moves, desyncs() does not.
        dst.on_new("D1", 100);
        int dsn = OUCHMessage::encode_rejected(dsb, "D1", 'X');
        dst.on_response(OUCHMessage::parse_response(dsb, dsn));
        ASSERT(dst.rejects() == 1 && dst.desyncs() == 0, "ouch_dsy_real_reject_not_desync");
        // An ack for a token never registered: both move — and the
        // difference rejects() - desyncs() stays the true rejection count.
        dsn = OUCHMessage::encode_accepted(dsb, "GHOST1", 'B', 10, "X", 1.0, 9);
        dst.on_response(OUCHMessage::parse_response(dsb, dsn));
        ASSERT(dst.rejects() == 2 && dst.desyncs() == 1, "ouch_dsy_unknown_token_counts");
        ASSERT(dst.rejects() - dst.desyncs() == 1, "ouch_dsy_difference_is_true_rejects");
        // A REPLACED naming an unknown PREVIOUS token is a desync too.
        dsn = OUCHMessage::encode_replaced(dsb, "NEWD", "GHOST2", 10, 1.0, 10);
        dst.on_response(OUCHMessage::parse_response(dsb, dsn));
        ASSERT(dst.desyncs() == 2, "ouch_dsy_replaced_unknown_prev");
    }

    // #442 reset_session — the tracker's new-day wipe.
    {
        ouch::OUCHOrderTracker rss;
        uint8_t rssb[64];
        rss.on_new("S1", 100);
        int rssn = OUCHMessage::encode_accepted(rssb, "S1", 'B', 100, "AAPL", 10.0, 1);
        rss.on_response(OUCHMessage::parse_response(rssb, rssn));
        rssn = OUCHMessage::encode_executed(rssb, "S1", 40, 10.0, 900);
        rss.on_response(OUCHMessage::parse_response(rssb, rssn));
        rssn = OUCHMessage::encode_accepted(rssb, "GHOST", 'B', 10, "X", 1.0, 2);
        rss.on_response(OUCHMessage::parse_response(rssb, rssn));   // a desync too
        ASSERT(rss.order_count() == 1 && rss.exec_count() == 1 && rss.desyncs() == 1,
               "ouch_rss_populated");
        rss.reset_session();
        ASSERT(rss.order_count() == 0 && rss.active_count() == 0, "ouch_rss_map_wiped");
        ASSERT(rss.live() == 0 && rss.fills() == 0 && rss.rejects() == 0
               && rss.desyncs() == 0 && rss.exec_count() == 0, "ouch_rss_counters_zero");
        ASSERT(rss.total_ordered_shares() == 0 && rss.fill_vwap() == 0.0
               && rss.fill_rate() == 0.0, "ouch_rss_aggregates_zero");
        // Yesterday's token is REUSABLE today without colliding.
        rss.on_new("S1", 50);
        ASSERT(rss.remaining("S1") == 50 && rss.filled("S1") == 0, "ouch_rss_token_reuse_clean");
    }

    // #450 working_shares_side / net_working_shares — the tracker's first
    // direction-aware read (MILESTONE 450).
    {
        ouch::OUCHOrderTracker wss;
        uint8_t wsb[64];
        ASSERT(wss.working_shares_side('B') == 0 && wss.net_working_shares() == 0,
               "ouch_wss_empty");
        wss.on_new("B1", 300);
        // Unacked NEW carries no confirmed side — counts on neither.
        ASSERT(wss.working_shares_side('B') == 0, "ouch_wss_unacked_sideless");
        int wsn = OUCHMessage::encode_accepted(wsb, "B1", 'B', 300, "AAPL", 10.0, 1);
        wss.on_response(OUCHMessage::parse_response(wsb, wsn));
        wss.on_new("S1", 100);
        wsn = OUCHMessage::encode_accepted(wsb, "S1", 'S', 100, "AAPL", 10.1, 2);
        wss.on_response(OUCHMessage::parse_response(wsb, wsn));
        ASSERT(wss.working_shares_side('B') == 300 && wss.working_shares_side('S') == 100,
               "ouch_wss_split");
        ASSERT(wss.net_working_shares() == 200, "ouch_wss_net_long_tilt");
        // The split sums to the direction-blind aggregate (#304).
        ASSERT(wss.working_shares_side('B') + wss.working_shares_side('S')
               == wss.working_shares(), "ouch_wss_sums_to_304");
        // A partial fill shrinks its own side only.
        wsn = OUCHMessage::encode_executed(wsb, "B1", 120, 10.0, 900);
        wss.on_response(OUCHMessage::parse_response(wsb, wsn));
        ASSERT(wss.working_shares_side('B') == 180 && wss.working_shares_side('S') == 100,
               "ouch_wss_fill_shrinks_own_side");
        ASSERT(wss.net_working_shares() == 80, "ouch_wss_net_after_fill");
        // A Replaced migration carries the side with the chain (#386 copy).
        wsn = OUCHMessage::encode_replaced(wsb, "B2", "B1", 250, 10.05, 3);
        wss.on_response(OUCHMessage::parse_response(wsb, wsn));
        ASSERT(wss.working_shares_side('B') == 250, "ouch_wss_side_survives_migration");
    }

    // #458 bought/sold/net_filled_shares — realized directional flow.
    {
        ouch::OUCHOrderTracker rff;
        uint8_t rfb[64];
        ASSERT(rff.bought_shares() == 0 && rff.net_filled_shares() == 0, "ouch_rff_empty");
        rff.on_new("B1", 200);
        int rfn = OUCHMessage::encode_accepted(rfb, "B1", 'B', 200, "AAPL", 10.0, 1);
        rff.on_response(OUCHMessage::parse_response(rfb, rfn));
        rff.on_new("S1", 150);
        rfn = OUCHMessage::encode_accepted(rfb, "S1", 'S', 150, "AAPL", 10.1, 2);
        rff.on_response(OUCHMessage::parse_response(rfb, rfn));
        // Buy 120 of 200, sell 150 of 150 -> net -30.
        rfn = OUCHMessage::encode_executed(rfb, "B1", 120, 10.0, 900);
        rff.on_response(OUCHMessage::parse_response(rfb, rfn));
        rfn = OUCHMessage::encode_executed(rfb, "S1", 150, 10.1, 901);
        rff.on_response(OUCHMessage::parse_response(rfb, rfn));
        ASSERT(rff.bought_shares() == 120 && rff.sold_shares() == 150, "ouch_rff_split");
        ASSERT(rff.net_filled_shares() == -30, "ouch_rff_net_short");
        // The split sums to the direction-blind exec tape (#328).
        ASSERT(rff.bought_shares() + rff.sold_shares() == rff.total_filled_shares(),
               "ouch_rff_sums_to_filled");
        // A bust on the buy unwinds bought flow only.
        rfn = OUCHMessage::encode_broken_trade(rfb, "B1", 20, 900, 'E');
        rff.on_response(OUCHMessage::parse_response(rfb, rfn));
        ASSERT(rff.bought_shares() == 100 && rff.sold_shares() == 150, "ouch_rff_bust_unwinds_buy");
        ASSERT(rff.net_filled_shares() == -50, "ouch_rff_net_after_bust");
        // reset_session clears the realized flow.
        rff.reset_session();
        ASSERT(rff.bought_shares() == 0 && rff.sold_shares() == 0
               && rff.net_filled_shares() == 0, "ouch_rff_reset");
    }

    // #474 avg_buy_price / avg_sell_price / realized_spread_capture.
    {
        ouch::OUCHOrderTracker mmk;
        uint8_t mmb[64];
        auto cl = [](double a, double b) { const double d = a - b; return (d<0?-d:d) < 1e-9; };
        ASSERT(mmk.avg_buy_price() == 0.0 && mmk.realized_spread_capture() == 0.0, "ouch_mmk_empty");
        // Buy 100@10.00 then 100@10.20 -> buy VWAP 10.10.
        mmk.on_new("B1", 200);
        int mmn = OUCHMessage::encode_accepted(mmb, "B1", 'B', 200, "AAPL", 10.0, 1);
        mmk.on_response(OUCHMessage::parse_response(mmb, mmn));
        mmn = OUCHMessage::encode_executed(mmb, "B1", 100, 10.00, 900);
        mmk.on_response(OUCHMessage::parse_response(mmb, mmn));
        mmn = OUCHMessage::encode_executed(mmb, "B1", 100, 10.20, 901);
        mmk.on_response(OUCHMessage::parse_response(mmb, mmn));
        ASSERT(cl(mmk.avg_buy_price(), 10.10), "ouch_mmk_buy_vwap");
        // Only one side filled -> spread capture not yet meaningful.
        ASSERT(mmk.realized_spread_capture() == 0.0, "ouch_mmk_one_sided_zero");
        // Sell 100@10.30 -> sell VWAP 10.30, capture 0.20.
        mmk.on_new("S1", 100);
        mmn = OUCHMessage::encode_accepted(mmb, "S1", 'S', 100, "AAPL", 10.3, 2);
        mmk.on_response(OUCHMessage::parse_response(mmb, mmn));
        mmn = OUCHMessage::encode_executed(mmb, "S1", 100, 10.30, 902);
        mmk.on_response(OUCHMessage::parse_response(mmb, mmn));
        ASSERT(cl(mmk.avg_sell_price(), 10.30), "ouch_mmk_sell_vwap");
        ASSERT(cl(mmk.realized_spread_capture(), 0.20), "ouch_mmk_spread_capture");
        // #506 bps version: 0.20 over a 10.10 buy VWAP.
        ASSERT(cl(mmk.realized_spread_capture_bps(), (10.30 - 10.10) / 10.10 * 10000.0),
               "ouch_mmk_spread_capture_bps");
        // A bust on the buy leg leaves the buy VWAP unchanged (proportional
        // notional unwind): 50 of 200 busted -> avg_buy still 10.10.
        mmn = OUCHMessage::encode_broken_trade(mmb, "B1", 50, 900, 'E');
        mmk.on_response(OUCHMessage::parse_response(mmb, mmn));
        ASSERT(mmk.bought_shares() == 150 && cl(mmk.avg_buy_price(), 10.10),
               "ouch_mmk_bust_keeps_vwap");
        ASSERT(cl(mmk.realized_spread_capture(), 0.20), "ouch_mmk_capture_after_bust");
        mmk.reset_session();
        ASSERT(mmk.avg_buy_price() == 0.0 && mmk.avg_sell_price() == 0.0, "ouch_mmk_reset");

        // #506: a clean 100-bps scenario (0.10 over a 10.00 buy VWAP).
        ouch::OUCHOrderTracker sc;
        uint8_t scb[64];
        ASSERT(sc.realized_spread_capture_bps() == 0.0, "ouch_scbps_empty");
        sc.on_new("B", 100);
        int scn = OUCHMessage::encode_accepted(scb, "B", 'B', 100, "AAPL", 10.0, 1);
        sc.on_response(OUCHMessage::parse_response(scb, scn));
        scn = OUCHMessage::encode_executed(scb, "B", 100, 10.00, 900);
        sc.on_response(OUCHMessage::parse_response(scb, scn));
        // One-sided so far -> not meaningful.
        ASSERT(sc.realized_spread_capture_bps() == 0.0, "ouch_scbps_one_sided");
        sc.on_new("S", 100);
        scn = OUCHMessage::encode_accepted(scb, "S", 'S', 100, "AAPL", 10.1, 2);
        sc.on_response(OUCHMessage::parse_response(scb, scn));
        scn = OUCHMessage::encode_executed(scb, "S", 100, 10.10, 901);
        sc.on_response(OUCHMessage::parse_response(scb, scn));
        // 0.10 over 10.00 = 100 bps exactly.
        ASSERT(cl(sc.realized_spread_capture_bps(), 100.0), "ouch_scbps_100");
    }

    // #482 net_cash_flow / gross_traded_notional.
    {
        ouch::OUCHOrderTracker ncf;
        uint8_t ncb[64];
        auto cl = [](double a, double b) { const double d = a - b; return (d<0?-d:d) < 1e-9; };
        ASSERT(ncf.net_cash_flow() == 0.0 && ncf.gross_traded_notional() == 0.0, "ouch_ncf_empty");
        // Buy 100@10.00, sell 100@10.10 -> flat, net cash +10 (= realized P&L).
        ncf.on_new("B1", 100);
        int nn = OUCHMessage::encode_accepted(ncb, "B1", 'B', 100, "AAPL", 10.0, 1);
        ncf.on_response(OUCHMessage::parse_response(ncb, nn));
        nn = OUCHMessage::encode_executed(ncb, "B1", 100, 10.00, 900);
        ncf.on_response(OUCHMessage::parse_response(ncb, nn));
        ncf.on_new("S1", 100);
        nn = OUCHMessage::encode_accepted(ncb, "S1", 'S', 100, "AAPL", 10.1, 2);
        ncf.on_response(OUCHMessage::parse_response(ncb, nn));
        nn = OUCHMessage::encode_executed(ncb, "S1", 100, 10.10, 901);
        ncf.on_response(OUCHMessage::parse_response(ncb, nn));
        ASSERT(ncf.net_filled_shares() == 0, "ouch_ncf_flat");
        ASSERT(cl(ncf.net_cash_flow(), 10.0), "ouch_ncf_flat_is_realized_pnl");
        ASSERT(cl(ncf.gross_traded_notional(), 2010.0), "ouch_ncf_turnover");
        // Gross = buy + sell notional = fill_vwap x exec shares.
        ASSERT(cl(ncf.gross_traded_notional(), ncf.fill_vwap() * 200.0), "ouch_ncf_gross_identity");
        // Buy 50 more @10.00 -> net long 50, net cash goes negative (cash out).
        ncf.on_new("B2", 50);
        nn = OUCHMessage::encode_accepted(ncb, "B2", 'B', 50, "AAPL", 10.0, 3);
        ncf.on_response(OUCHMessage::parse_response(ncb, nn));
        nn = OUCHMessage::encode_executed(ncb, "B2", 50, 10.00, 902);
        ncf.on_response(OUCHMessage::parse_response(ncb, nn));
        ASSERT(ncf.net_filled_shares() == 50, "ouch_ncf_now_long");
        ASSERT(cl(ncf.net_cash_flow(), 10.0 - 500.0), "ouch_ncf_open_position_cash_out");
        ncf.reset_session();
        ASSERT(ncf.net_cash_flow() == 0.0 && ncf.gross_traded_notional() == 0.0, "ouch_ncf_reset");
    }

    // #490 mark_to_market_pnl — total P&L at a caller-supplied mark.
    {
        auto cl = [](double a, double b) { const double d = a - b; return (d<0?-d:d) < 1e-9; };
        ouch::OUCHOrderTracker mtm;
        uint8_t mtb[64];
        ASSERT(mtm.mark_to_market_pnl(100.0) == 0.0, "ouch_mtm_empty_zero");
        // Long 100 @ 10.00: cash -1000, pos +100. MtM = -1000 + 100*mark.
        mtm.on_new("L1", 100);
        int mn = OUCHMessage::encode_accepted(mtb, "L1", 'B', 100, "AAPL", 10.0, 1);
        mtm.on_response(OUCHMessage::parse_response(mtb, mn));
        mn = OUCHMessage::encode_executed(mtb, "L1", 100, 10.00, 900);
        mtm.on_response(OUCHMessage::parse_response(mtb, mn));
        ASSERT(cl(mtm.mark_to_market_pnl(12.0), 200.0), "ouch_mtm_long_unrealized_up");
        ASSERT(cl(mtm.mark_to_market_pnl(10.0), 0.0), "ouch_mtm_long_at_cost");
        ASSERT(cl(mtm.mark_to_market_pnl(8.0), -200.0), "ouch_mtm_long_underwater");
        // Sell 100 @ 11.00 -> flat: MtM collapses to the realized cash (+100),
        // independent of the mark.
        mtm.on_new("S1", 100);
        mn = OUCHMessage::encode_accepted(mtb, "S1", 'S', 100, "AAPL", 11.0, 2);
        mtm.on_response(OUCHMessage::parse_response(mtb, mn));
        mn = OUCHMessage::encode_executed(mtb, "S1", 100, 11.00, 901);
        mtm.on_response(OUCHMessage::parse_response(mtb, mn));
        ASSERT(mtm.net_filled_shares() == 0, "ouch_mtm_flat");
        ASSERT(cl(mtm.mark_to_market_pnl(12.0), 100.0)
               && cl(mtm.mark_to_market_pnl(5.0), 100.0), "ouch_mtm_flat_mark_independent");

        // A fresh SHORT: sell 50 @ 20.00 -> cash +1000, pos -50.
        ouch::OUCHOrderTracker sh;
        uint8_t shb[64];
        sh.on_new("SH1", 50);
        int sn = OUCHMessage::encode_accepted(shb, "SH1", 'S', 50, "TSLA", 20.0, 1);
        sh.on_response(OUCHMessage::parse_response(shb, sn));
        sn = OUCHMessage::encode_executed(shb, "SH1", 50, 20.00, 902);
        sh.on_response(OUCHMessage::parse_response(shb, sn));
        // Short profits as the mark FALLS: MtM@18 = 1000 - 900 = +100.
        ASSERT(cl(sh.mark_to_market_pnl(18.0), 100.0), "ouch_mtm_short_profit");
        ASSERT(cl(sh.mark_to_market_pnl(22.0), -100.0), "ouch_mtm_short_loss");
        // #498 breakeven for the short: -1000 / -50 = 20 = the entry (no
        // spread captured yet), and MtM there is exactly 0.
        ASSERT(cl(sh.breakeven_mark(), 20.0), "ouch_be_short_at_entry");
        ASSERT(cl(sh.mark_to_market_pnl(sh.breakeven_mark()), 0.0), "ouch_be_short_zeroes_mtm");
    }

    // #498 breakeven_mark — where mark_to_market_pnl washes to zero.
    {
        auto cl = [](double a, double b) { const double d = a - b; return (d<0?-d:d) < 1e-9; };
        ouch::OUCHOrderTracker be;
        uint8_t beb[64];
        ASSERT(be.breakeven_mark() == 0.0, "ouch_be_empty");
        // Long 100 @ 10.00: cash -1000, pos +100 -> breakeven at the entry 10.
        be.on_new("B1", 100);
        int bn = OUCHMessage::encode_accepted(beb, "B1", 'B', 100, "AAPL", 10.0, 1);
        be.on_response(OUCHMessage::parse_response(beb, bn));
        bn = OUCHMessage::encode_executed(beb, "B1", 100, 10.00, 900);
        be.on_response(OUCHMessage::parse_response(beb, bn));
        ASSERT(cl(be.breakeven_mark(), 10.0), "ouch_be_long_at_entry");
        ASSERT(cl(be.mark_to_market_pnl(be.breakeven_mark()), 0.0), "ouch_be_zeroes_mtm");
        // Sell 50 @ 12.00 -> cash -400, pos +50: captured spread drops the
        // breakeven BELOW the entry to 8 (it can fall to 8 and still be even).
        be.on_new("S1", 50);
        bn = OUCHMessage::encode_accepted(beb, "S1", 'S', 50, "AAPL", 12.0, 2);
        be.on_response(OUCHMessage::parse_response(beb, bn));
        bn = OUCHMessage::encode_executed(beb, "S1", 50, 12.00, 901);
        be.on_response(OUCHMessage::parse_response(beb, bn));
        ASSERT(be.net_filled_shares() == 50, "ouch_be_still_long_50");
        ASSERT(cl(be.breakeven_mark(), 8.0), "ouch_be_dropped_below_entry");
        ASSERT(cl(be.mark_to_market_pnl(8.0), 0.0), "ouch_be_new_zeroes_mtm");
        // Close the rest @ 8.00 -> flat: no breakeven (MtM is now constant).
        be.on_new("S2", 50);
        bn = OUCHMessage::encode_accepted(beb, "S2", 'S', 50, "AAPL", 8.0, 3);
        be.on_response(OUCHMessage::parse_response(beb, bn));
        bn = OUCHMessage::encode_executed(beb, "S2", 50, 8.00, 902);
        be.on_response(OUCHMessage::parse_response(beb, bn));
        ASSERT(be.net_filled_shares() == 0 && be.breakeven_mark() == 0.0, "ouch_be_flat_none");
    }

    // #514 realized_pnl — banked P&L on the round-tripped portion, mark-independent.
    {
        auto cl = [](double a, double b) { const double d = a - b; return (d<0?-d:d) < 1e-9; };
        ouch::OUCHOrderTracker rpl;
        uint8_t rpb[64];
        ASSERT(rpl.realized_pnl() == 0.0, "ouch_rpnl_empty_zero");
        // Buy 100 @ 10.00 -> long, nothing round-tripped yet.
        rpl.on_new("B1", 100);
        int rn = OUCHMessage::encode_accepted(rpb, "B1", 'B', 100, "AAPL", 10.0, 1);
        rpl.on_response(OUCHMessage::parse_response(rpb, rn));
        rn = OUCHMessage::encode_executed(rpb, "B1", 100, 10.00, 900);
        rpl.on_response(OUCHMessage::parse_response(rpb, rn));
        ASSERT(rpl.realized_pnl() == 0.0, "ouch_rpnl_one_sided_zero");
        // Sell 60 @ 11.00 -> matched 60 at (11 - 10) = +60 banked, still long 40.
        rpl.on_new("S1", 60);
        rn = OUCHMessage::encode_accepted(rpb, "S1", 'S', 60, "AAPL", 11.0, 2);
        rpl.on_response(OUCHMessage::parse_response(rpb, rn));
        rn = OUCHMessage::encode_executed(rpb, "S1", 60, 11.00, 901);
        rpl.on_response(OUCHMessage::parse_response(rpb, rn));
        ASSERT(rpl.net_filled_shares() == 40, "ouch_rpnl_still_long");
        ASSERT(cl(rpl.realized_pnl(), 60.0), "ouch_rpnl_partial_banked");
        // net_cash_flow still carries the open inventory's cash cost, so the two
        // DIVERGE while the position is open (660 sold - 1000 bought = -340).
        ASSERT(cl(rpl.net_cash_flow(), 660.0 - 1000.0), "ouch_rpnl_ncf_diverges_open");
        // Per-share identity: realized_pnl == matched * realized_spread_capture.
        ASSERT(cl(rpl.realized_pnl(), 60.0 * rpl.realized_spread_capture()), "ouch_rpnl_edge_identity");
        // #522 unrealized_pnl — the open long (40 @ avg cost 10) marked to a price.
        // At 12: 40*(12-10) = +80; at 10 (cost): 0; at 8: 40*(8-10) = -80.
        ASSERT(cl(rpl.unrealized_pnl(12.0), 80.0), "ouch_upnl_long_up");
        ASSERT(cl(rpl.unrealized_pnl(10.0), 0.0), "ouch_upnl_long_at_cost");
        ASSERT(cl(rpl.unrealized_pnl(8.0), -80.0), "ouch_upnl_long_underwater");
        // Decomposition identity: mark-to-market == realized + unrealized at any mark.
        ASSERT(cl(rpl.mark_to_market_pnl(12.0), rpl.realized_pnl() + rpl.unrealized_pnl(12.0)),
               "ouch_upnl_decomposition");
        // Close the last 40 @ 12.00 -> FLAT. avg_sell 11.4, realized 100*(11.4-10)
        // = 140, and now it equals net_cash_flow exactly (every share matched).
        rpl.on_new("S2", 40);
        rn = OUCHMessage::encode_accepted(rpb, "S2", 'S', 40, "AAPL", 12.0, 3);
        rpl.on_response(OUCHMessage::parse_response(rpb, rn));
        rn = OUCHMessage::encode_executed(rpb, "S2", 40, 12.00, 902);
        rpl.on_response(OUCHMessage::parse_response(rpb, rn));
        ASSERT(rpl.net_filled_shares() == 0, "ouch_rpnl_flat");
        ASSERT(cl(rpl.realized_pnl(), 140.0)
               && cl(rpl.realized_pnl(), rpl.net_cash_flow()), "ouch_rpnl_flat_eq_ncf");
        // #522: FLAT -> nothing open -> unrealized is 0 at any mark (within fp tol).
        ASSERT(cl(rpl.unrealized_pnl(12.0), 0.0) && cl(rpl.unrealized_pnl(5.0), 0.0),
               "ouch_upnl_flat_zero");
        rpl.reset_session();
        ASSERT(rpl.realized_pnl() == 0.0 && rpl.unrealized_pnl(10.0) == 0.0, "ouch_rpnl_reset");
    }

    // #466 projected_net_shares — realized (#458) + working (#450) exposure.
    {
        ouch::OUCHOrderTracker pnt;
        uint8_t pnb[64];
        ASSERT(pnt.projected_net_shares() == 0, "ouch_pnt_empty");
        // Buy order 200, accepted then 120 filled -> filled +120, working buy 80.
        pnt.on_new("B1", 200);
        int pnn = OUCHMessage::encode_accepted(pnb, "B1", 'B', 200, "AAPL", 10.0, 1);
        pnt.on_response(OUCHMessage::parse_response(pnb, pnn));
        pnn = OUCHMessage::encode_executed(pnb, "B1", 120, 10.0, 900);
        pnt.on_response(OUCHMessage::parse_response(pnb, pnn));
        // A resting sell of 100 (working, not filled).
        pnt.on_new("S1", 100);
        pnn = OUCHMessage::encode_accepted(pnb, "S1", 'S', 100, "AAPL", 10.2, 2);
        pnt.on_response(OUCHMessage::parse_response(pnb, pnn));
        ASSERT(pnt.net_filled_shares() == 120, "ouch_pnt_realized_120");
        ASSERT(pnt.net_working_shares() == -20, "ouch_pnt_working_minus20");   // 80 buy - 100 sell
        // Projected = 120 + (80 - 100) = 100.
        ASSERT(pnt.projected_net_shares() == 100, "ouch_pnt_projected_100");
        // Filling the resting sell moves it from working to realized — the
        // projection is unchanged (it already counted the sell).
        pnn = OUCHMessage::encode_executed(pnb, "S1", 100, 10.2, 901);
        pnt.on_response(OUCHMessage::parse_response(pnb, pnn));
        ASSERT(pnt.net_filled_shares() == 20 && pnt.net_working_shares() == 80,
               "ouch_pnt_after_sell_fill");
        ASSERT(pnt.projected_net_shares() == 100, "ouch_pnt_projection_stable");
    }

    // #288 filled_fraction — per-order completion.
    ouch::OUCHOrderTracker ff;
    ff.on_new("A", 100);                                       // filled 0 / remaining 100
    ASSERT(std::fabs(ff.filled_fraction("A") - 0.0) < 1e-9, "ouch_ff_zero");
    n = OUCHMessage::encode_executed(buf, "A", 60, 10.0, 1);
    ff.on_response(OUCHMessage::parse_response(buf, n));       // filled 60 / remaining 40
    ASSERT(std::fabs(ff.filled_fraction("A") - 0.6) < 1e-9, "ouch_ff_partial");
    ASSERT(ff.filled_fraction("UNKNOWN") == 0.0, "ouch_ff_unknown");

    // #296 status_count — point-in-time snapshot by state.
    ouch::OUCHOrderTracker sc;
    sc.on_new("A", 100); sc.on_new("B", 100); sc.on_new("C", 100);   // all NEW
    n = OUCHMessage::encode_accepted(buf, "A", 'B', 100, "AAPL", 150.25, 99101);
    sc.on_response(OUCHMessage::parse_response(buf, n));              // A -> LIVE
    n = OUCHMessage::encode_accepted(buf, "B", 'B', 100, "AAPL", 150.25, 99102);
    sc.on_response(OUCHMessage::parse_response(buf, n));              // B -> LIVE
    ASSERT(sc.status_count(ouch::OrderState::NEW) == 1, "ouch_sc_new");   // C
    ASSERT(sc.status_count(ouch::OrderState::LIVE) == 2, "ouch_sc_live"); // A, B
    n = OUCHMessage::encode_cancelled(buf, "A", 100, 'U');
    sc.on_response(OUCHMessage::parse_response(buf, n));              // A -> CANCELLED
    ASSERT(sc.status_count(ouch::OrderState::LIVE) == 1, "ouch_sc_live_after_cxl");
    ASSERT(sc.status_count(ouch::OrderState::CANCELLED) == 1, "ouch_sc_cancelled");

    // #304 working_shares — LIVE/PARTIAL remaining only (confirmed book exposure).
    ouch::OUCHOrderTracker ws;
    ws.on_new("A", 100); ws.on_new("B", 100);                       // both NEW (not acked)
    ASSERT(ws.working_shares() == 0, "ouch_ws_none_acked");         // distinct from total_remaining
    n = OUCHMessage::encode_accepted(buf, "A", 'B', 100, "AAPL", 150.25, 99201);
    ws.on_response(OUCHMessage::parse_response(buf, n));            // A -> LIVE, remaining 100
    ASSERT(ws.working_shares() == 100, "ouch_ws_a_live");
    n = OUCHMessage::encode_executed(buf, "A", 30, 150.25, 1);
    ws.on_response(OUCHMessage::parse_response(buf, n));            // A -> PARTIAL, remaining 70
    n = OUCHMessage::encode_accepted(buf, "B", 'B', 100, "AAPL", 150.25, 99202);
    ws.on_response(OUCHMessage::parse_response(buf, n));            // B -> LIVE, remaining 100
    ASSERT(ws.working_shares() == 170, "ouch_ws_partial_plus_live"); // 70 + 100
    n = OUCHMessage::encode_cancelled(buf, "A", 70, 'U');
    ws.on_response(OUCHMessage::parse_response(buf, n));            // A -> CANCELLED, dropped
    ASSERT(ws.working_shares() == 100, "ouch_ws_after_cancel");      // B only

    // #312 largest_remaining — biggest single working order by remaining.
    ouch::OUCHOrderTracker lr;
    lr.on_new("A", 100); lr.on_new("B", 300); lr.on_new("C", 200);
    ASSERT(lr.largest_remaining() == 0, "ouch_lr_none_working");     // all still NEW
    n = OUCHMessage::encode_accepted(buf, "A", 'B', 100, "AAPL", 150.25, 99301);
    lr.on_response(OUCHMessage::parse_response(buf, n));
    n = OUCHMessage::encode_accepted(buf, "B", 'B', 300, "AAPL", 150.25, 99302);
    lr.on_response(OUCHMessage::parse_response(buf, n));
    n = OUCHMessage::encode_accepted(buf, "C", 'B', 200, "AAPL", 150.25, 99303);
    lr.on_response(OUCHMessage::parse_response(buf, n));
    ASSERT(lr.largest_remaining() == 300, "ouch_lr_b_largest");      // B 300
    n = OUCHMessage::encode_executed(buf, "B", 250, 150.25, 1);
    lr.on_response(OUCHMessage::parse_response(buf, n));             // B -> remaining 50
    ASSERT(lr.largest_remaining() == 200, "ouch_lr_c_now_largest");  // C 200 > A 100 > B 50
    // #369 avg_working_shares — mean remaining over working orders.
    // Now A LIVE 100, B PARTIAL 50, C LIVE 200 -> (100+50+200)/3 = 116.666...
    ASSERT(lr.working_shares() == 350, "ouch_aws_working_total");
    ASSERT(std::fabs(lr.avg_working_shares() - 350.0/3.0) < 1e-9, "ouch_aws_mean");
    ouch::OUCHOrderTracker aws0;
    aws0.on_new("Z", 100);   // NEW only, nothing working
    ASSERT(aws0.avg_working_shares() == 0.0, "ouch_aws_none_working_zero");

    // #320 total_broken_shares / broken_share_rate
    ouch::OUCHOrderTracker bsr;
    bsr.on_new("X1", 200); bsr.on_new("X2", 100);
    n = OUCHMessage::encode_accepted(buf, "X1", 'B', 200, "AAPL", 50.0, 88001);
    bsr.on_response(OUCHMessage::parse_response(buf, n));
    n = OUCHMessage::encode_executed(buf, "X1", 200, 50.0, 1);
    bsr.on_response(OUCHMessage::parse_response(buf, n));
    ASSERT(bsr.total_broken_shares() == 0, "ouch_bsr_none_yet");
    n = OUCHMessage::encode_broken_trade(buf, "X1", 100, 88001, 'E');
    bsr.on_response(OUCHMessage::parse_response(buf, n));
    ASSERT(bsr.total_broken_shares() == 100, "ouch_bsr_100");
    // ordered = 300, broken = 100 -> rate ~= 1/3
    ASSERT(std::fabs(bsr.broken_share_rate() - 100.0/300.0) < 1e-9, "ouch_bsr_rate");
    n = OUCHMessage::encode_broken_trade(buf, "X1", 50, 88001, 'E');
    bsr.on_response(OUCHMessage::parse_response(buf, n));
    ASSERT(bsr.total_broken_shares() == 150, "ouch_bsr_150");
    ASSERT(bsr.brokens() == 2, "ouch_bsr_event_count");
    ouch::OUCHOrderTracker bsr0;
    ASSERT(std::fabs(bsr0.broken_share_rate()) < 1e-12, "ouch_bsr_zero");

    // #328 exec_count / avg_exec_shares — per-execution slice granularity.
    ouch::OUCHOrderTracker aes;
    ASSERT(aes.exec_count() == 0 && aes.avg_exec_shares() == 0.0, "ouch_aes_empty");
    aes.on_new("E1", 300);
    n = OUCHMessage::encode_accepted(buf, "E1", 'B', 300, "AAPL", 50.0, 77001);
    aes.on_response(OUCHMessage::parse_response(buf, n));
    n = OUCHMessage::encode_executed(buf, "E1", 100, 50.0, 1);   // slice 1: 100
    aes.on_response(OUCHMessage::parse_response(buf, n));
    n = OUCHMessage::encode_executed(buf, "E1", 200, 50.0, 2);   // slice 2: 200 (fills)
    aes.on_response(OUCHMessage::parse_response(buf, n));
    ASSERT(aes.exec_count() == 2, "ouch_aes_two_execs");
    ASSERT(std::fabs(aes.avg_exec_shares() - 150.0) < 1e-9, "ouch_aes_avg_150"); // (100+200)/2
    // an over-fill attempt clamps to remaining 0 -> exec 0 -> NOT counted
    n = OUCHMessage::encode_executed(buf, "E1", 50, 50.0, 3);
    aes.on_response(OUCHMessage::parse_response(buf, n));
    ASSERT(aes.exec_count() == 2, "ouch_aes_overfill_not_counted");
    // a Broken Trade does NOT change the execution-event stats (#328 vs #320)
    n = OUCHMessage::encode_broken_trade(buf, "E1", 100, 77001, 'E');
    aes.on_response(OUCHMessage::parse_response(buf, n));
    ASSERT(aes.exec_count() == 2 && std::fabs(aes.avg_exec_shares() - 150.0) < 1e-9,
           "ouch_aes_break_unaffected");

    // #337 avg_order_size / executions_per_order — order sizing & fill fragmentation.
    ouch::OUCHOrderTracker frag;
    ASSERT(frag.avg_order_size() == 0.0 && frag.executions_per_order() == 0.0, "ouch_frag_empty");
    frag.on_new("F1", 100);
    frag.on_new("F2", 300);                                     // 2 orders, 400 ordered
    ASSERT(std::fabs(frag.avg_order_size() - 200.0) < 1e-9, "ouch_frag_avg_order_200");
    n = OUCHMessage::encode_accepted(buf, "F1", 'B', 100, "AAPL", 50.0, 78001);
    frag.on_response(OUCHMessage::parse_response(buf, n));
    n = OUCHMessage::encode_executed(buf, "F1", 40, 50.0, 1);   // F1 slice 1
    frag.on_response(OUCHMessage::parse_response(buf, n));
    n = OUCHMessage::encode_executed(buf, "F1", 60, 50.0, 2);   // F1 slice 2 (fills)
    frag.on_response(OUCHMessage::parse_response(buf, n));
    // 2 execution events across 2 tracked orders -> 1.0 executions/order
    ASSERT(std::fabs(frag.executions_per_order() - 1.0) < 1e-9, "ouch_frag_exec_per_order");
    // ordered total is unchanged by fills -> avg_order_size stays 200
    ASSERT(std::fabs(frag.avg_order_size() - 200.0) < 1e-9, "ouch_frag_avg_order_stable");

    // #345 reject_rate — per-order (not per-share) rejection rate.
    ouch::OUCHOrderTracker rjt;
    ASSERT(rjt.reject_rate() == 0.0, "ouch_rjt_empty");
    rjt.on_new("RR1", 100);
    rjt.on_new("RR2", 100);
    rjt.on_new("RR3", 100);
    n = OUCHMessage::encode_accepted(buf, "RR1", 'B', 100, "AAPL", 50.0, 79001);
    rjt.on_response(OUCHMessage::parse_response(buf, n));
    n = OUCHMessage::encode_rejected(buf, "RR2", 'X');
    rjt.on_response(OUCHMessage::parse_response(buf, n));
    // RR3 never gets a response -> stays NEW; 1 of 3 tracked orders rejected
    ASSERT(std::fabs(rjt.reject_rate() - 1.0/3.0) < 1e-9, "ouch_rjt_one_of_three");

    // #353 cancel_rate — per-order (not per-share) cancellation rate.
    ASSERT(rjt.cancel_rate() == 0.0, "ouch_rjt_cancel_rate_none_yet");
    n = OUCHMessage::encode_cancelled(buf, "RR1", 100);   // RR1 (LIVE) fully cancelled
    rjt.on_response(OUCHMessage::parse_response(buf, n));
    rjt.on_new("RR4", 100);
    n = OUCHMessage::encode_accepted(buf, "RR4", 'B', 100, "AAPL", 50.0, 79002);
    rjt.on_response(OUCHMessage::parse_response(buf, n));
    n = OUCHMessage::encode_cancelled(buf, "RR4", 100);
    rjt.on_response(OUCHMessage::parse_response(buf, n));
    // 4 tracked orders now (RR1..RR4): RR1 + RR4 cancelled -> 2/4 = 0.5
    ASSERT(rjt.order_count() == 4, "ouch_rjt_order_count_4");
    ASSERT(std::fabs(rjt.cancel_rate() - 0.5) < 1e-9, "ouch_rjt_cancel_rate_half");
    // #361 order_fill_rate — per-order completion (none of RR1..RR4 filled -> 0).
    ASSERT(rjt.order_fill_rate() == 0.0, "ouch_rjt_order_fill_rate_none");

    // #361 order_fill_rate (per-order) vs fill_rate (#250, per-share).
    ouch::OUCHOrderTracker ofr;
    ASSERT(ofr.order_fill_rate() == 0.0, "ouch_ofr_empty");
    ofr.on_new("F1", 100);   // will fully fill
    ofr.on_new("F2", 400);   // will half-fill (stays PARTIAL, not FILLED)
    n = OUCHMessage::encode_accepted(buf, "F1", 'B', 100, "AAPL", 50.0, 80101);
    ofr.on_response(OUCHMessage::parse_response(buf, n));
    n = OUCHMessage::encode_executed(buf, "F1", 100, 50.0, 1);   // F1 fully filled
    ofr.on_response(OUCHMessage::parse_response(buf, n));
    n = OUCHMessage::encode_accepted(buf, "F2", 'B', 400, "AAPL", 50.0, 80102);
    ofr.on_response(OUCHMessage::parse_response(buf, n));
    n = OUCHMessage::encode_executed(buf, "F2", 200, 50.0, 2);   // F2 half filled -> PARTIAL
    ofr.on_response(OUCHMessage::parse_response(buf, n));
    // 1 of 2 orders reached FILLED -> order_fill_rate 0.5...
    ASSERT(std::fabs(ofr.order_fill_rate() - 0.5) < 1e-9, "ouch_ofr_half_by_order");
    // ...but 300 of 500 shares filled -> fill_rate 0.6 (they disagree by design).
    ASSERT(std::fabs(ofr.fill_rate() - 0.6) < 1e-9, "ouch_ofr_vs_share_fill_rate");
}

// OUCH ↔ SoupBinTCP #78 — full round-trip login→order→accepted→executed.
void test_soupbin_ouch_session() {
    SECTION("SoupBin/OUCH Session (#78)");
    using namespace soupbin;

    // The client builds a stream: Login Request + Enter Order ('U' with OUCH 'O').
    uint8_t cstream[256];
    std::size_t coff = 0;
    coff += pack_login_request(cstream + coff, sizeof(cstream) - coff,
                               "USER1", "PASS", "", "0");
    uint8_t ouch[64];
    const int olen = OUCHMessage::enter_order(ouch, "TOK0001", 'B', 100, "AAPL", 150.25);
    coff += pack_data(cstream + coff, sizeof(cstream) - coff,
                      ouch, static_cast<std::size_t>(olen), /*client_side=*/true);

    // The mock exchange replies: A (login accepted) + S(Accepted) + S(Executed).
    uint8_t sstream[256];
    const std::size_t slen = mock_exchange_respond(sstream, sizeof(sstream),
                                                    cstream, coff, /*start_seq=*/1);
    ASSERT(slen > 0, "soup_exchange_responded");

    // The client consumes the whole TCP stream (3 packets at once).
    OuchSessionClient client;
    const std::size_t consumed = client.consume(sstream, slen);
    ASSERT(consumed == slen, "soup_client_consumed_all");
    ASSERT(client.logged_in(), "soup_client_logged_in");
    ASSERT(client.accepts() == 1, "soup_one_accepted");
    ASSERT(client.executes() == 1, "soup_one_executed");
    ASSERT(client.errors() == 0, "soup_no_errors");

    // Logout → End of Session.
    uint8_t lo[8]; const std::size_t lolen = pack_logout_request(lo);
    uint8_t resp[16];
    const std::size_t rlen = mock_exchange_respond(resp, sizeof(resp), lo, lolen);
    client.consume(resp, rlen);
    ASSERT(client.session_ended(), "soup_session_ended");

    // #145 wrappery klienta — pack_enter_order (OUCH 'O' w pakiecie 'U') + roundtrip.
    uint8_t opkt[64];
    const std::size_t oplen = pack_enter_order(opkt, sizeof(opkt), "TOK1", 'B', 100, "AAPL", 150.25);
    ASSERT(oplen > 0, "pack_enter_order_built");
    const ParsedPacket op = parse_packet(opkt, oplen);
    ASSERT(op.valid && op.type == PacketType::UNSEQUENCED_DATA && op.payload[0] == 'O',
           "pack_enter_order_is_U_O");
    uint8_t oresp[256];
    const std::size_t orlen = mock_exchange_respond(oresp, sizeof(oresp), opkt, oplen);
    OuchSessionClient oc;
    oc.consume(oresp, orlen);
    ASSERT(oc.accepts() == 1 && oc.executes() == 1, "pack_enter_order_roundtrip");

    // #139 Login Rejected — parse the rejection reason.
    OuchSessionClient cr;
    uint8_t jpkt[8];
    pack_header(jpkt, PacketType::LOGIN_REJECTED, 1);
    jpkt[HEADER_SIZE] = 'A';                         // 'A' = not authorized
    cr.consume(jpkt, HEADER_SIZE + 1);
    ASSERT(!cr.logged_in() && cr.login_reject_reason() == 'A', "soup_login_rejected_reason");

    // #234 session client rozpoznaje REJECTED i REPLACED w SEQUENCED_DATA.
    OuchSessionClient rc;
    uint8_t omsg[64], spkt[96];
    int ol = OUCHMessage::encode_rejected(omsg, "TOK1", 'X');         // J -> "REJECTED"
    pack_header(spkt, PacketType::SEQUENCED_DATA, static_cast<uint16_t>(ol));
    std::memcpy(spkt + HEADER_SIZE, omsg, static_cast<size_t>(ol));
    rc.consume(spkt, HEADER_SIZE + ol);
    ASSERT(rc.rejects() == 1 && rc.errors() == 0, "soup_rejected_counted");
    ol = OUCHMessage::encode_replaced(omsg, "NEW", "OLD", 80, 151.0, 4242);  // U -> "REPLACED"
    pack_header(spkt, PacketType::SEQUENCED_DATA, static_cast<uint16_t>(ol));
    std::memcpy(spkt + HEADER_SIZE, omsg, static_cast<size_t>(ol));
    rc.consume(spkt, HEADER_SIZE + ol);
    ASSERT(rc.replaces() == 1 && rc.errors() == 0, "soup_replaced_counted");

    // #434 legal-but-uncounted types no longer inflate errors().
    ASSERT(rc.others() == 0, "soup_others_starts_zero");
    ol = OUCHMessage::encode_broken_trade(omsg, "TOK1", 50, 4242, 'E');       // B -> "BROKEN"
    pack_header(spkt, PacketType::SEQUENCED_DATA, static_cast<uint16_t>(ol));
    std::memcpy(spkt + HEADER_SIZE, omsg, static_cast<size_t>(ol));
    rc.consume(spkt, HEADER_SIZE + ol);
    ASSERT(rc.others() == 1 && rc.errors() == 0, "soup_broken_is_other_not_error");
    ol = OUCHMessage::encode_cancel_pending(omsg, "TOK1");                    // P -> "CXL_PEND"
    pack_header(spkt, PacketType::SEQUENCED_DATA, static_cast<uint16_t>(ol));
    std::memcpy(spkt + HEADER_SIZE, omsg, static_cast<size_t>(ol));
    rc.consume(spkt, HEADER_SIZE + ol);
    ol = OUCHMessage::encode_restated(omsg, "TOK1", 60, 150.0, 'P');          // R -> "RESTATED"
    pack_header(spkt, PacketType::SEQUENCED_DATA, static_cast<uint16_t>(ol));
    std::memcpy(spkt + HEADER_SIZE, omsg, static_cast<size_t>(ol));
    rc.consume(spkt, HEADER_SIZE + ol);
    ASSERT(rc.others() == 3 && rc.errors() == 0, "soup_lifecycle_traffic_is_normal");
    // A genuinely unknown payload type is still an ERROR.
    omsg[0] = 'Z';
    pack_header(spkt, PacketType::SEQUENCED_DATA, 10);
    std::memcpy(spkt + HEADER_SIZE, omsg, 10);
    rc.consume(spkt, HEADER_SIZE + 10);
    ASSERT(rc.errors() == 1 && rc.others() == 3, "soup_unknown_still_error");

    // #118 HeartbeatTimer — bidirekcyjne heartbeaty SoupBin.
    soupbin::HeartbeatTimer hb;
    hb.on_tx(1000); hb.on_rx(1000);
    ASSERT(!hb.need_send(1500, 1000), "hb_not_due");          // 500 < 1000
    ASSERT(hb.need_send(2000, 1000), "hb_due");               // 1000 >= 1000
    ASSERT(!hb.peer_timed_out(5000, 15000), "hb_peer_alive"); // 4000 <= 15000
    ASSERT(hb.peer_timed_out(20000, 15000), "hb_peer_dead");  // 19000 > 15000
}


// =====================================================
// Negative / Edge-Case Tests
// =====================================================

void test_negative_cases() {
    SECTION("Negative Cases");

    // fill_order with non-existent order_id must not crash
    {
        OMS oms(1000, 200000.0);
        oms.fill_order(9999, 100, 150.0);  // should print WARNING but not crash
        ASSERT(oms.order_count() == 0u, "neg_fill_unknown_no_crash");
    }

    // OMS position limit (pending exposure): two submits without any fill — second blocked
    // by pending exposure from the first. Prevents overcommit when submits race ahead of fills.
    {
        OMS oms(100, 1000000.0);  // max_position=100
        Order* o = oms.submit_order("AAPL", Side::BUY, 10.0, 100);
        ASSERT(o != nullptr, "neg_pos_limit_first_ok");
        // Realized=0, pending=+100, next BUY 1 → exposure=101 > 100 → reject
        Order* o2 = oms.submit_order("AAPL", Side::BUY, 10.0, 1);
        ASSERT(o2 == nullptr, "neg_pos_limit_reject");
    }

    // Cancel releases pending exposure: after cancelling we should have capacity again.
    {
        OMS oms(100, 1000000.0);
        Order* o = oms.submit_order("AAPL", Side::BUY, 10.0, 100);
        ASSERT(o != nullptr, "neg_cancel_first_submit");
        if (!o) return;
        oms.cancel_order(o->order_id);
        // Pending now 0 — same-size order should fit again
        Order* o2 = oms.submit_order("AAPL", Side::BUY, 10.0, 100);
        ASSERT(o2 != nullptr, "neg_cancel_releases_pending");
    }

    // Partial fill: pending decreases by filled amount, remaining still counts toward limit.
    {
        OMS oms(100, 1000000.0);
        Order* o = oms.submit_order("AAPL", Side::BUY, 10.0, 100);
        ASSERT(o != nullptr, "neg_partial_first_submit");
        if (!o) return;
        oms.fill_order(o->order_id, 30, 10.0);  // realized=30, pending=70 (100-30 unfilled)
        // exposure for new BUY 1 = 30 + 70 + 1 = 101 → reject
        Order* o2 = oms.submit_order("AAPL", Side::BUY, 10.0, 1);
        ASSERT(o2 == nullptr, "neg_partial_pending_blocks");
    }

    // Risk: order at exact max_order_value boundary should be rejected
    {
        RiskLimits limits;
        limits.max_order_value = 10000;   // $10,000
        limits.max_position_per_symbol = 10000;
        limits.max_orders_per_second = 10000;
        RiskManager risk(limits);
        // 10001 × $1 = $10,001 > limit → reject
        auto r = risk.check_order("AAPL", Side::BUY, 1.0, 10001);
        ASSERT(r.action != RiskAction::ALLOW, "neg_risk_over_limit");
        // 9999 × $1 = $9,999 ≤ limit → accept
        auto r2 = risk.check_order("AAPL", Side::BUY, 1.0, 9999);
        ASSERT(r2.action == RiskAction::ALLOW, "neg_risk_under_limit");
    }

    // OUCH: parse nullptr data must return "ERROR" without crash
    {
        OUCHResponse resp = OUCHMessage::parse_response(nullptr, 0);
        ASSERT(strcmp(resp.type, "ERROR") == 0, "neg_ouch_null_data");
    }

    // OUCH: parse truncated Accepted message (< 41 bytes)
    {
        uint8_t short_buf[10] = {'A', 0};
        OUCHResponse resp = OUCHMessage::parse_response(short_buf, 10);
        ASSERT(strcmp(resp.type, "ERROR") == 0, "neg_ouch_short_accepted");
    }

    // OMS: reject order with zero quantity
    {
        OMS oms(1000, 1000000.0);
        Order* o = oms.submit_order("AAPL", Side::BUY, 150.0, 0);
        ASSERT(o == nullptr, "neg_oms_zero_qty");
    }

    // OMS: reject order with negative/zero price
    {
        OMS oms(1000, 1000000.0);
        Order* o = oms.submit_order("AAPL", Side::BUY, 0.0, 100);
        ASSERT(o == nullptr, "neg_oms_zero_price");
    }

    // FIX: input with no '|' or '=' delimiters → 0 fields, no crash
    {
        FIXMessage msg;
        msg.parse("ABCDEFGHIJK");
        ASSERT(msg.field_count() == 0, "neg_fix_no_delimiters");
    }

    // FIX: long input without null terminator within 1024 bytes → bounded scan
    {
        char raw[1500];
        std::memset(raw, '|', sizeof(raw) - 1);
        raw[sizeof(raw) - 1] = '\0';  // null only at index 1499
        FIXMessage msg;
        msg.parse(raw);  // memchr bounds the scan to 1024 — must not crash
        ASSERT(msg.field_count() == 0, "neg_fix_long_input_no_crash");
    }

    // OUCH: truncated Cancelled (< 20 bytes) → ERROR
    {
        uint8_t buf[10] = {'C', 0};
        OUCHResponse resp = OUCHMessage::parse_response(buf, 10);
        ASSERT(strcmp(resp.type, "ERROR") == 0, "neg_ouch_short_cancelled");
    }

    // OUCH: truncated Executed (< 31 bytes) → ERROR
    {
        uint8_t buf[20] = {'E', 0};
        OUCHResponse resp = OUCHMessage::parse_response(buf, 20);
        ASSERT(strcmp(resp.type, "ERROR") == 0, "neg_ouch_short_executed");
    }

    // OUCH: unknown message type → "UNKNOWN"
    {
        uint8_t buf[10] = {'Z', 0};
        OUCHResponse resp = OUCHMessage::parse_response(buf, 10);
        ASSERT(strcmp(resp.type, "UNKNOWN") == 0, "neg_ouch_unknown_type");
    }

    // ITCH: nullptr / zero-length must return ERROR, not crash
    {
        ITCHParser p;
        auto pm = p.parse(nullptr, 0);
        ASSERT(pm.type == MsgType::ERROR, "neg_itch_null_data");
    }

    // ITCH: oversized buffer for ADD_ORDER — parser only reads first 34 bytes, rest ignored
    {
        ITCHParser p;
        uint8_t buf[1000] = {'A'};  // type 'A', remaining bytes zero
        auto pm = p.parse(buf, sizeof(buf));
        ASSERT(pm.type == MsgType::ADD_ORDER, "neg_itch_oversized_buffer");
    }

    printf("  Negative: %d assertions\n", 16);
}


// =====================================================
// FlatOrderBook (occupancy-bitmap cursors) Tests
// =====================================================

void test_flat_orderbook() {
    SECTION("FlatOrderBook bitmap");

    using FB = orderbook::FlatOrderBook<16384>;

    // Basic add / best-cursor tracking / cross
    FB fob;
    ASSERT(fob.empty(), "flatob_starts_empty");
    fob.add_buy(10050, 10);
    fob.add_buy(10030, 5);
    fob.add_sell(10080, 12);
    ASSERT(fob.best_bid() == 10050, "flatob_best_bid_tracked");
    ASSERT(fob.best_ask() == 10080, "flatob_best_ask_tracked");
    ASSERT(fob.trades() == 0, "flatob_no_cross_yet");
    fob.add_buy(10080, 15);  // crosses the 12-lot ask, 3 rest as bid
    ASSERT(fob.trades() == 1, "flatob_cross_trades_once");
    ASSERT(fob.bid_qty_at(10080) == 3, "flatob_taker_remainder_rests");
    ASSERT(fob.best_ask() == FB::NO_ASK, "flatob_ask_side_empty_after_fill");
    ASSERT(fob.best_bid() == 10080, "flatob_best_bid_is_remainder");

    // Sparse book: emptying the best must jump far down in one scan
    FB fsp;
    fsp.add_buy(40, 5);       // deep bid, bitmap word 0
    fsp.add_buy(10000, 7);    // best bid, bitmap word 156
    fsp.add_sell(10000, 7);   // exact fill of the best level
    ASSERT(fsp.trades() == 1, "flatob_sparse_fill_trades");
    ASSERT(fsp.best_bid() == 40, "flatob_best_jumps_down_155_words");
    fsp.add_sell(40, 5);
    ASSERT(fsp.best_bid() == FB::NO_BID, "flatob_bid_side_fully_empty");
    ASSERT(fsp.trades() == 2, "flatob_deep_bid_filled_too");

    // 64-bit word boundary: levels 63 (word 0, bit 63) and 64 (word 1, bit 0)
    FB fwb;
    fwb.add_sell(64, 4);
    fwb.add_sell(63, 4);
    ASSERT(fwb.best_ask() == 63, "flatob_word_boundary_best_ask");
    fwb.add_buy(63, 4);  // fills level 63 exactly
    ASSERT(fwb.best_ask() == 64, "flatob_advance_crosses_word_boundary");

    // Extreme levels: 0 and LEVELS-1
    FB fex;
    fex.add_buy(0, 1);
    fex.add_sell(16383, 1);
    ASSERT(fex.best_bid() == 0 && fex.best_ask() == 16383, "flatob_extreme_levels_rest");
    fex.add_sell(0, 1);  // fills the level-0 bid
    ASSERT(fex.trades() == 1 && fex.best_bid() == FB::NO_BID, "flatob_level0_fill_empties_bids");

    // Cancel/modify path rescans via the bitmap
    FB fcx;
    fcx.submit_with_id(1, 8000, 10, /*is_buy=*/true);
    fcx.submit_with_id(2, 100, 10, /*is_buy=*/true);
    fcx.submit_with_id(3, 9000, 10, /*is_buy=*/false);
    ASSERT(fcx.best_bid() == 8000, "flatob_id_tracked_best_bid");
    ASSERT(fcx.cancel(1), "flatob_cancel_best_bid_ok");
    ASSERT(fcx.best_bid() == 100, "flatob_cancel_rescans_to_deep_bid");
    ASSERT(fcx.modify(3, 8500, 10), "flatob_modify_ask_ok");
    ASSERT(fcx.best_ask() == 8500, "flatob_modified_ask_repriced");
    fcx.submit_with_id(4, 8000, 6, /*is_buy=*/true);  // re-occupy an emptied level
    ASSERT(fcx.best_bid() == 8000, "flatob_readd_on_emptied_level");

    // Randomized equivalence vs a std::map reference book on one stream.
    // Any divergence in trades/best/level quantities is a matching bug.
    {
        std::map<int, int, std::greater<int>> refb;
        std::map<int, int> refa;
        unsigned long long reft = 0;
        static FB feq;  // 132 KB of arrays — keep off the stack
        unsigned int lcg = 0xC0FFEEu;
        auto nxt = [&lcg]() { lcg = lcg * 1664525u + 1013904223u; return lcg; };
        for (int i = 0; i < 20000; ++i) {
            const int px = 9000 + static_cast<int>(nxt() % 401u);
            const int q  = 1 + static_cast<int>(nxt() % 50u);
            // Side from a HIGH bit: an LCG's bit 0 alternates with period 2,
            // which with an even number of draws per loop pins the side.
            const bool buy = ((nxt() >> 16) & 1u) != 0;
            if (buy) { feq.add_buy(px, q); refb[px] += q; }
            else     { feq.add_sell(px, q); refa[px] += q; }
            while (!refb.empty() && !refa.empty()) {
                auto bb = refb.begin();
                auto ba = refa.begin();
                if (bb->first < ba->first) break;
                const int fill = std::min(bb->second, ba->second);
                ++reft;
                bb->second -= fill;
                ba->second -= fill;
                if (bb->second == 0) refb.erase(bb);
                if (ba->second == 0) refa.erase(ba);
            }
        }
        ASSERT(feq.trades() == reft, "flatob_trades_match_map_ref_20k_ops");
        const int refbb = refb.empty() ? static_cast<int>(FB::NO_BID) : refb.begin()->first;
        const int refba = refa.empty() ? static_cast<int>(FB::NO_ASK) : refa.begin()->first;
        ASSERT(feq.best_bid() == refbb, "flatob_best_bid_matches_map_ref");
        ASSERT(feq.best_ask() == refba, "flatob_best_ask_matches_map_ref");
        bool level_qty_ok = true;
        for (int p = 9000; p <= 9400; ++p) {
            const auto itb = refb.find(p);
            if (feq.bid_qty_at(p) != (itb == refb.end() ? 0 : itb->second)) { level_qty_ok = false; break; }
            const auto ita = refa.find(p);
            if (feq.ask_qty_at(p) != (ita == refa.end() ? 0 : ita->second)) { level_qty_ok = false; break; }
        }
        ASSERT(level_qty_ok, "flatob_level_qty_matches_map_ref_full_band");
    }
}


// =====================================================
// Main
// =====================================================

int main() {
    printf("=== HFT Infrastructure Lab — Integration Tests ===\n");

    test_itch_parser();
    test_itch_book();
    test_flat_orderbook();
    test_multicast_gap_recovery();
    test_oms();
    test_oms_short_and_replace();
    test_risk();
    test_risk_price_band();
    test_backtester();
    test_router();
    test_router_ewma_partial();
    test_logger();
    test_strategy();
    test_strategy_edge_cases();
    test_momentum();
    test_bollinger();
    test_donchian();
    test_rsi();
    test_ma_crossover();
    test_volatility();
    test_ema();
    test_macd();
    test_stochastic();
    test_wma();
    test_hull_ma();
    test_dema();
    test_tema();
    test_trix();
    test_cci();
    test_bollinger_pctb();
    test_roc();
    test_aroon();
    test_coppock();
    test_obv();
    test_volume_oscillator();
    test_pvt();
    test_ppo();
    test_force_index();
    test_mfi();
    test_vwma();
    test_nvi_pvi();
    test_close_atr();
    test_keltner();
    test_zlema();
    test_chandelier();
    test_awesome();
    test_accel_decel();
    test_rolling_median();
    test_supertrend();
    test_ultimate();
    test_choppiness();
    test_percent_rank();
    test_cfo();
    test_pgo();
    test_kst();
    test_vidya();
    test_cog();
    test_cmo();
    test_zscore();
    test_tsi();
    test_dpo();
    test_kama();
    test_linreg();
    test_rolling_stddev();
    test_fisher();
    test_backtest();
    test_ensemble();
    test_trailing_stop();
    test_pov_algo();
    test_signal_throttle();
    test_vwap_tracker();
    test_market_maker();
    test_fix();
    test_fix_session();
    test_fix_order_state();
    test_ouch();
    test_ouch_order_state();
    test_soupbin_ouch_session();
    test_fill_simulator();
    test_spsc_queue();
    test_mpsc_queue();
    test_mpmc_queue();
    test_sequencer();
    test_waitable_mpsc();
    test_varlen_ring();
    test_logger_async_flush();
    test_lockfree_logger();
    test_mmap_logger();
    test_risk_rate_limiter();
    test_risk_drawdown_no_peak();
    test_protocols_interleaved();
    test_lobster_reader();
    test_simulator();
    test_integration();
    test_negative_cases();

    printf("\n========================================\n");
    printf("  %d/%d tests passed", tests_passed, tests_total);
    if (tests_failed > 0)
        printf("  (%d FAILED)", tests_failed);
    printf("\n========================================\n");

    return (tests_failed == 0) ? 0 : 1;
}