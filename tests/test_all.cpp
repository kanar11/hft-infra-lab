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
#include "../strategy/ensemble.hpp"
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

    // #164 liquidity_within (N ticks od best).
    itch::ITCHOrderBook lw;
    lw.on_add(1, 'S', 100.00, 100); lw.on_add(2, 'S', 100.01, 200); lw.on_add(3, 'S', 100.05, 300);
    ASSERT(lw.liquidity_within('S', 1) == 300, "itchbook_liq_within_1");  // 10000+10001
    ASSERT(lw.liquidity_within('S', 5) == 600, "itchbook_liq_within_5");  // + 10005

    // #174 total_shares + level_count (rozmiar i grubosc ksiazki).
    ASSERT(lw.total_shares('S') == 600, "itchbook_total_shares_ask");     // 100+200+300
    ASSERT(lw.level_count('S') == 3, "itchbook_level_count_ask");
    ASSERT(lw.total_shares('B') == 0 && lw.level_count('B') == 0, "itchbook_empty_bid_side");

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