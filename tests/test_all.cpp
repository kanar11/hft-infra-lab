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
#include "../strategy/market_maker.hpp"
#include "../fix-protocol/fix_parser.hpp"
#include "../fix-protocol/fix_session.hpp"
#include "../ouch-protocol/ouch_protocol.hpp"
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
    test_market_maker();
    test_fix();
    test_fix_session();
    test_ouch();
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