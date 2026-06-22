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

// All module headers
#include "../itch-parser/itch_parser.hpp"
#include "../itch-parser/itch_book.hpp"
#include "../multicast/gap_recovery.hpp"
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


// OMS #77 — P&L pozycji krótkich, flip long↔short, replace_order (amend).
void test_oms_short_and_replace() {
    SECTION("OMS Short P&L + Replace");
    auto close = [](double a, double b) { const double d = a - b; return (d < 0 ? -d : d) < 0.01; };

    // --- Short P&L: sprzedaj wysoko, odkup nisko → zysk ---
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

    // --- Flip long→short jednym fillem + dokończenie covera ---
    {
        OMS oms(10000, 100000000.0);
        oms.fill_order((oms.submit_order("X", Side::BUY, 10.00, 100))->order_id, 100, 10.00);
        // SELL 150 @ $12: zamknij 100 long (realize $200) i flip do short 50 @ $12
        oms.fill_order((oms.submit_order("X", Side::SELL, 12.00, 150))->order_id, 150, 12.00);
        const Position* p = oms.get_position("X");
        ASSERT(p->net_qty == -50, "flip_to_short_50");
        ASSERT(close(to_float(p->avg_price), 12.00), "flip_avg_is_fill_px");
        ASSERT(close(to_float(p->realized_pnl), 200.0), "flip_realized_200");
        // Cover short 50 @ $11 → +$50 → razem $250
        oms.fill_order((oms.submit_order("X", Side::BUY, 11.00, 50))->order_id, 50, 11.00);
        p = oms.get_position("X");
        ASSERT(p->net_qty == 0, "flip_fully_closed");
        ASSERT(close(to_float(p->realized_pnl), 250.0), "flip_total_realized_250");
    }

    // --- replace_order: amend ceny/ilości ---
    {
        OMS oms(1000, 1000000.0);
        Order* o = oms.submit_order("AAPL", Side::BUY, 100.00, 50);
        ASSERT(o != nullptr, "replace_submit_ok");
        if (!o) return;
        ASSERT(oms.replace_order(o->order_id, 101.00, 80), "replace_applied");
        const Order* r = oms.get_order(o->order_id);
        ASSERT(r->quantity == 80 && close(to_float(r->price), 101.00), "replace_new_px_qty");
    }
    {   // amend ponad limit pozycji → odrzucony, zlecenie bez zmian
        OMS oms(100, 1000000.0);
        Order* a = oms.submit_order("AAPL", Side::BUY, 10.00, 50);
        ASSERT(!oms.replace_order(a->order_id, 10.00, 200), "replace_rejects_over_limit");
        ASSERT(oms.get_order(a->order_id)->quantity == 50, "replace_unchanged_on_reject");
    }

    {   // #83 prowizje: brutto vs netto. $0.01/akcja, round-trip 100 akcji.
        OMS oms(10000, 100000000.0, /*commission_per_share=*/0.01);
        oms.fill_order(oms.submit_order("AAPL", Side::BUY,  10.00, 100)->order_id, 100, 10.00);
        oms.fill_order(oms.submit_order("AAPL", Side::SELL, 12.00, 100)->order_id, 100, 12.00);
        const Position* p = oms.get_position("AAPL");
        ASSERT(close(to_float(p->realized_pnl), 200.0), "fee_gross_pnl_200");   // (12-10)*100
        ASSERT(close(to_float(p->fees), 2.0), "fee_total_2");                   // 200 akcji * $0.01
        ASSERT(close(to_float(p->net_pnl()), 198.0), "fee_net_pnl_198");
        ASSERT(close(to_float(oms.total_fees()), 2.0), "fee_oms_total_2");
    }

    {   // #100 cancel_all / cancel_all_symbol — risk-off masowe anulowanie.
        OMS oms(1000000, 1000000000.0);
        oms.submit_order("AAA", Side::BUY,  10.00, 100);   // SENT
        oms.submit_order("BBB", Side::SELL, 20.00, 50);    // SENT
        Order* f = oms.submit_order("AAA", Side::BUY, 10.00, 100);
        oms.fill_order(f->order_id, 100, 10.00);           // FILLED — nie anulowalne
        ASSERT(oms.cancel_all() == 2, "cancelall_two_open");
        ASSERT(oms.get_position("AAA")->pending_qty == 0, "cancelall_releases_pending");
        // per-symbol
        OMS o2(1000000, 1000000000.0);
        o2.submit_order("AAA", Side::BUY, 10.00, 100);
        o2.submit_order("BBB", Side::BUY, 10.00, 100);
        ASSERT(o2.cancel_all_symbol("AAA") == 1, "cancelall_symbol_one");
    }

    {   // #172 wygasanie GTD: purge_expired anuluje wygasle, zostawia bez expiry.
        OMS oms(100000, 1000000000.0);
        OMSReject why = OMSReject::NONE;
        Order* g = oms.submit_order("AAA", Side::BUY, 10.0, 100, &why, /*expire_ns=*/5000);
        Order* d = oms.submit_order("BBB", Side::BUY, 10.0, 100);   // bez wygasniecia
        ASSERT(oms.purge_expired(6000) == 1, "gtd_purged_one");     // 5000 <= 6000
        ASSERT(oms.get_order(g->order_id)->status == OrderStatus::CANCELLED, "gtd_expired_cancelled");
        ASSERT(oms.get_order(d->order_id)->status == OrderStatus::SENT, "gtd_no_expiry_kept");
    }

    {   // #180 open_order_notional: kapital w pracujacych zleceniach (reszta).
        OMS oms(100000, 1000000000.0);
        Order* a = oms.submit_order("AAA", Side::BUY, 10.0, 100);   // notional 1000
        Order* b = oms.submit_order("BBB", Side::SELL, 20.0, 50);   // notional 1000
        oms.fill_order(a->order_id, 40, 10.0);                      // reszta 60 -> 600
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
        ASSERT(!oms.is_flat(), "flat_false_working_order");          // pracujace zlecenie
        oms.fill_order(o->order_id, 100, 10.0);                      // pozycja 100
        ASSERT(oms.open_position_count() == 1 && !oms.is_flat(), "flat_false_open_position");
        Order* s = oms.submit_order("AAA", Side::SELL, 11.0, 100);
        oms.fill_order(s->order_id, 100, 11.0);                      // netto 0
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

    {   // #204 reset_session_counters (zeruje statystyki, zostawia pozycje/zlecenia).
        OMS oms(100000, 1000000000.0);
        Order* o = oms.submit_order("AAA", Side::BUY, 10.0, 100);
        oms.fill_order(o->order_id, 50, 10.0);                   // fills 1, PARTIAL
        ASSERT(oms.total_submitted() == 1 && oms.total_fills() == 1, "rsc_counters_before");
        oms.reset_session_counters();
        ASSERT(oms.total_submitted() == 0 && oms.total_fills() == 0
               && oms.total_cancels() == 0 && oms.total_replaces() == 0, "rsc_counters_zeroed");
        ASSERT(oms.order_count() >= 1 && oms.open_position_count() == 1, "rsc_state_kept");
    }

    {   // #212 submit_reject_rate (maly max_order_value -> czesc odrzucona).
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
    }

    {   // #228 fill_ratio (wykonane / zlecone akcje).
        OMS oms(1000000, 1000000000.0);
        Order* a = oms.submit_order("AAA", Side::BUY, 10.0, 100);   // zlecone 100
        oms.fill_order(a->order_id, 60, 10.0);                      // wykonane 60
        Order* b = oms.submit_order("BBB", Side::BUY, 10.0, 100);   // zlecone razem 200
        oms.fill_order(b->order_id, 100, 10.0);                     // wykonane razem 160
        ASSERT(oms.total_ordered_shares() == 200 && oms.total_filled_shares() == 160,
               "fillratio_accumulators");
        ASSERT(close(oms.fill_ratio(), 0.8), "fillratio_080");      // 160/200
    }

    {   // #236 avg_commission_per_share (prowizja 0.01/akcja).
        OMS oms(1000000, 1000000000.0, /*commission_per_share=*/0.01);
        Order* a = oms.submit_order("AAA", Side::BUY, 10.0, 100);
        oms.fill_order(a->order_id, 100, 10.0);                     // fee 1.0, filled 100
        ASSERT(close(to_float(oms.total_fees()), 1.0), "avgcomm_total_fee");
        ASSERT(close(oms.avg_commission_per_share(), 0.01), "avgcomm_per_share"); // 1.0/100
        OMS empt(1000000, 1000000000.0);
        ASSERT(empt.avg_commission_per_share() == 0.0, "avgcomm_empty_zero");
    }

    {   // #244 winning_symbols / losing_symbols (atrybucja P&L).
        OMS oms(1000000, 1000000000.0);
        // A: long 100@10, zamknij @12 -> +200; B: @9 -> -100; C: @10 -> 0
        Order* a1 = oms.submit_order("AAA", Side::BUY, 10.0, 100); oms.fill_order(a1->order_id, 100, 10.0);
        Order* a2 = oms.submit_order("AAA", Side::SELL, 12.0, 100); oms.fill_order(a2->order_id, 100, 12.0);
        Order* b1 = oms.submit_order("BBB", Side::BUY, 10.0, 100); oms.fill_order(b1->order_id, 100, 10.0);
        Order* b2 = oms.submit_order("BBB", Side::SELL, 9.0, 100);  oms.fill_order(b2->order_id, 100, 9.0);
        Order* c1 = oms.submit_order("CCC", Side::BUY, 10.0, 100); oms.fill_order(c1->order_id, 100, 10.0);
        Order* c2 = oms.submit_order("CCC", Side::SELL, 10.0, 100); oms.fill_order(c2->order_id, 100, 10.0);
        ASSERT(oms.winning_symbols() == 1, "winsym_one");   // AAA
        ASSERT(oms.losing_symbols() == 1, "losesym_one");   // BBB (CCC = 0, pominiety)
        // #298 symbol_win_rate — 1 winner / (1 winner + 1 loser) = 0.5 (CCC flat excluded).
        ASSERT(close(oms.symbol_win_rate(), 0.5), "symwinrate_half");
        OMS empt(1000000, 1000000000.0);
        ASSERT(empt.symbol_win_rate() == 0.0, "symwinrate_empty_zero");
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

    {   // #274 avg_fill_size (shares per fill).
        OMS oms(1000000, 1000000000.0);
        Order* a = oms.submit_order("AAA", Side::BUY, 10.0, 100);
        oms.fill_order(a->order_id, 40, 10.0);                     // fill 1: 40
        oms.fill_order(a->order_id, 60, 10.0);                     // fill 2: 60
        ASSERT(oms.total_fills() == 2 && oms.total_filled_shares() == 100, "avgfill_accum");
        ASSERT(close(oms.avg_fill_size(), 50.0), "avgfill_50");     // 100 / 2
    }

    {   // #166 runtime zmiana prowizji.
        OMS oms(100000, 1000000000.0, /*commission_per_share=*/0.005);
        ASSERT(close(oms.commission_per_share(), 0.005), "comm_initial");
        oms.set_commission(0.01);
        ASSERT(close(oms.commission_per_share(), 0.01), "comm_updated");
        Order* o = oms.submit_order("AAA", Side::BUY, 10.0, 100);
        oms.fill_order(o->order_id, 100, 10.0);          // fee 100 * 0.01 = 1.0
        ASSERT(close(to_float(oms.total_fees()), 1.0), "comm_new_rate_applied");
    }

    {   // #151 liczniki operacji cyklu zycia (fills/cancels/replaces).
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

    {   // #141 avg_fill_price — zlecenie wypelniane po dwoch cenach.
        OMS oms(100000, 1000000000.0);
        Order* o = oms.submit_order("AAPL", Side::BUY, 100.00, 100);
        oms.fill_order(o->order_id, 40, 100.00);                  // 40 @ 100.00
        oms.fill_order(o->order_id, 60, 101.00);                  // 60 @ 101.00
        const Order* r = oms.get_order(o->order_id);
        // avg = (40*100 + 60*101)/100 = 100.60
        ASSERT(close(to_float(r->avg_fill_price()), 100.60), "avg_fill_price_blended");
    }

    {   // #128 count_by_status — observability stanu zlecen.
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

    {   // #120 agregaty P&L portfela (realized/net po wszystkich pozycjach).
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

    {   // #96 unrealized P&L (mark-to-market) dla long i short.
        OMS oms(10000, 100000000.0);
        oms.fill_order(oms.submit_order("AAPL", Side::BUY, 50.00, 100)->order_id, 100, 50.00);
        const Position* lp = oms.get_position("AAPL");
        ASSERT(close(to_float(lp->unrealized_pnl(to_fixed(52.00))), 200.0), "mtm_long_up");   // (52-50)*100
        ASSERT(close(to_float(lp->unrealized_pnl(to_fixed(48.00))), -200.0), "mtm_long_down");
        OMS oms2(10000, 100000000.0);
        oms2.fill_order(oms2.submit_order("AAPL", Side::SELL, 50.00, 100)->order_id, 100, 50.00);
        const Position* sp = oms2.get_position("AAPL");
        ASSERT(close(to_float(sp->unrealized_pnl(to_fixed(48.00))), 200.0), "mtm_short_profit"); // short zyskuje gdy spada
        ASSERT(close(to_float(sp->total_pnl(to_fixed(48.00))), 200.0), "mtm_total_short");       // realized 0 + unrl 200
    }

    {   // #88 reject reasons — caller rozróżnia DLACZEGO odrzucono.
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
        // #136 statystyki odrzucen per powod (powyzej: 1x kazdy typ).
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
    // #125 reject reason: brak venue vs brak plynnosci
    ASSERT(rd.reject_reason == RouteReject::NO_VENUES, "router_reject_no_venues");
    SmartOrderRouter dry(RoutingStrategy::BEST_PRICE);
    dry.add_venue(Venue("A", 100, 0.0));         // venue jest, ale bez quote
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

    // 1000 prób → niezerowy reject rate, partial rate i slippage.
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

    // Aggressive urgency → większy slippage od passive (statystycznie).
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

    book.on_execute(999, 10);                     // nieznany ref → orphan
    ASSERT(book.orphans() == 1, "itchbook_orphan_counted");

    // #87 mikrostruktura: mid + top-of-book imbalance.
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

    // #95 pre-trade impact: walk księgi → VWAP marketowego zlecenia.
    itch::ITCHOrderBook fb;
    fb.on_add(1, 'S', 100.00, 100);
    fb.on_add(2, 'S', 100.01, 200);
    fb.on_add(3, 'S', 100.02, 300);
    double v = 0.0;
    int64_t f = fb.expected_fill('B', 250, v);                  // 100@100.00 + 150@100.01
    ASSERT(f == 250, "itchbook_fill_qty_250");
    ASSERT(close(v, 100.006), "itchbook_fill_vwap");            // (100*100.00+150*100.01)/250
    int64_t f2 = fb.expected_fill('B', 1000, v);                // tylko 600 płynności
    ASSERT(f2 == 600, "itchbook_fill_partial_600");

    // #123 top_levels: top N poziomow po stronie.
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

    // #155 vwap_depth: VWAP top-N poziomow.
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

    // #164 liquidity_within (N ticks od best).
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

    // #174 total_shares + level_count (rozmiar i grubosc ksiazki).
    // #223 fillable_shares (do ceny limit).
    // lw aski: 100.00(100), 100.01(200), 100.05(300)
    ASSERT(lw.fillable_shares('B', 100.01) == 300, "itchbook_fillable_buy_mid");  // <=100.01
    ASSERT(lw.fillable_shares('B', 100.05) == 600, "itchbook_fillable_buy_all");
    ASSERT(lw.fillable_shares('B', 99.99) == 0, "itchbook_fillable_buy_none");    // < best ask
    itch::ITCHOrderBook fs;                                                       // osobna ksiega na bidy
    fs.on_add(10, 'B', 99.99, 150); fs.on_add(11, 'B', 99.98, 250);
    ASSERT(fs.fillable_shares('S', 99.98) == 400, "itchbook_fillable_sell_both"); // >=99.98
    ASSERT(fs.fillable_shares('S', 99.99) == 150, "itchbook_fillable_sell_top");  // >=99.99

    // #247 price_to_fill (najgorszy poziom do sweep'a).
    itch::ITCHOrderBook pf;
    pf.on_add(1, 'S', 100.00, 100); pf.on_add(2, 'S', 100.02, 200); pf.on_add(3, 'S', 100.05, 300);
    ASSERT(close(pf.price_to_fill('B', 100), 100.00), "itchbook_ptf_buy_top");   // 100 @ best
    ASSERT(close(pf.price_to_fill('B', 250), 100.02), "itchbook_ptf_buy_mid");   // 100+150 -> 2gi poziom
    ASSERT(pf.price_to_fill('B', 700) == 0.0, "itchbook_ptf_insufficient");      // > 600 dostepne
    pf.on_add(4, 'B', 99.99, 150); pf.on_add(5, 'B', 99.98, 250);
    ASSERT(close(pf.price_to_fill('S', 200), 99.98), "itchbook_ptf_sell");       // 150+50 -> 99.98

    ASSERT(lw.total_shares('S') == 600, "itchbook_total_shares_ask");     // 100+200+300
    ASSERT(lw.level_count('S') == 3, "itchbook_level_count_ask");
    ASSERT(lw.total_shares('B') == 0 && lw.level_count('B') == 0, "itchbook_empty_bid_side");

    // #191 total_notional ($ glebokosci).
    itch::ITCHOrderBook tn;
    tn.on_add(1, 'S', 100.00, 100);   // 10000.00
    tn.on_add(2, 'S', 100.02, 200);   // 20004.00
    ASSERT(close(tn.total_notional('S'), 30004.0), "itchbook_total_notional_ask");
    ASSERT(tn.total_notional('B') == 0.0, "itchbook_total_notional_empty_bid");

    // #199 slippage_bps (koszt egzekucji vs mid).
    itch::ITCHOrderBook sl;
    sl.on_add(1, 'B', 99.98, 100);                          // best bid
    sl.on_add(2, 'S', 100.00, 100); sl.on_add(3, 'S', 100.02, 200);  // asks
    // mid = 99.99; BUY 100 -> vwap 100.00 -> (100.00-99.99)/99.99*10000 ~ 1.0001 bps
    ASSERT(close(sl.slippage_bps('B', 100), 1.00010001), "itchbook_slippage_buy_100");
    // wieksze zlecenie zjada glebszy poziom -> wieksza slippage
    ASSERT(sl.slippage_bps('B', 200) > sl.slippage_bps('B', 100), "itchbook_slippage_monotone");
    // SELL 50 wchodzi w bid 99.98 -> (99.99-99.98)/99.99*10000 ~ 1.0001 bps
    ASSERT(close(sl.slippage_bps('S', 50), 1.00010001), "itchbook_slippage_sell_50");
    itch::ITCHOrderBook nlq;                                // ksiega jednostronna -> 0
    nlq.on_add(1, 'B', 99.98, 100);
    ASSERT(nlq.slippage_bps('B', 100) == 0.0, "itchbook_slippage_no_ask_liq");

    // #231 round_trip_cost_bps (kup+sprzedaj).
    // sl: bid 99.98 / ask 100.00, mid 99.99; buy 100 slip ~1bps, sell 50 slip ~1bps
    ASSERT(close(sl.round_trip_cost_bps(100), 2.0 * (0.01/99.99*10000.0)),
           "itchbook_round_trip_cost");
    ASSERT(nlq.round_trip_cost_bps(100) == 0.0, "itchbook_round_trip_onesided");

    // #207 spread_ticks (calkowita liczba ticow $0.01).
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

    // #215 notional_imbalance (wazony wartoscia, rozny od depth_imbalance).
    itch::ITCHOrderBook ni;
    ni.on_add(1, 'B', 50.00, 200);   // bid $: 50*200 = 10000, 200 szt.
    ni.on_add(2, 'S', 100.00, 150);  // ask $: 100*150 = 15000, 150 szt.
    // notional: (10000-15000)/25000 = -0.2 ; depth (szt): (200-150)/350 = +0.1428
    ASSERT(close(ni.notional_imbalance(1), -0.2), "itchbook_notional_imb");
    ASSERT(ni.depth_imbalance(1) > 0.0, "itchbook_depth_imb_differs_sign");  // przeciwny znak

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

    sb.clear();
    ASSERT(sb.resting_orders() == 0 && sb.best_bid() == 0.0, "itchbook_clear_resets");
}

// Multicast gap-recovery #82 — detekcja luk + retransmisja + rekoncyliacja.
void test_multicast_gap_recovery() {
    SECTION("Multicast Gap Recovery (#82)");
    multicast::GapRecovery gr;
    gr.observe(1); gr.observe(2);
    gr.observe(5);                                // luka: brak 3,4
    ASSERT(gr.has_gaps() && gr.missing_count() == 2, "gaprec_two_missing");
    uint64_t lo = 0, hi = 0;
    ASSERT(gr.next_request(lo, hi) && lo == 3 && hi == 4, "gaprec_request_range");

    ASSERT(gr.on_retransmit(3), "gaprec_recover_3");
    ASSERT(gr.on_retransmit(4), "gaprec_recover_4");
    ASSERT(!gr.has_gaps(), "gaprec_fully_recovered");
    ASSERT(gr.recovered == 2, "gaprec_recovered_count");
    ASSERT(!gr.on_retransmit(99), "gaprec_unknown_retransmit_false");

    gr.observe(8);                               // luka: brak 6,7
    gr.observe(6);                               // spóźniony primary wypełnia 6
    ASSERT(gr.missing_count() == 1 && gr.recovered == 3, "gaprec_late_primary_recovers");

    // #149 lista zakresow luk (ciagle przedzialy).
    multicast::GapRecovery mr2;
    mr2.observe(1); mr2.observe(5);                      // brak 2,3,4 -> [2,4]
    mr2.observe(10);                                     // brak 6,7,8,9 -> [6,9]
    const auto rngs = mr2.missing_ranges();
    ASSERT(rngs.size() == 2, "gaprec_two_ranges");
    ASSERT(rngs[0].first == 2 && rngs[0].second == 4, "gaprec_range_2_4");
    ASSERT(rngs[1].first == 6 && rngs[1].second == 9, "gaprec_range_6_9");

    // #156 recovery_completeness.
    multicast::GapRecovery rc2;
    ASSERT(std::fabs(rc2.recovery_completeness() - 1.0) < 1e-9, "gaprec_complete_when_empty");
    rc2.observe(1); rc2.observe(4);                     // brak 2,3
    ASSERT(std::fabs(rc2.recovery_completeness() - 0.0) < 1e-9, "gaprec_complete_0");
    rc2.on_retransmit(2);                               // recovered 1, missing 1
    ASSERT(std::fabs(rc2.recovery_completeness() - 0.5) < 1e-9, "gaprec_complete_half");
    rc2.on_retransmit(3);                               // recovered 2, missing 0
    ASSERT(std::fabs(rc2.recovery_completeness() - 1.0) < 1e-9, "gaprec_complete_full");

    // #110 ReorderBuffer — dostarcza zawsze w kolejnosci, trzyma "przyszle".
    multicast::ReorderBuffer<int> rb;
    rb.push(1, 10);                              // expected=1 -> dostarcz, expected->2
    ASSERT(rb.out.size() == 1 && rb.out[0] == 10, "reorder_inorder_deliver");
    rb.push(3, 30);                              // luka przy 2 -> buforuj
    rb.push(4, 40);                              // buforuj
    ASSERT(rb.out.size() == 1 && rb.buffered() == 2, "reorder_holds_future");
    rb.push(2, 20);                              // wypelnia luke -> drain 2,3,4
    ASSERT(rb.buffered() == 0, "reorder_drained");
    ASSERT(rb.out.size() == 4 && rb.out[1] == 20 && rb.out[2] == 30 && rb.out[3] == 40,
           "reorder_delivered_in_order");
    rb.push(2, 99);                              // < expected -> duplikat
    ASSERT(rb.duplicates == 1 && rb.out.size() == 4, "reorder_drops_duplicate");

    // #115 snapshot vs retransmisja: duza luka -> snapshot resync.
    multicast::GapRecovery sr;
    sr.observe(1); sr.observe(10);               // brak 2..9 (8 pakietow)
    ASSERT(sr.missing_count() == 8, "snap_big_gap");
    ASSERT(sr.recommend_snapshot(5), "snap_recommend_over_threshold");   // 8>=5
    ASSERT(!sr.recommend_snapshot(20), "snap_no_recommend_under");
    sr.snapshot_resync(10);                      // snapshot pokrywa do seq 10
    ASSERT(!sr.has_gaps() && sr.expected == 11, "snap_resync_clears_gaps");

    // #122 MultiChannelRecovery — agregacja po kanalach feedu.
    multicast::MultiChannelRecovery mc;
    mc.observe(1, 1); mc.observe(1, 3);          // kanal 1: luka (brak 2)
    mc.observe(2, 5); mc.observe(2, 6);          // kanal 2: w kolejnosci
    ASSERT(mc.channel_count() == 2, "mcr_two_channels");
    ASSERT(mc.any_gaps() && mc.total_missing() == 1, "mcr_gap_in_ch1");
    ASSERT(mc.on_retransmit(1, 2), "mcr_recover_ch1");
    ASSERT(!mc.any_gaps() && mc.total_recovered() == 1, "mcr_all_recovered");

    // #132 FeedRateMeter — sliding-window rate.
    multicast::FeedRateMeter fr(1000);                    // okno 1000 ns
    fr.on_message(100); fr.on_message(200); fr.on_message(300);
    ASSERT(fr.count(300) == 3, "rate_3_in_window");
    ASSERT(std::fabs(fr.rate_per_sec(300) - 3e6) < 1.0, "rate_per_sec_3M");  // 3*1e9/1000
    ASSERT(fr.count(1301) == 0, "rate_window_expired");   // wszystkie starsze niz okno

    // #163 peak rate (burst).
    multicast::FeedRateMeter pm(1000);
    pm.on_message(0); pm.on_message(100); pm.on_message(200);  // 3 w oknie -> peak 3
    ASSERT(pm.peak_count() == 3, "rate_peak_3");
    pm.on_message(1300);                                       // stare wyrzucone, count 1
    ASSERT(pm.count(1300) == 1 && pm.peak_count() == 3, "rate_peak_holds");
    ASSERT(std::fabs(pm.peak_rate_per_sec() - 3e6) < 1.0, "rate_peak_rate_3M");

    // #171 DedupWindow — at-most-once (odrzuca duplikaty).
    multicast::DedupWindow dw(100);
    ASSERT(dw.accept(1), "dedup_1_new");
    ASSERT(!dw.accept(1), "dedup_1_dup");
    ASSERT(dw.accept(2), "dedup_2_new");
    ASSERT(dw.accept(5), "dedup_5_new_gap_ok");     // luka OK, to nie duplikat
    ASSERT(!dw.accept(5), "dedup_5_dup");
    ASSERT(dw.duplicates == 2, "dedup_count");

    // #179 BackpressureMonitor — zaleglosc konsumenta wzgledem feedu.
    multicast::BackpressureMonitor bp;
    bp.on_enqueue(); bp.on_enqueue(); bp.on_enqueue();   // depth 3
    ASSERT(bp.depth() == 3 && bp.peak_depth == 3, "bp_depth_peak");
    bp.on_dequeue();                                     // depth 2
    ASSERT(bp.depth() == 2 && bp.peak_depth == 3, "bp_peak_retained");
    ASSERT(bp.overloaded(2) && !bp.overloaded(5), "bp_overloaded_threshold");
    bp.on_dequeue(10);                                   // nie schodzi ponizej 0
    ASSERT(bp.depth() == 0, "bp_no_underflow");

    // #187 LossRateMeter — agregatowa stopa utraty.
    multicast::LossRateMeter lrm;
    const uint64_t seqs[] = {1, 2, 3, 5, 6};             // brak 4; zakres 1..6
    for (uint64_t s : seqs) lrm.on_packet(s);
    ASSERT(lrm.expected() == 6 && lrm.received == 5, "loss_expected_received");
    ASSERT(lrm.lost() == 1, "loss_count");
    ASSERT(std::fabs(lrm.loss_rate() - 1.0/6.0) < 1e-9, "loss_rate");

    // #195 OutOfOrderMeter — odsetek pakietow poza kolejnoscia.
    multicast::OutOfOrderMeter ooo;
    const uint64_t oseq[] = {1, 2, 4, 3, 5};             // 3 przychodzi po 4
    for (uint64_t s : oseq) ooo.on_packet(s);
    ASSERT(ooo.total == 5 && ooo.out_of_order == 1, "ooo_counts");
    ASSERT(std::fabs(ooo.ooo_rate() - 0.2) < 1e-9, "ooo_rate");

    // #203 SequenceResetDetector — reset vs reorder.
    multicast::SequenceResetDetector srd(1000);
    ASSERT(!srd.on_seq(5000), "srd_init_no_reset");
    ASSERT(!srd.on_seq(5001), "srd_normal");
    ASSERT(!srd.on_seq(4999), "srd_small_reorder_not_reset");   // spadek 2 < 1000
    ASSERT(srd.on_seq(10), "srd_big_drop_is_reset");            // 5001 -> 10
    ASSERT(srd.resets == 1, "srd_reset_count");
    ASSERT(!srd.on_seq(11), "srd_normal_after_reset");          // nowa baza 10

    // #211 SnapshotRequestThrottle — min odstep miedzy zadaniami.
    multicast::SnapshotRequestThrottle srt(1000);               // min 1000 ns
    ASSERT(srt.allow(0), "srt_first_allowed");
    ASSERT(!srt.allow(500), "srt_throttled_500");               // 500 < 1000 od 0
    ASSERT(!srt.allow(999), "srt_throttled_999");
    ASSERT(srt.allow(1000), "srt_allowed_1000");                // 1000 >= 1000
    ASSERT(srt.allow(2500), "srt_allowed_2500");                // 1500 od ostatniego
    ASSERT(srt.suppressed == 2, "srt_suppressed_count");

    // #219 TokenBucket — burst do pojemnosci + uzupelnianie w czasie.
    multicast::TokenBucket tb(5.0, 1000.0);                     // 5 tokenow, 1000/s
    for (int i = 0; i < 5; ++i) ASSERT(tb.try_consume(0, 1.0), "tb_burst_ok");  // 5 od reki
    ASSERT(!tb.try_consume(0, 1.0), "tb_empty");               // 6ty pusto
    // po 2.5 ms uzupelni ~2.5 tokena -> dwa consume ok, trzeci nie
    ASSERT(tb.try_consume(2500000, 1.0), "tb_refill_1");
    ASSERT(tb.try_consume(2500000, 1.0), "tb_refill_2");
    ASSERT(!tb.try_consume(2500000, 1.0), "tb_refill_exhausted");

    // #227 ConflationBuffer — najnowszy stan per klucz + licznik konflacji.
    multicast::ConflationBuffer cb;
    cb.update(1, 100.0);              // klucz 1
    cb.update(1, 101.0);             // nadpisanie -> konflacja 1
    cb.update(2, 50.0);             // nowy klucz
    cb.update(1, 102.0);            // konflacja 2
    ASSERT(cb.pending() == 2 && cb.conflated == 2, "conflate_pending_count");
    double v = 0.0;
    ASSERT(cb.get(1, v) && std::fabs(v - 102.0) < 1e-9, "conflate_latest_value");
    ASSERT(cb.get(2, v) && std::fabs(v - 50.0) < 1e-9, "conflate_other_key");
    ASSERT(!cb.get(99, v), "conflate_unknown_false");
    cb.drain();
    ASSERT(cb.pending() == 0, "conflate_drain");

    // #235 LatencyTracker — EWMA + szczyt opoznienia.
    multicast::LatencyTracker lt(0.5);
    lt.sample(100);                  // ewma 100, max 100
    ASSERT(std::fabs(lt.avg_ns() - 100.0) < 1e-9 && lt.peak_ns() == 100, "lat_first");
    lt.sample(200);                  // ewma 0.5*200+0.5*100 = 150, max 200
    ASSERT(std::fabs(lt.avg_ns() - 150.0) < 1e-9 && lt.peak_ns() == 200, "lat_blend_peak");
    lt.sample(50);                   // ewma 0.5*50+0.5*150 = 100, max 200 (peak zostaje)
    ASSERT(std::fabs(lt.avg_ns() - 100.0) < 1e-9 && lt.peak_ns() == 200, "lat_peak_retained");
    ASSERT(lt.count == 3, "lat_count");

    // #243 ContiguousTracker — cumulative-ack watermark.
    multicast::ContiguousTracker ct;            // next_expected 1
    ct.receive(1);
    ASSERT(ct.contiguous_high() == 1, "contig_first");
    ct.receive(3);                              // luka: brak 2 -> buforuj 3
    ASSERT(ct.contiguous_high() == 1 && ct.buffered() == 1, "contig_gap_buffered");
    ct.receive(2);                              // wypelnia luke -> wciaga 2 i 3
    ASSERT(ct.contiguous_high() == 3 && ct.buffered() == 0, "contig_filled_pulls_ahead");
    ct.receive(1);                              // duplikat -> ignoruj
    ASSERT(ct.contiguous_high() == 3, "contig_duplicate_ignored");

    // #257 SlidingWindowRate — count within a moving window (1000 ns).
    multicast::SlidingWindowRate sw(1000);
    sw.on_event(0); sw.on_event(500); sw.on_event(900);
    ASSERT(sw.count() == 3, "swr_all_in_window");          // all within [-100, 900]
    sw.on_event(1500);                                     // prunes <= 500 (0 and 500)
    ASSERT(sw.count() == 2, "swr_pruned_old");             // keeps 900, 1500
    ASSERT(std::fabs(sw.rate_per_sec() - 2.0 * 1e9 / 1000.0) < 1e-3, "swr_rate");

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

    // #305 InterArrivalStats — feed jitter envelope.
    multicast::InterArrivalStats ias;
    ias.on_message(1000);     // first -> no gap
    ias.on_message(1050);     // gap 50
    ias.on_message(1060);     // gap 10
    ias.on_message(1200);     // gap 140
    ASSERT(ias.count == 3 && ias.min_gap == 10 && ias.max_gap == 140, "ias_min_max");
    ASSERT(std::fabs(ias.mean_gap() - (200.0 / 3.0)) < 1e-9, "ias_mean");  // (50+10+140)/3
    ASSERT(ias.jitter() == 130, "ias_jitter");                            // 140 - 10

    // #142 InterArrivalMeter — min/max/avg/jitter odstepow.
    multicast::InterArrivalMeter im;
    im.on_message(0); im.on_message(100); im.on_message(150); im.on_message(400);
    ASSERT(im.min_gap_ns() == 50, "iam_min_50");          // gaps: 100,50,250
    ASSERT(im.max_gap_ns() == 250, "iam_max_250");
    ASSERT(im.jitter_ns() == 200, "iam_jitter_200");      // 250-50
    ASSERT(std::fabs(im.avg_gap_ns() - 400.0/3.0) < 1e-6, "iam_avg");

    // #91 A/B line arbitration — pierwsza linia wygrywa, druga dedup; B łata lukę A.
    multicast::ABLineArbitrator arb;
    ASSERT(arb.on_packet(1, true),  "ab_a1_new");
    ASSERT(!arb.on_packet(1, false), "ab_b1_dup");        // B's 1 = duplikat
    ASSERT(arb.on_packet(2, true),  "ab_a2_new");
    ASSERT(arb.on_packet(4, true),  "ab_a4_gap");          // A skoczyło → brak 3
    ASSERT(arb.has_gaps() && arb.missing_count() == 1, "ab_gap_3_pending");
    ASSERT(arb.on_packet(3, false), "ab_b3_fills_gap");    // B dostarcza 3
    ASSERT(!arb.has_gaps(), "ab_self_healed");
    ASSERT(!arb.on_packet(4, false), "ab_b4_dup");
    ASSERT(arb.a_first == 3 && arb.b_first == 1, "ab_first_counts");

    // #98 staleness: brak pakietu > timeout = martwy feed.
    multicast::FeedStalenessMonitor sm;
    ASSERT(!sm.check(1000, 500), "stale_not_started");   // brak pierwszego pakietu
    sm.on_packet(1000);
    ASSERT(!sm.check(1400, 500), "stale_fresh");          // 400 <= 500
    ASSERT(sm.check(1600, 500), "stale_after_timeout");   // 600 > 500
    ASSERT(sm.is_stale() && sm.stale_events == 1, "stale_event_counted");
    sm.on_packet(1700);
    ASSERT(!sm.check(1800, 500), "stale_recovered");
}

// Momentum #85 — trend-following; znak decyzji odwrotny do mean-reversion.
void test_momentum() {
    SECTION("Momentum Strategy (#85)");
    MomentumStrategy m(/*window=*/3, /*threshold_pct=*/0.1, /*order_size=*/100);
    m.on_market_data("AAPL", 100.0);
    m.on_market_data("AAPL", 100.0);
    m.on_market_data("AAPL", 100.0);                 // okno pełne, SMA=100
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

// Donchian #124 — wybicie z kanalu (przebicie N-okresowego max/min).
void test_donchian() {
    SECTION("Donchian Breakout (#124)");
    DonchianBreakout up(3, 100);                  // kanal z 3 poprzednich
    up.on_market_data("X", 100.0);
    up.on_market_data("X", 101.0);
    up.on_market_data("X", 99.0);                 // okno pełne: hi=101, lo=99
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
    const Signal sf = fl.on_market_data("Z", 100.0);  // wewnatrz [99,101]
    ASSERT(!sf.valid, "donchian_inside_channel_holds");
}

// RSI #135 — oscylator pedu; rosnaca seria -> RSI 100 -> SELL, malejaca -> BUY.
void test_rsi() {
    SECTION("RSI Strategy (#135)");
    RSIStrategy up(3, 70.0, 30.0, 100);
    up.on_market_data("X", 100.0);                // baseline
    up.on_market_data("X", 101.0);
    up.on_market_data("X", 102.0);
    const Signal su = up.on_market_data("X", 103.0);  // okno pelne: same zyski -> RSI=100
    ASSERT(su.valid && su.side == Side::SELL, "rsi_overbought_sells");

    RSIStrategy dn(3, 70.0, 30.0, 100);
    dn.on_market_data("Y", 100.0);
    dn.on_market_data("Y", 99.0);
    dn.on_market_data("Y", 98.0);
    const Signal sd = dn.on_market_data("Y", 97.0);   // same straty -> RSI=0
    ASSERT(sd.valid && sd.side == Side::BUY, "rsi_oversold_buys");
}

// MA Crossover #157 — golden/death cross (przeciecie szybkiej i wolnej SMA).
void test_ma_crossover() {
    SECTION("MA Crossover (#157)");
    MACrossover x(2, 3, 100);                          // fast=2, slow=3
    x.on_market_data("X", 100.0);
    x.on_market_data("X", 100.0);
    x.on_market_data("X", 100.0);                      // setup, fast==slow, brak sygnalu
    const Signal up = x.on_market_data("X", 110.0);    // fast 105 > slow 103.3 -> golden cross
    ASSERT(up.valid && up.side == Side::BUY, "macross_golden_buys");
    const Signal dn = x.on_market_data("X", 90.0);     // fast 100 == slow 100 -> nie above -> death cross
    ASSERT(dn.valid && dn.side == Side::SELL, "macross_death_sells");
}

// Volatility #165 — kroczaca zmiennosc zwrotow + vol-targeting sizing.
void test_volatility() {
    SECTION("Volatility Estimator (#165)");
    VolatilityEstimator c(4);
    for (int i = 0; i < 5; ++i) c.on_price(100.0);          // stala cena -> zero vol
    ASSERT(c.volatility() == 0.0, "vol_constant_zero");

    VolatilityEstimator v(4);
    v.on_price(100.0); v.on_price(110.0); v.on_price(100.0); v.on_price(110.0); v.on_price(100.0);
    ASSERT(v.volatility() > 0.0, "vol_varying_positive");
    ASSERT(v.samples() == 4, "vol_samples");
    // vol-targeting: duza zmiennosc (~10%) vs cel 1% -> mniejsza pozycja
    ASSERT(v.target_size(1000, 0.01) < 1000, "vol_target_size_smaller");
    // brak zmiennosci -> base_size
    ASSERT(c.target_size(1000, 0.01) == 1000, "vol_target_size_base_when_calm");
}

// EMA #173 — wykladnicza srednia kroczaca.
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

// MACD #182 — momentum z trzech EMA.
void test_macd() {
    SECTION("MACD (#182)");
    MACD m;                                   // 12/26/9
    for (int i = 1; i <= 60; ++i) m.update(100.0 + i);   // monotoniczny wzrost
    ASSERT(m.ready(), "macd_ready");
    ASSERT(m.macd() > 0.0, "macd_positive_on_uptrend");   // fast EMA nad slow
    ASSERT(m.bullish(), "macd_bullish_on_uptrend");
    ASSERT(std::fabs(m.histogram() - (m.macd() - m.signal())) < 1e-9, "macd_histogram_def");
    MACD d;
    for (int i = 1; i <= 60; ++i) d.update(200.0 - i);   // monotoniczny spadek
    ASSERT(d.macd() < 0.0, "macd_negative_on_downtrend");
    ASSERT(!d.bullish(), "macd_not_bullish_on_downtrend");
}

// Stochastic #190 — oscylator %K.
void test_stochastic() {
    SECTION("Stochastic (#190)");
    Stochastic up(5);
    for (int i = 1; i <= 5; ++i) up.update(i);            // 1..5, cur=5 = szczyt
    ASSERT(up.ready(), "stoch_ready");
    ASSERT(std::fabs(up.percent_k() - 100.0) < 1e-9, "stoch_k_top_100");
    ASSERT(up.overbought(), "stoch_overbought");
    Stochastic dn(5);
    for (int i = 0; i < 5; ++i) dn.update(5.0 - i);       // 5,4,3,2,1, cur=1 = dno
    ASSERT(std::fabs(dn.percent_k() - 0.0) < 1e-9, "stoch_k_bottom_0");
    ASSERT(dn.oversold(), "stoch_oversold");
    Stochastic mid(3);
    mid.update(30); mid.update(10); mid.update(20);       // lo10 hi30 cur20 -> 50
    ASSERT(std::fabs(mid.percent_k() - 50.0) < 1e-9, "stoch_k_mid_50");
    Stochastic flat(3);
    flat.update(100); flat.update(100);                   // plaskie -> 50
    ASSERT(std::fabs(flat.percent_k() - 50.0) < 1e-9, "stoch_k_flat_neutral");
}

// WMA #198 — liniowo wazona srednia.
void test_wma() {
    SECTION("WMA (#198)");
    WMA w(3);
    w.update(1.0); w.update(2.0); w.update(3.0);          // (1*1+2*2+3*3)/(1+2+3)=14/6
    ASSERT(w.ready(), "wma_ready");
    ASSERT(std::fabs(w.value() - 14.0/6.0) < 1e-9, "wma_weighted");
    // przesuniecie okna: dochodzi 4, wypada 1 -> (2*1+3*2+4*3)/6 = 20/6
    w.update(4.0);
    ASSERT(std::fabs(w.value() - 20.0/6.0) < 1e-9, "wma_window_slide");
    WMA s(1);
    s.update(42.0);
    ASSERT(std::fabs(s.value() - 42.0) < 1e-9, "wma_period1_is_last");
}

// HullMA #206 — niskoopozniona srednia na bazie WMA.
void test_hull_ma() {
    SECTION("HullMA (#206)");
    HullMA c(9);
    for (int i = 0; i < 30; ++i) c.update(50.0);          // stala -> HMA = stala
    ASSERT(c.ready(), "hma_ready");
    ASSERT(std::fabs(c.value() - 50.0) < 1e-9, "hma_constant");
    // rosnacy trend -> HMA rosnie i (niska zwloka) wyprzedza prosta srednia okna
    HullMA r(9);
    for (int i = 1; i <= 29; ++i) r.update(static_cast<double>(i));
    const double prev = r.value();
    r.update(30.0);
    ASSERT(r.value() > prev, "hma_rises_on_uptrend");
    ASSERT(r.value() > 15.5, "hma_low_lag_above_mean");  // srednia 1..30 = 15.5
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
    ASSERT(r.value() > 15.0, "dema_low_lag");             // sledzi swieze (1..21 srednia ~11)
}

// TEMA #222 — potrojna EMA, najnizsza zwloka.
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
    ASSERT(r.value() > 15.0, "tema_low_lag");             // niska zwloka na rampie
}

// TRIX #230 — oscylator momentum z potrojnej EMA.
void test_trix() {
    SECTION("TRIX (#230)");
    TRIX c(5);
    for (int i = 0; i < 40; ++i) c.update(50.0);          // stala -> brak zmiany -> 0
    ASSERT(c.ready(), "trix_ready");
    ASSERT(std::fabs(c.value() - 0.0) < 1e-9, "trix_flat_zero");
    TRIX up(5);
    for (int i = 1; i <= 40; ++i) up.update(static_cast<double>(i));  // rosnaco
    ASSERT(up.value() > 0.0, "trix_positive_on_uptrend");
    TRIX dn(5);
    for (int i = 0; i < 30; ++i) dn.update(100.0 - i);   // malejaco (ceny dodatnie)
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

// Ensemble #140 — glosowanie sygnalow (zgoda >= min_agree).
void test_ensemble() {
    SECTION("Signal Ensemble (#140)");
    auto mk = [](Side s, int32_t q) {
        Signal sig; sig.valid = true; sig.side = s; sig.quantity = q; sig.price = 100.0;
        std::strncpy(sig.stock, "X", 8); return sig;
    };
    // 2 BUY vs 1 SELL, min_agree 2 -> BUY z suma qty 150
    Signal arr[3] = { mk(Side::BUY, 100), mk(Side::BUY, 50), mk(Side::SELL, 80) };
    const Signal r = combine_signals(arr, 3, 2);
    ASSERT(r.valid && r.side == Side::BUY && r.quantity == 150, "ensemble_majority_buy");
    // 1 vs 1, min_agree 2 -> brak zgody -> HOLD
    Signal tie[2] = { mk(Side::BUY, 100), mk(Side::SELL, 100) };
    ASSERT(!combine_signals(tie, 2, 2).valid, "ensemble_tie_holds");
    // min_agree 3 ale tylko 2 BUY -> HOLD
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

    // short, entry 100, trail 5 -> stop 105; spadek zaciska, wzrost wybija
    TrailingStop ss(false, 100.0, 5.0);
    ASSERT(close(ss.stop(), 105.0), "ts_short_initial");
    ASSERT(!ss.update(90.0), "ts_short_ratchet");        // stop -> 95
    ASSERT(close(ss.stop(), 95.0), "ts_short_ratchets_down");
    ASSERT(ss.update(95.0), "ts_short_stopped");         // 95 >= 95 -> exit
}

// POV #99 — Percentage-of-Volume execution algo (slicing adaptacyjny do wolumenu).
void test_pov_algo() {
    SECTION("POV Execution Algo (#99)");
    POVExecutor pov(1000, 0.10);                       // rodzic 1000, 10% wolumenu
    ASSERT(pov.on_market_volume(2000) == 200, "pov_slice_10pct_200");
    ASSERT(pov.remaining() == 800, "pov_remaining_800");
    ASSERT(pov.on_market_volume(10000) == 800, "pov_capped_to_remaining");  // 1000→800
    ASSERT(pov.done(), "pov_done");
    ASSERT(pov.on_market_volume(5000) == 0, "pov_zero_after_done");
    ASSERT(pov.slices() == 2, "pov_slice_count");

    POVExecutor low(1000, 0.10);
    ASSERT(low.on_market_volume(4) == 0, "pov_tiny_vol_no_slice");   // 0.4 < 0.5
}

// SignalThrottle #104 — minimalny odstep miedzy sygnalami per symbol.
void test_signal_throttle() {
    SECTION("Signal Throttle (#104)");
    SignalThrottle th(5);                                  // min 5 sekwencji
    ASSERT(th.allow("AAPL", 0), "throttle_first_passes");  // pierwszy zawsze
    ASSERT(!th.allow("AAPL", 3), "throttle_too_soon");      // 3 < 5 -> stlumiony
    ASSERT(th.allow("AAPL", 5), "throttle_after_cooldown"); // 5 >= 5
    ASSERT(th.allow("MSFT", 1), "throttle_per_symbol_indep"); // inny symbol niezalezny
    ASSERT(th.suppressed() == 1, "throttle_suppressed_count");
    th.reset_symbol("AAPL");
    ASSERT(th.allow("AAPL", 6), "throttle_reset_symbol");   // po resecie znow przechodzi
}

// VWAPTracker #113 — rynkowy VWAP + slippage egzekucji w bps.
void test_vwap_tracker() {
    SECTION("VWAP Tracker (#113)");
    auto close = [](double a, double b) { const double d = a - b; return (d<0?-d:d) < 1e-3; };
    VWAPTracker v;
    v.on_trade(100.0, 100);
    v.on_trade(102.0, 100);                       // VWAP = (10000+10200)/200 = 101
    ASSERT(close(v.vwap(), 101.0), "vwap_value");
    ASSERT(v.volume() == 200, "vwap_volume");
    // BUY @102 vs VWAP 101 -> (102-101)/101*1e4 = +99.01 bps (gorzej)
    ASSERT(close(v.slippage_bps(102.0, true), 99.0099), "vwap_buy_slippage_positive");
    // SELL @102 vs VWAP 101 -> pobilismy VWAP -> ujemne
    ASSERT(v.slippage_bps(102.0, false) < 0.0, "vwap_sell_beats_negative");
    ASSERT(close(v.slippage_bps(101.0, true), 0.0), "vwap_at_vwap_zero");
}

// Bollinger #93 — mean-reversion adaptacyjny do zmienności (pasma ±k·σ).
void test_bollinger() {
    SECTION("Bollinger Strategy (#93)");
    // window=2, k=0.5: dla 2 punktów b-mean == σ, więc b>a o cokolwiek przebija
    // pasmo (k<1) → deterministyczny sygnał.
    BollingerStrategy up(2, 0.5, 100);
    up.on_market_data("X", 100.0);
    const Signal su = up.on_market_data("X", 102.0);
    ASSERT(su.valid && su.side == Side::SELL, "bollinger_above_band_sells");

    BollingerStrategy dn(2, 0.5, 100);
    dn.on_market_data("Y", 100.0);
    const Signal sd = dn.on_market_data("Y", 98.0);
    ASSERT(sd.valid && sd.side == Side::BUY, "bollinger_below_band_buys");

    // Zero zmienności (σ=0) → brak sygnału (adaptacja: spokojny rynek = cisza).
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

    // --- Partial single-venue: zlecenie > top-of-book size ---
    {
        SmartOrderRouter r(RoutingStrategy::BEST_PRICE);
        r.add_venue(Venue("V", 100, 0.0));
        r.update_quote("V", 10.0, 11.0, 50, 50);   // tylko 50 dostępne
        const RouteDecision d = r.route_order("BUY", 200);
        ASSERT(d.valid && d.quantity == 50, "partial_filled_50");
        ASSERT(d.unfilled_qty == 150, "partial_unfilled_150");
    }

    // --- Split shortfall: Σpłynność < zlecenie ---
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

    // --- #86 venue health: seria odrzuceń wyłącza venue, sukces reaktywuje ---
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

    // --- #109 available_liquidity: suma top-of-book po aktywnych venue ---
    {
        SmartOrderRouter r(RoutingStrategy::BEST_PRICE);
        r.add_venue(Venue("A", 100, 0.0));
        r.add_venue(Venue("B", 100, 0.0));
        r.update_quote("A", 10.0, 11.0, 200, 150);   // bid 200, ask 150
        r.update_quote("B", 10.0, 11.0, 300, 250);
        ASSERT(r.available_liquidity(true) == 400, "liq_buy_sum_asks");   // 150+250
        ASSERT(r.available_liquidity(false) == 500, "liq_sell_sum_bids"); // 200+300
        r.record_reject("A"); r.record_reject("A"); r.record_reject("A"); // A wyłączone
        ASSERT(r.available_liquidity(true) == 250, "liq_excludes_inactive");
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
        // SPLIT rozbija miedzy oba (prog 100)
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
        rs.reset_routing_stats();
        ASSERT(rs.total_routed_shares() == 0, "tca_reset_zero");
    }

    // --- #138 kumulatywny koszt oplat ---
    {
        auto close = [](double a, double b) { const double d = a - b; return (d<0?-d:d) < 1e-6; };
        SmartOrderRouter r(RoutingStrategy::BEST_PRICE);
        r.add_venue(Venue("A", 100, 0.002));         // taker fee 0.002/akcja
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
        r.add_venue(Venue("B", 100, 0.001));         // ask 11 + fee 0.001 = 11.001 (lepszy)
        r.update_quote("A", 10.0, 11.0, 100, 100);
        r.update_quote("B", 10.0, 11.0, 100, 100);
        ASSERT(r.active_venue_count() == 2, "best_eff_active_2");
        ASSERT(close(r.best_effective_price(true), 11.001), "best_eff_buy_B");   // min all-in
        // SELL: bid 10 - fee; B: 10-0.001=9.999, A: 10-0.005=9.995 -> max 9.999
        ASSERT(close(r.best_effective_price(false), 9.999), "best_eff_sell_B");
        r.set_venue_active("B", false);
        ASSERT(r.active_venue_count() == 1, "best_eff_active_1_after_disable");
        ASSERT(close(r.best_effective_price(true), 11.005), "best_eff_buy_A_only");
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
        SmartOrderRouter e;                                    // brak venue -> reject
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
        r.update_quote("B", 10.0, 11.0, 100, 100);                 // B zostalo, dziala
        ASSERT(std::strcmp(r.route_order("BUY", 10).venue, "B") == 0, "remove_B_still_routes");
    }

    // --- #176 set_venue_fee (runtime zmiana taryfy -> routing all-in) ---
    {
        SmartOrderRouter r(RoutingStrategy::BEST_PRICE);
        r.add_venue(Venue("A", 100, 0.02));            // drozszy taker
        r.add_venue(Venue("B", 100, 0.01));            // tanszy
        r.update_quote("A", 10.0, 11.0, 100, 100);
        r.update_quote("B", 10.0, 11.0, 100, 100);     // ten sam quote
        ASSERT(std::strcmp(r.route_order("BUY", 10).venue, "B") == 0, "fee_default_B_cheaper");
        ASSERT(r.set_venue_fee("A", 0.0), "fee_set_A_zero");   // A teraz all-in 11.00
        ASSERT(std::strcmp(r.route_order("BUY", 10).venue, "A") == 0, "fee_reroute_A");
        ASSERT(!r.set_venue_fee("GHOST", 0.0), "fee_unknown_false");
    }

    // --- #184 is_marketable (pre-route guard dla limit orderow) ---
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

    // --- #192 reset_session_stats (pelny reset TCA, venue zostaja) ---
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
        r.set_venue_active("B", false);
        ASSERT(std::strcmp(r.fastest_venue(), "A") == 0, "fastest_A_after_disable");
        SmartOrderRouter empt(RoutingStrategy::BEST_PRICE);
        ASSERT(empt.fastest_venue() == nullptr, "fastest_empty_null");
        // #286 avg_venue_latency_ns: A 500, C 800 active (B disabled above) -> 650
        ASSERT(std::fabs(r.avg_venue_latency_ns() - 650.0) < 1e-9, "avglat_after_disable");
    }

    // --- #286 avg_venue_latency_ns ---
    {
        SmartOrderRouter r(RoutingStrategy::BEST_PRICE);
        r.add_venue(Venue("A", 100, 0.0));   // latency 100
        r.add_venue(Venue("B", 300, 0.0));   // latency 300
        ASSERT(std::fabs(r.avg_venue_latency_ns() - 200.0) < 1e-9, "avglat_200");
        r.set_venue_active("B", false);
        ASSERT(std::fabs(r.avg_venue_latency_ns() - 100.0) < 1e-9, "avglat_one_active");
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

    // --- #208 nbbo_spread / nbbo_spread_bps (skonsolidowany rynek) ---
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

    // --- #240 effective_spread_bps (koszt przejscia z oplatami) ---
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

    // --- #216 venue_share_pct (koncentracja egzekucji) ---
    {
        SmartOrderRouter r(RoutingStrategy::BEST_PRICE);
        r.add_venue(Venue("A", 100, 0.0));             // tanszy (fee 0) -> tu trafi routing
        r.add_venue(Venue("B", 100, 0.01));
        r.update_quote("A", 10.0, 11.0, 1000, 1000);
        r.update_quote("B", 10.0, 11.0, 1000, 1000);
        r.route_order("BUY", 30);
        r.route_order("BUY", 70);                       // razem 100, wszystko na A
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
        r.update_quote("B", 10.0, 11.0, 100, 200);     // ask size 200 -> razem 300
        ASSERT(r.fill_shortfall(true, 250) == 0, "shortfall_covered");
        ASSERT(r.fill_shortfall(true, 400) == 100, "shortfall_100");      // 400 - 300
        ASSERT(r.fill_shortfall(true, 300) == 0, "shortfall_exact");
        ASSERT(std::fabs(r.fillable_ratio(true, 400) - 0.75) < 1e-9, "ratio_three_quarters");
        ASSERT(std::fabs(r.fillable_ratio(true, 250) - 1.0) < 1e-9, "ratio_full");
    }

    // --- #232 avg_fee_per_share (TCA: netto taker/maker) ---
    {
        SmartOrderRouter r(RoutingStrategy::BEST_PRICE);
        r.add_venue(Venue("A", 100, 0.002));           // taker fee 0.002/akcja
        r.update_quote("A", 10.0, 11.0, 1000, 1000);
        r.route_order("BUY", 100);                      // fee 0.2
        r.route_order("BUY", 100);                      // fee 0.2, razem 0.4 / 200 szt
        ASSERT(std::fabs(r.avg_fee_per_share() - 0.002) < 1e-9, "avgfee_taker");
        SmartOrderRouter empt(RoutingStrategy::BEST_PRICE);
        ASSERT(empt.avg_fee_per_share() == 0.0, "avgfee_empty_zero");
    }
}


// Backtester #80 — rdzeń metryk wynikowych (Sharpe/DD/hit-rate/fill-rate).
void test_backtester() {
    SECTION("Backtester (#80)");
    backtest::Backtester bt;
    bt.on_order(true);  bt.on_trade(+100.0);
    bt.on_order(true);  bt.on_trade(-40.0);
    bt.on_order(true);  bt.on_trade(+60.0);
    bt.on_order(false);                          // submitted, nie filled
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

    // #102 atrybucja per-tag (np. per strategia).
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

    // Bez ceny referencyjnej band jest pomijany — pierwsze zlecenie przechodzi.
    ASSERT(r.check_order("AAPL", Side::BUY, 150.00, 10).action == RiskAction::ALLOW,
           "band_no_ref_allows");

    r.update_reference_price("AAPL", 150.00);
    ASSERT(r.check_order("AAPL", Side::BUY, 165.00, 10).action == RiskAction::ALLOW,
           "band_within_10pct_allows");                 // +10% < 20%
    ASSERT(r.check_order("AAPL", Side::BUY, 1500.00, 10).action == RiskAction::REJECT,
           "band_fat_finger_rejects");                  // +900% — gruba pomyłka
    ASSERT(r.check_order("AAPL", Side::SELL, 100.00, 10).action == RiskAction::REJECT,
           "band_far_below_rejects");                   // -33% < -20%

    // Band wyłączony (≤0) → nawet 10× cena przechodzi.
    RiskLimits off = lim; off.max_price_band_pct = 0.0;
    RiskManager r2(off);
    r2.update_reference_price("AAPL", 150.00);
    ASSERT(r2.check_order("AAPL", Side::BUY, 1500.00, 10).action == RiskAction::ALLOW,
           "band_disabled_allows");

    // Atomic kill switch — toggling działa jak wcześniej (typ atomic<bool>).
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

    // #106 asymetryczne limity: long 1000, short tylko 300.
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

    // #114 bezpiecznik serii strat: 3 stratne z rzedu -> kill switch.
    RiskLimits cl;
    cl.max_consecutive_losses = 3;
    RiskManager r7(cl);
    r7.update_pnl(-10.0); r7.update_pnl(-10.0);
    ASSERT(!r7.is_kill_switch_active() && r7.get_consecutive_losses() == 2, "consec_2_ok");
    r7.update_pnl(-10.0);                                    // 3. -> trip
    ASSERT(r7.is_kill_switch_active(), "consec_3_trips_kill");
    // zysk w srodku zeruje serie
    RiskManager r8(cl);
    r8.update_pnl(-10.0); r8.update_pnl(-10.0); r8.update_pnl(+5.0);
    ASSERT(r8.get_consecutive_losses() == 0, "consec_win_resets");
    r8.update_pnl(-10.0); r8.update_pnl(-10.0);
    ASSERT(!r8.is_kill_switch_active(), "consec_no_trip_after_reset");

    // #121 powod zatrzasniecia kill switcha.
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
    rbk.update_pnl(-200.0);                                   // ponad dzienny limit
    rbk.check_order("AAPL", Side::BUY, 10.0, 1);              // circuit breaker w check
    ASSERT(rbk.get_kill_reason() == KillReason::CIRCUIT_BREAKER, "killreason_circuit");

    // #129 runtime update limitow — zaostrz pozycje intraday.
    RiskManager rl(lim);
    ASSERT(rl.check_order("AAPL", Side::BUY, 1.0, 500).action == RiskAction::ALLOW,
           "setlim_before_ok");                       // 500 < lim (1000000)
    RiskLimits tighter = rl.get_limits();
    tighter.max_position_per_symbol = 100;
    rl.set_limits(tighter);
    ASSERT(rl.get_limits().max_position_per_symbol == 100, "setlim_applied");
    ASSERT(rl.check_order("AAPL", Side::BUY, 1.0, 500).action == RiskAction::REJECT,
           "setlim_after_rejects");                    // 500 > 100 teraz

    // #137 zapytania o ekspozycje (|pos+pend|, total = niezmiennik O(1)).
    RiskManager re(lim);
    re.on_order_sent("AAPL", Side::BUY, 100);          // pending +100
    re.on_order_sent("MSFT", Side::SELL, 50);          // pending -50
    ASSERT(re.get_exposure("AAPL") == 100, "exp_aapl_100");
    ASSERT(re.get_exposure("MSFT") == 50, "exp_msft_50");
    ASSERT(re.get_total_exposure() == 150, "exp_total_150");
    re.update_position("AAPL", Side::BUY, 60);          // pos+60 pend-60 -> bez zmian
    ASSERT(re.get_exposure("AAPL") == 100 && re.get_total_exposure() == 150,
           "exp_invariant_after_fill");

    // #144 dzienny limit obrotu (turnover).
    RiskLimits tl;
    tl.max_daily_traded_notional = 1000.0;
    RiskManager rt(tl);
    ASSERT(rt.check_order("AAPL", Side::BUY, 10.0, 10).action == RiskAction::ALLOW,
           "turnover_under_ok");
    rt.add_traded_notional(1500.0);                     // przekroczony obrot
    ASSERT(std::fabs(rt.get_traded_notional() - 1500.0) < 1e-9, "turnover_tracked");
    ASSERT(rt.check_order("AAPL", Side::BUY, 10.0, 10).action == RiskAction::REJECT,
           "turnover_over_rejects");

    // #153 ekspozycja nominalna w $.
    RiskManager rn(lim);
    rn.update_reference_price("AAPL", 150.0);
    rn.on_order_sent("AAPL", Side::BUY, 100);          // 100 sztuk
    ASSERT(std::fabs(rn.get_position_notional("AAPL") - 15000.0) < 1e-6, "notional_15000");
    ASSERT(rn.get_position_notional("MSFT") == 0.0, "notional_no_ref_zero");

    // #161 per-symbol override limitu pozycji.
    RiskLimits pl; pl.max_position_per_symbol = 1000;
    RiskManager rp(pl);
    ASSERT(rp.check_order("TSLA", Side::BUY, 1.0, 500).action == RiskAction::ALLOW,
           "symlim_global_ok");                       // 500 < 1000
    rp.set_symbol_position_limit("TSLA", 100);        // ciaśniej dla TSLA
    ASSERT(rp.check_order("TSLA", Side::BUY, 1.0, 500).action == RiskAction::REJECT,
           "symlim_override_rejects");                 // 500 > 100
    ASSERT(rp.check_order("AAPL", Side::BUY, 1.0, 500).action == RiskAction::ALLOW,
           "symlim_other_uses_global");                // AAPL: globalny 1000
    rp.set_symbol_position_limit("TSLA", 0);          // zdejmij override
    ASSERT(rp.check_order("TSLA", Side::BUY, 1.0, 500).action == RiskAction::ALLOW,
           "symlim_override_removed");

    // #167 wczesne ostrzezenie limitu (cap 1000).
    RiskManager rw(pl);
    rw.on_order_sent("AAPL", Side::BUY, 700);          // ekspozycja 700 = 70%
    ASSERT(!rw.is_near_position_limit("AAPL", 80.0), "warn_below_80pct");   // 700 < 800
    ASSERT(rw.is_near_position_limit("AAPL", 60.0), "warn_above_60pct");    // 700 >= 600

    // #181 wykorzystanie budzetu ekspozycji portfela (limit 1000).
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
    rhr.on_order_sent("AAPL", Side::BUY, 300);                              // pozycja +300
    ASSERT(rhr.headroom_shares("AAPL", Side::BUY) == 700, "headroom_buy");  // 1000 - 300
    ASSERT(rhr.headroom_shares("AAPL", Side::SELL) == 1300, "headroom_sell"); // 1000 + 300 (flip)
    ASSERT(rhr.headroom_shares("MSFT", Side::BUY) == 1000, "headroom_fresh"); // pelny cap

    // #259 projected_exposure (resulting |position| after a hypothetical fill).
    RiskManager rpe(hrl);
    rpe.on_order_sent("AAPL", Side::BUY, 300);                              // pozycja +300
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

    // #189 limit wartosci pozycji per symbol ($10k; shares hojne).
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

    // #197 current_drawdown_pct (high-water mark).
    RiskManager rdd;                       // domyslne limity
    rdd.update_pnl(1000.0);                 // peak 1000, daily 1000
    ASSERT(std::fabs(rdd.current_drawdown_pct() - 0.0) < 1e-6, "dd_at_peak_zero");
    rdd.update_pnl(-300.0);                 // daily 700 -> dd = 300/1000 = 30%
    ASSERT(std::fabs(rdd.current_drawdown_pct() - 30.0) < 1e-6, "dd_30pct");
    ASSERT(std::fabs(rdd.get_peak_pnl() - 1000.0) < 1e-6, "dd_peak_1000");
    RiskManager rzz;
    rzz.update_pnl(-500.0);                 // peak 0 -> brak referencji
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

    // #205 consecutive_losses_remaining (breaker serii strat, prog 3).
    RiskLimits cll; cll.max_consecutive_losses = 3;
    RiskManager rcl(cll);
    ASSERT(rcl.consecutive_losses_remaining() == 3, "clr_full_at_start");
    rcl.update_pnl(-10.0);                  // strata 1
    ASSERT(rcl.consecutive_losses_remaining() == 2, "clr_after_one_loss");
    rcl.update_pnl(-10.0);                  // strata 2
    ASSERT(rcl.consecutive_losses_remaining() == 1, "clr_after_two_losses");
    rcl.update_pnl(5.0);                    // zysk zeruje serie
    ASSERT(rcl.consecutive_losses_remaining() == 3, "clr_reset_on_win");
    RiskManager rcd;                        // domyslnie wylaczony (0)
    ASSERT(rcd.consecutive_losses_remaining() == -1, "clr_disabled");

    // #213 remaining_loss_budget (max_daily_loss 1000).
    RiskLimits lbl; lbl.max_daily_loss = 1000;
    RiskManager rlb(lbl);
    ASSERT(std::fabs(rlb.remaining_loss_budget() - 1000.0) < 1e-6, "rlb_full_at_start");
    rlb.update_pnl(-300.0);                 // budzet 700
    ASSERT(std::fabs(rlb.remaining_loss_budget() - 700.0) < 1e-6, "rlb_after_loss");
    rlb.update_pnl(-800.0);                 // za prog -> clamp 0
    ASSERT(std::fabs(rlb.remaining_loss_budget() - 0.0) < 1e-6, "rlb_clamped_zero");
    RiskManager rlp(lbl);
    rlp.update_pnl(500.0);                  // zysk zwieksza budzet -> 1500
    ASSERT(std::fabs(rlp.remaining_loss_budget() - 1500.0) < 1e-6, "rlb_profit_extends");

    // #307 loss_budget_utilization_pct (same 1000 limit).
    ASSERT(std::fabs(rlb.loss_budget_utilization_pct() - 100.0) < 1e-6, "lbu_clamped_full"); // -1100/1000
    ASSERT(rlp.loss_budget_utilization_pct() == 0.0, "lbu_profit_zero");        // in profit
    RiskManager rlu(lbl);
    rlu.update_pnl(-250.0);                  // 250 of 1000 -> 25%
    ASSERT(std::fabs(rlu.loss_budget_utilization_pct() - 25.0) < 1e-6, "lbu_quarter");

    // #221 daily_turnover_pct (limit obrotu 100000).
    RiskLimits tnl; tnl.max_daily_traded_notional = 100000.0;
    RiskManager rtn(tnl);
    rtn.add_traded_notional(30000.0);
    ASSERT(std::fabs(rtn.daily_turnover_pct() - 30.0) < 1e-6, "turnover_30pct");
    rtn.add_traded_notional(20000.0);       // razem 50000
    ASSERT(std::fabs(rtn.daily_turnover_pct() - 50.0) < 1e-6, "turnover_50pct");
    RiskManager rtd;                        // limit wylaczony (0)
    rtd.add_traded_notional(50000.0);
    ASSERT(std::fabs(rtd.daily_turnover_pct() - 0.0) < 1e-6, "turnover_disabled");

    // #229 check_reject_rate (maly max_order_value -> czesc check'ow odrzucona).
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

    // #94 fat-finger na ilość — qty cap niezależny od notional.
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
           "qtycap_over_rejects");   // tani notional ($10), ale 1001 szt. > limit

    // #175 prog minimalnej ceny (penny-stock filter).
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

    // --- Admin builder: Logon parsuje się jako poprawny FIX z 8/9/10 ---
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

    // --- #119 inbound SequenceReset: ustaw oczekiwany seq, ignoruj wsteczny ---
    {
        fix::FIXSession si;
        si.observe_inbound(1, 100);
        ASSERT(si.expected_inbound_seq() == 2, "seqreset_expected_2");
        const auto g = si.observe_inbound(5, 200);                 // luka 2..4
        ASSERT(g.valid && si.expected_inbound_seq() == 6, "seqreset_after_gap_6");
        si.apply_inbound_sequence_reset(10);                       // NewSeqNo=10
        ASSERT(si.expected_inbound_seq() == 10, "seqreset_applied_10");
        si.apply_inbound_sequence_reset(3);                        // wsteczny -> ignoruj
        ASSERT(si.expected_inbound_seq() == 10, "seqreset_ignores_backwards");
    }

    // --- ResendRequest niesie BeginSeqNo(7)/EndSeqNo(16) i bumpuje licznik ---
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

    // --- Persystencja seq: wyślij kilka, persist, nowa sesja, load, kontynuuj ---
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
        // Po restarcie kolejny build używa seq 3 — nie reużywa 1/2.
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
        // #168 typowane akcesory
        ASSERT(m.get_int(38) == 100, "fix_get_int_qty");
        ASSERT(std::fabs(m.get_double(44) - 150.25) < 1e-6, "fix_get_double_price");
        ASSERT(m.get_int(99999) == 0, "fix_get_int_missing_zero");
        // #177 klasyfikacja admin vs application
        ASSERT(!m.is_admin(), "fix_neworder_is_application");
        ASSERT(FIXMessage::is_admin_msg_type("0"), "fix_admin_heartbeat");
        ASSERT(FIXMessage::is_admin_msg_type("A"), "fix_admin_logon");
        ASSERT(!FIXMessage::is_admin_msg_type("D"), "fix_app_neworder");
        ASSERT(!FIXMessage::is_admin_msg_type("AE"), "fix_multichar_not_admin");
        s.build_heartbeat(buf, sizeof(buf), nullptr, '|');
        FIXMessage hbeat; hbeat.parse(buf);
        ASSERT(hbeat.is_admin(), "fix_heartbeat_is_admin");

        // #185 OrderStatusRequest (35=H) — rekoncyliacja po reconnekcie.
        s.build_order_status_request(buf, sizeof(buf), "ORD1", "AAPL", Side::BUY, '|');
        FIXMessage osr; osr.parse(buf);
        ASSERT(osr.is_valid() && osr.get_msg_type()[0] == 'H', "fix_osr_H_valid");
        ASSERT(std::strcmp(osr.get_field(11), "ORD1") == 0, "fix_osr_clordid");
        ASSERT(std::strcmp(osr.get_symbol(), "AAPL") == 0, "fix_osr_symbol");
        ASSERT(std::strcmp(osr.get_side(), "BUY") == 0, "fix_osr_side");
        ASSERT(!osr.is_admin(), "fix_osr_is_application");

        // #193 OrderMassCancelRequest (35=q) — panic button.
        s.build_mass_cancel(buf, sizeof(buf), "MC1", '1', "AAPL", '|');   // po symbolu
        FIXMessage mc1; mc1.parse(buf);
        ASSERT(mc1.is_valid() && mc1.get_msg_type()[0] == 'q', "fix_masscancel_q_valid");
        ASSERT(std::strcmp(mc1.get_field(530), "1") == 0, "fix_masscancel_by_symbol");
        ASSERT(std::strcmp(mc1.get_symbol(), "AAPL") == 0, "fix_masscancel_symbol");
        s.build_mass_cancel(buf, sizeof(buf), "MC2", '7', nullptr, '|'); // wszystkie
        FIXMessage mc2; mc2.parse(buf);
        ASSERT(std::strcmp(mc2.get_field(530), "7") == 0, "fix_masscancel_all");

        // #201 OrderMassCancelReport (35=r) — odpowiedz gieldy.
        s.build_mass_cancel_report(buf, sizeof(buf), "MC2", '7', 5, '|');
        FIXMessage mcr; mcr.parse(buf);
        ASSERT(mcr.is_valid() && mcr.get_msg_type()[0] == 'r', "fix_mcr_r_valid");
        ASSERT(std::strcmp(mcr.get_field(531), "7") == 0, "fix_mcr_response_all");
        ASSERT(mcr.get_int(533) == 5, "fix_mcr_affected_count");

        // #209 MarketDataRequest (35=V) — subskrypcja danych rynkowych.
        s.build_market_data_request(buf, sizeof(buf), "MDR1", '1', 1, "AAPL", '|');
        FIXMessage mdr; mdr.parse(buf);
        ASSERT(mdr.is_valid() && mdr.get_msg_type()[0] == 'V', "fix_mdr_V_valid");
        ASSERT(std::strcmp(mdr.get_field(262), "MDR1") == 0, "fix_mdr_reqid");
        ASSERT(std::strcmp(mdr.get_field(263), "1") == 0, "fix_mdr_subtype");
        ASSERT(mdr.get_int(264) == 1 && std::strcmp(mdr.get_symbol(), "AAPL") == 0, "fix_mdr_depth_symbol");

        // #217 MarketDataSnapshotFullRefresh (35=W) — odpowiedz na 35=V.
        s.build_md_snapshot(buf, sizeof(buf), "MDR1", "AAPL", 99.98, 100, 100.02, 200, '|');
        FIXMessage mdw; mdw.parse(buf);
        ASSERT(mdw.is_valid() && mdw.get_msg_type()[0] == 'W', "fix_mdw_W_valid");
        ASSERT(std::strcmp(mdw.get_field(262), "MDR1") == 0
               && std::strcmp(mdw.get_symbol(), "AAPL") == 0, "fix_mdw_reqid_symbol");
        ASSERT(mdw.get_int(268) == 2, "fix_mdw_two_entries");
        ASSERT(std::strcmp(mdw.get_field(269), "0") == 0, "fix_mdw_first_is_bid"); // pierwszy wpis = bid
        ASSERT(std::fabs(mdw.get_double(270) - 99.98) < 1e-6, "fix_mdw_bid_px");

        // #225 MarketDataIncrementalRefresh (35=X) — przyrostowa zmiana bid.
        s.build_md_incremental(buf, sizeof(buf), "MDR1", '1', '0', "AAPL", 100.05, 500, '|');
        FIXMessage mdx; mdx.parse(buf);
        ASSERT(mdx.is_valid() && mdx.get_msg_type()[0] == 'X', "fix_mdx_X_valid");
        ASSERT(std::strcmp(mdx.get_field(279), "1") == 0, "fix_mdx_update_change"); // change
        ASSERT(std::strcmp(mdx.get_field(269), "0") == 0, "fix_mdx_bid_entry");     // bid
        ASSERT(std::fabs(mdx.get_double(270) - 100.05) < 1e-6 && mdx.get_int(271) == 500,
               "fix_mdx_px_size");

        // #233 MarketDataRequestReject (35=Y) — odrzucenie subskrypcji.
        s.build_md_request_reject(buf, sizeof(buf), "MDR1", '0', '|');  // unknown symbol
        FIXMessage mdy; mdy.parse(buf);
        ASSERT(mdy.is_valid() && mdy.get_msg_type()[0] == 'Y', "fix_mdy_Y_valid");
        ASSERT(std::strcmp(mdy.get_field(262), "MDR1") == 0, "fix_mdy_reqid_echo");
        ASSERT(std::strcmp(mdy.get_field(281), "0") == 0, "fix_mdy_reject_reason");

        // #241 parse_exec_report — ekstrakcja pol 35=8 do struktury.
        s.build_exec_report(buf, sizeof(buf), "ORD1", "EX1", "E1", '2', '2',  // Fill/Fill
                            "AAPL", Side::BUY, 100, 150.25, 100, 0, '|');
        FIXMessage erm; erm.parse(buf);
        const fix::FIXSession::ExecReport exr = fix::FIXSession::parse_exec_report(erm);
        ASSERT(exr.valid && exr.exec_type == '2' && exr.ord_status == '2', "fix_execrep_type_status");
        ASSERT(exr.last_qty == 100 && std::fabs(exr.last_px - 150.25) < 1e-6, "fix_execrep_last");
        ASSERT(exr.cum_qty == 100 && exr.leaves_qty == 0, "fix_execrep_cum_leaves");
        // wiadomosc nie-8 -> invalid
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

        // #256 QuoteRequest (35=R) — RFQ request side.
        s.build_quote_request(buf, sizeof(buf), "QR1", "AAPL", '|');
        FIXMessage qrq; qrq.parse(buf);
        ASSERT(qrq.is_valid() && qrq.get_msg_type()[0] == 'R', "fix_qreq_R_valid");
        ASSERT(std::strcmp(qrq.get_field(131), "QR1") == 0
               && std::strcmp(qrq.get_symbol(), "AAPL") == 0, "fix_qreq_id_symbol");

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

        // #295 BusinessMessageReject (35=j) — application-level rejection.
        s.build_business_reject(buf, sizeof(buf), 42, "D", 2, "Unknown symbol", '|');
        FIXMessage bmr; bmr.parse(buf);
        ASSERT(bmr.is_valid() && bmr.get_msg_type()[0] == 'j', "fix_bizrej_j_valid");
        ASSERT(bmr.get_int(45) == 42 && std::strcmp(bmr.get_field(372), "D") == 0,
               "fix_bizrej_ref");
        ASSERT(bmr.get_int(380) == 2 && std::strcmp(bmr.get_field(58), "Unknown symbol") == 0,
               "fix_bizrej_reason_text");
        ASSERT(!bmr.is_admin(), "fix_bizrej_application");  // business reject is application

        // #303 TradingSessionStatus (35=h) — venue session phase.
        s.build_trading_session_status(buf, sizeof(buf), "REG", 2, '|');  // 2 = Open
        FIXMessage tss; tss.parse(buf);
        ASSERT(tss.is_valid() && tss.get_msg_type()[0] == 'h', "fix_tss_h_valid");
        ASSERT(std::strcmp(tss.get_field(336), "REG") == 0 && tss.get_int(340) == 2,
               "fix_tss_id_status");
        ASSERT(!tss.is_admin(), "fix_tss_application");

        s.build_cancel_replace(buf, sizeof(buf), "ORD2", "ORD1", "AAPL", Side::SELL, 80, 151.00, '|');
        FIXMessage g; g.parse(buf);
        ASSERT(g.is_valid() && g.get_msg_type()[0] == 'G', "fix_replace_G_valid");
        ASSERT(std::strcmp(g.get_field(41), "ORD1") == 0, "fix_replace_origclordid");

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

        // #158 inbound TestRequest -> zapamietaj 112, odpowiedz Heartbeatem.
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

        // #126 Session-level Reject (35=3) — np. po negatywnym validate_new_order.
        s.build_reject(buf, sizeof(buf), 42, "D", 1, "Required tag missing", '|');
        FIXMessage rj; rj.parse(buf);
        ASSERT(rj.is_valid() && rj.get_msg_type()[0] == '3', "fix_reject_valid");
        ASSERT(std::atoi(rj.get_field(45)) == 42, "fix_reject_refseqnum");
        ASSERT(std::atoi(rj.get_field(373)) == 1, "fix_reject_reason_code");
        ASSERT(std::strcmp(rj.get_field(372), "D") == 0, "fix_reject_refmsgtype");

        // #133 Business Message Reject (35=j) — np. nieznany symbol.
        s.build_business_reject(buf, sizeof(buf), "D", "ORD9", 2, "Unknown symbol", '|');
        FIXMessage bj; bj.parse(buf);
        ASSERT(bj.is_valid() && bj.get_msg_type()[0] == 'j', "fix_busreject_valid");
        ASSERT(std::strcmp(bj.get_field(379), "ORD9") == 0, "fix_busreject_refid");
        ASSERT(std::atoi(bj.get_field(380)) == 2, "fix_busreject_reason");

        // #143 OrderCancelReject (35=9) — odrzucenie cancel/replace (np. za pozno).
        s.build_cancel_reject(buf, sizeof(buf), "CXL1", "ORD1", "EXG1", '2', '2', 0,
                              "Too late to cancel", '|');
        FIXMessage cr; cr.parse(buf);
        ASSERT(cr.is_valid() && cr.get_msg_type()[0] == '9', "fix_cxlreject_valid");
        ASSERT(cr.get_field(434)[0] == '2', "fix_cxlreject_response_to");  // wobec Replace
        ASSERT(std::atoi(cr.get_field(102)) == 0, "fix_cxlreject_reason_too_late");
        ASSERT(std::strcmp(cr.get_field(41), "ORD1") == 0, "fix_cxlreject_origclordid");
    }
}


// FIX order state #111 — kliencka maszyna stanu z ExecutionReportow (35=8).
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
}

// OUCH order state #89 — kliencka maszyna stanu (token → live/partial/filled).
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

    // #112 Order Replaced ('U'): encode → decode (nowy + poprzedni token).
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

    // #178 Cancel Reject ('I'): gielda odrzuca probe anulowania.
    n = OUCHMessage::encode_cancel_reject(buf, "TOK9", 'T');   // T = too late (juz wykonane)
    const OUCHResponse cr = OUCHMessage::parse_response(buf, n);
    ASSERT(std::strcmp(cr.type, "CXL_REJECT") == 0, "ouch_cxlrej_parsed");
    ASSERT(std::strcmp(cr.token, "TOK9") == 0, "ouch_cxlrej_token");
    ASSERT(cr.reason[0] == 'T', "ouch_cxlrej_reason");
    ASSERT(std::strcmp(OUCHMessage::parse_response(buf, 15).type, "ERROR") == 0,
           "ouch_cxlrej_short_error");

    // #186 Cancel Pending ('P'): anulowanie przyjete, jeszcze nie finalne.
    n = OUCHMessage::encode_cancel_pending(buf, "TOK5");
    const OUCHResponse cp = OUCHMessage::parse_response(buf, n);
    ASSERT(std::strcmp(cp.type, "CXL_PEND") == 0, "ouch_cxlpend_parsed");
    ASSERT(std::strcmp(cp.token, "TOK5") == 0, "ouch_cxlpend_token");
    ASSERT(std::strcmp(OUCHMessage::parse_response(buf, 14).type, "ERROR") == 0,
           "ouch_cxlpend_short_error");

    // #194 AIQ Canceled ('D'): self-match prevention zdejmuje czesc zlecenia.
    n = OUCHMessage::encode_aiq_canceled(buf, "TOK7", 40, 'Q');
    const OUCHResponse aq = OUCHMessage::parse_response(buf, n);
    ASSERT(std::strcmp(aq.type, "AIQ_CXL") == 0, "ouch_aiq_parsed");
    ASSERT(std::strcmp(aq.token, "TOK7") == 0 && aq.shares == 40, "ouch_aiq_fields");
    ASSERT(aq.reason[0] == 'Q', "ouch_aiq_reason");
    ASSERT(std::strcmp(OUCHMessage::parse_response(buf, 19).type, "ERROR") == 0,
           "ouch_aiq_short_error");

    // #202 System Event ('S'): zdarzenie sesji (otwarcie rynku).
    n = OUCHMessage::encode_system_event(buf, 123456789, 'O');
    const OUCHResponse se = OUCHMessage::parse_response(buf, n);
    ASSERT(std::strcmp(se.type, "SYS_EVENT") == 0, "ouch_sysevent_parsed");
    ASSERT(se.match_number == 123456789 && se.reason[0] == 'O', "ouch_sysevent_fields");
    ASSERT(std::strcmp(OUCHMessage::parse_response(buf, 9).type, "ERROR") == 0,
           "ouch_sysevent_short_error");

    // #210 Restated ('R'): gielda zmienia parametry bez prosby klienta.
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

    // #152 parse_order — strona gieldy dekoduje zlecenia klienta O/X/U.
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

    // #226 Modify Order ('M') — redukcja wolumenu (decrease-only).
    n = OUCHMessage::modify_order(buf, "TOK1", 50);
    const OUCHOrder mo = OUCHMessage::parse_order(buf, n);
    ASSERT(mo.valid && mo.type == 'M' && std::strcmp(mo.token, "TOK1") == 0
           && mo.shares == 50, "ouch_parse_modify");
    ASSERT(OUCHMessage::validate_order(mo) == nullptr, "ouch_modify_valid");
    n = OUCHMessage::modify_order(buf, "TOK1", 0);                      // 0 = nieprawidlowe
    ASSERT(std::strcmp(OUCHMessage::validate_order(OUCHMessage::parse_order(buf, n)),
                       "non-positive shares") == 0, "ouch_modify_zero_rejected");

    // #169 validate_order — gateway gieldy waliduje zlecenie klienta.
    n = OUCHMessage::enter_order(buf, "TOK1", 'B', 100, "AAPL", 150.25);
    ASSERT(OUCHMessage::validate_order(OUCHMessage::parse_order(buf, n)) == nullptr,
           "ouch_validate_ok");
    n = OUCHMessage::enter_order(buf, "TOK1", 'B', 0, "AAPL", 150.25);   // shares 0
    ASSERT(std::strcmp(OUCHMessage::validate_order(OUCHMessage::parse_order(buf, n)),
                       "non-positive shares") == 0, "ouch_validate_zero_shares");
    n = OUCHMessage::enter_order(buf, "TOK1", 'X', 100, "AAPL", 150.25); // zla strona
    ASSERT(std::strcmp(OUCHMessage::validate_order(OUCHMessage::parse_order(buf, n)),
                       "invalid side") == 0, "ouch_validate_bad_side");

    // #242 agregaty wolumenu trackera (swiezy tracker).
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
}

// OUCH ↔ SoupBinTCP #78 — pełny roundtrip login→order→accepted→executed.
void test_soupbin_ouch_session() {
    SECTION("SoupBin/OUCH Session (#78)");
    using namespace soupbin;

    // Klient buduje strumień: Login Request + Enter Order ('U' z OUCH 'O').
    uint8_t cstream[256];
    std::size_t coff = 0;
    coff += pack_login_request(cstream + coff, sizeof(cstream) - coff,
                               "USER1", "PASS", "", "0");
    uint8_t ouch[64];
    const int olen = OUCHMessage::enter_order(ouch, "TOK0001", 'B', 100, "AAPL", 150.25);
    coff += pack_data(cstream + coff, sizeof(cstream) - coff,
                      ouch, static_cast<std::size_t>(olen), /*client_side=*/true);

    // Mock giełda odpowiada: A (login accepted) + S(Accepted) + S(Executed).
    uint8_t sstream[256];
    const std::size_t slen = mock_exchange_respond(sstream, sizeof(sstream),
                                                    cstream, coff, /*start_seq=*/1);
    ASSERT(slen > 0, "soup_exchange_responded");

    // Klient konsumuje cały strumień TCP (3 pakiety na raz).
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

    // #139 Login Rejected — parsuj powod odmowy.
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
// Main
// =====================================================

int main() {
    printf("=== HFT Infrastructure Lab — Integration Tests ===\n");

    test_itch_parser();
    test_itch_book();
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
    test_cmo();
    test_zscore();
    test_tsi();
    test_dpo();
    test_kama();
    test_linreg();
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