/*
 * orderbook_pro_demo — testy + benchmark dla FullOrderBook.
 *
 * Pokrywa wszystkie ścieżki API:
 *   - LIMIT add/cancel/modify (priority preserve vs lose)
 *   - IOC/FOK lifecycle
 *   - POST_ONLY reject when crossing
 *   - ICEBERG: hidden refresh + priority loss
 *   - STP policies
 *   - L1/L2 views + microprice + imbalance
 *   - queue_position
 *   - Trade tape + VWAP
 *   - Snapshot serialize/load roundtrip
 *
 * Plus benchmark: M add+cancel ops, sortowane percentyle.
 *
 * Compile: g++ -O2 -std=c++17 -Wall -Wextra -o orderbook_pro_demo
 *          orderbook_pro_demo.cpp
 */
#include "orderbook_pro.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>


static int tests_passed = 0;
static int tests_failed = 0;
static int tests_total  = 0;

#define ASSERT(cond, msg) do { \
    ++tests_total; \
    if (!(cond)) { \
        std::printf("  FAIL: %s\n", msg); \
        ++tests_failed; \
    } else { \
        ++tests_passed; \
    } \
} while (0)


using Book = orderbook_pro::FullOrderBook<16384, 8192>;
using namespace orderbook_pro;


// ──────────────────────────────────────────────
// L1: basic add + match
// ──────────────────────────────────────────────

void test_basic_add_and_match() {
    Book b;
    auto id1 = b.submit(Side::BUY,  10000, 100);
    auto id2 = b.submit(Side::SELL, 10100, 100);
    ASSERT(id1 > 0 && id2 > 0,                   "basic_add_ids_positive");
    ASSERT(b.best_bid_ticks() == 10000,           "basic_best_bid");
    ASSERT(b.best_ask_ticks() == 10100,           "basic_best_ask");
    ASSERT(b.stats().total_fills == 0,            "basic_no_cross_no_fill");

    // Aggressive buy w book → execute against ask
    auto id3 = b.submit(Side::BUY, 10100, 100);
    (void)id3;
    ASSERT(b.stats().total_fills >= 1,            "basic_cross_makes_fill");
    ASSERT(b.stats().total_volume == 100,         "basic_cross_volume_100");
    // Po fill'u brak asków
    ASSERT(!b.has_ask(),                          "basic_ask_gone_after_full_fill");
    ASSERT(b.best_bid_ticks() == 10000,           "basic_best_bid_unchanged");
}


void test_cancel() {
    Book b;
    auto id = b.submit(Side::BUY, 10000, 100);
    ASSERT(b.has_bid(),                       "cancel_pre_has_bid");
    ASSERT(b.cancel(id),                      "cancel_returns_true");
    ASSERT(!b.has_bid(),                      "cancel_post_no_bid");
    ASSERT(b.stats().total_orders_cancelled == 1, "cancel_stats_incremented");
    ASSERT(!b.cancel(id),                     "cancel_unknown_returns_false");
}


void test_modify_priority_preserve_down_only() {
    Book b;
    auto id1 = b.submit(Side::BUY, 10000, 100);
    auto id2 = b.submit(Side::BUY, 10000, 200);  // ten join'uje po id1 (FIFO)
    ASSERT(b.queue_position(id1) == 0,        "modify_id1_at_head");
    ASSERT(b.queue_position(id2) == 1,        "modify_id2_after_id1");

    // Zmniejszamy id1 z qty 100 do 80 — same price, qty DOWN → priority OK
    auto rid = b.modify(id1, 10000, 80);
    ASSERT(rid == id1,                        "modify_returns_same_id");
    ASSERT(b.queue_position(id1) == 0,        "modify_priority_preserved");
    auto* o1 = b.find_order(id1);
    ASSERT(o1 && o1->total_qty == 80,         "modify_qty_updated");
}


// ──────────────────────────────────────────────
// IOC / FOK / POST_ONLY
// ──────────────────────────────────────────────

void test_ioc_takes_what_can_then_cancels() {
    Book b;
    b.submit(Side::SELL, 10100, 50);    // tylko 50 dostępne
    auto buy = b.submit(Side::BUY, 10100, 100, OrderType::IOC);
    (void)buy;
    // 50 wykonane, 50 cancelled — IOC nie zostaje w księdze
    ASSERT(b.stats().total_volume == 50,      "ioc_executes_available");
    ASSERT(!b.has_bid(),                       "ioc_no_residual_in_book");
}


void test_fok_kills_if_not_fully_fillable() {
    Book b;
    b.submit(Side::SELL, 10100, 50);
    RejectReason rr = RejectReason::NONE;
    auto buy = b.submit(Side::BUY, 10100, 100, OrderType::FOK,
                        TimeInForce::FOK, 0, 0, 0, &rr);
    ASSERT(buy == 0,                                "fok_returns_zero");
    ASSERT(rr == RejectReason::FOK_NOT_FILLABLE,    "fok_reason");
    // SELL nadal w księdze, nic się nie wykonało
    ASSERT(b.has_ask(),                              "fok_seller_intact");
    ASSERT(b.stats().total_fills == 0,               "fok_no_fills");
}


void test_post_only_rejects_when_would_cross() {
    Book b;
    b.submit(Side::SELL, 10100, 100);
    RejectReason rr = RejectReason::NONE;
    auto cross_buy = b.submit(Side::BUY, 10100, 50, OrderType::POST_ONLY,
                              TimeInForce::DAY, 0, 0, 0, &rr);
    ASSERT(cross_buy == 0,                              "post_only_reject");
    ASSERT(rr == RejectReason::POST_ONLY_WOULD_CROSS,   "post_only_reason");
    // SELL nietknięty
    ASSERT(b.has_ask(),                                  "post_only_no_match");

    // POST_ONLY na NIEKRZYŻUJĄCYM levelu — przechodzi
    auto safe_buy = b.submit(Side::BUY, 10000, 50, OrderType::POST_ONLY);
    ASSERT(safe_buy > 0,                                  "post_only_safe_accepts");
}


// ──────────────────────────────────────────────
// ICEBERG
// ──────────────────────────────────────────────

void test_iceberg_displays_partial() {
    Book b;
    // Iceberg sell 1000, displayed 100
    auto ice = b.submit(Side::SELL, 10100, 1000, OrderType::ICEBERG,
                        TimeInForce::DAY, 0, 0, /*displayed=*/100);
    ASSERT(ice > 0, "iceberg_accepted");
    ASSERT(b.total_volume_at_price(10100) == 100, "iceberg_only_displayed");
    auto* o = b.find_order(ice);
    ASSERT(o && o->total_qty == 1000 && o->displayed_qty == 100,
           "iceberg_qty_split");
}


// ──────────────────────────────────────────────
// L1/L2 + microprice + imbalance
// ──────────────────────────────────────────────

void test_top_of_book() {
    Book b;
    b.submit(Side::BUY,  10000, 200);
    b.submit(Side::BUY,  10000, 100);   // 2 zlecenia na tym samym poziomie
    b.submit(Side::SELL, 10100, 150);

    auto tob = b.top_of_book();
    ASSERT(tob.best_bid_ticks == 10000,  "tob_bid");
    ASSERT(tob.best_ask_ticks == 10100,  "tob_ask");
    ASSERT(tob.bid_qty == 300,           "tob_bid_qty_summed");
    ASSERT(tob.bid_count == 2,           "tob_bid_count");
    ASSERT(tob.ask_qty == 150,           "tob_ask_qty");
    ASSERT(tob.ask_count == 1,           "tob_ask_count");
}


void test_microprice_skews_toward_smaller_side() {
    Book b;
    b.submit(Side::BUY,  10000, 100);
    b.submit(Side::SELL, 10100, 900);     // ask thicker → micro w stronę bid
    const auto mid   = b.mid_ticks();
    const auto micro = b.microprice_ticks();
    ASSERT(mid == 10050,                  "micro_mid_50_50");
    // micro = (bid*ask_qty + ask*bid_qty)/(bid+ask)
    //       = (10000*900 + 10100*100)/(900+100) = 10010
    ASSERT(micro == 10010,                "micro_weighted_lower_when_ask_thicker");
    ASSERT(micro < mid,                    "micro_below_mid");
}


void test_imbalance_bps() {
    Book b;
    b.submit(Side::BUY,  10000, 700);
    b.submit(Side::SELL, 10100, 300);
    // imbalance = (700-300)/1000 * 10000 = +4000
    ASSERT(b.imbalance_bps() == 4000,     "imbalance_buy_heavy_4000");
}


void test_depth_l2() {
    Book b;
    b.submit(Side::BUY, 10000, 100);
    b.submit(Side::BUY, 9999,  200);
    b.submit(Side::BUY, 9998,  300);
    b.submit(Side::SELL, 10100, 150);
    b.submit(Side::SELL, 10101, 250);

    DepthLevel bids[5], asks[5];
    std::int32_t bn = 0, an = 0;
    b.depth(5, bids, asks, &bn, &an);

    ASSERT(bn == 3 && an == 2,                            "depth_counts");
    ASSERT(bids[0].price_ticks == 10000 && bids[0].qty == 100, "depth_bid_0");
    ASSERT(bids[1].price_ticks == 9999  && bids[1].qty == 200, "depth_bid_1");
    ASSERT(bids[2].price_ticks == 9998  && bids[2].qty == 300, "depth_bid_2");
    ASSERT(asks[0].price_ticks == 10100 && asks[0].qty == 150, "depth_ask_0");
    ASSERT(asks[1].price_ticks == 10101 && asks[1].qty == 250, "depth_ask_1");
}


// ──────────────────────────────────────────────
// Queue position
// ──────────────────────────────────────────────

void test_queue_position_fifo() {
    Book b;
    auto a = b.submit(Side::BUY, 10000, 100);
    auto c = b.submit(Side::BUY, 10000, 200);
    auto d = b.submit(Side::BUY, 10000, 300);
    ASSERT(b.queue_position(a) == 0,   "queue_a_first");
    ASSERT(b.queue_position(c) == 1,   "queue_c_second");
    ASSERT(b.queue_position(d) == 2,   "queue_d_third");
    b.cancel(c);   // d powinno awansować
    ASSERT(b.queue_position(a) == 0,   "queue_a_still_first_after_cancel");
    ASSERT(b.queue_position(d) == 1,   "queue_d_promoted");
}


// ──────────────────────────────────────────────
// Trade tape + VWAP
// ──────────────────────────────────────────────

void test_trade_tape_and_vwap() {
    Book b;
    b.submit(Side::SELL, 10100, 50);
    b.submit(Side::SELL, 10110, 50);
    b.submit(Side::BUY,  10120, 100);    // przechodzi 50 @ 10100 + 50 @ 10110

    Trade tape[8];
    auto got = b.recent_trades(tape, 8);
    ASSERT(got == 2,                  "tape_2_trades");
    // VWAP = (10100*50 + 10110*50) / 100 = 10105
    ASSERT(b.tape_vwap_ticks() == 10105, "tape_vwap");
}


// ──────────────────────────────────────────────
// STP
// ──────────────────────────────────────────────

void test_stp_cancel_newest() {
    Book b;
    b.set_stp_policy(SelfTradePrevention::CANCEL_NEWEST);
    // Maker z client_id=42
    b.submit(Side::SELL, 10100, 100, OrderType::LIMIT,
             TimeInForce::DAY, 0, /*client_id=*/42);
    // Taker z tego samego konta
    auto t = b.submit(Side::BUY, 10100, 100, OrderType::LIMIT,
                      TimeInForce::DAY, 0, /*client_id=*/42);
    (void)t;
    // STP zabija takera → fill ZERO, seller nadal w księdze
    ASSERT(b.stats().total_self_trade_blocks >= 1, "stp_block_counter");
    ASSERT(b.has_ask(),                            "stp_maker_intact");
}


// ──────────────────────────────────────────────
// Snapshot roundtrip
// ──────────────────────────────────────────────

void test_snapshot_roundtrip() {
    Book b;
    b.submit(Side::BUY,  10000, 100, OrderType::LIMIT, TimeInForce::DAY, 1001);
    b.submit(Side::BUY,  9999,  200, OrderType::LIMIT, TimeInForce::DAY, 1002);
    b.submit(Side::SELL, 10100, 150, OrderType::LIMIT, TimeInForce::DAY, 1003);

    const auto need = b.snapshot_size_estimate();
    std::vector<std::uint8_t> buf(need + 64);
    const auto written = b.serialize_snapshot(buf.data(), buf.size());
    ASSERT(written > 0,                      "snapshot_written");

    Book b2;
    ASSERT(b2.load_snapshot(buf.data(), written), "snapshot_load_ok");
    ASSERT(b2.best_bid_ticks() == 10000,           "snap_bid_restored");
    ASSERT(b2.best_ask_ticks() == 10100,           "snap_ask_restored");
    ASSERT(b2.find_order(1001) != nullptr,         "snap_order_1001_present");
    ASSERT(b2.find_order(1002) != nullptr,         "snap_order_1002_present");
    ASSERT(b2.find_order(1003) != nullptr,         "snap_order_1003_present");
}


// ──────────────────────────────────────────────
// Pre-trade reject reasons
// ──────────────────────────────────────────────

// ──────────────────────────────────────────────
// Auction matching (opening / closing cross)
// ──────────────────────────────────────────────

void test_auction_clearing_price() {
    Book b;
    // Auction mode: submit pomija match, orders sit w księdze do batch cross.
    b.enter_auction_mode();
    b.submit(Side::BUY, 10010, 50);
    b.submit(Side::BUY, 10005, 100);
    b.submit(Side::BUY, 10000, 200);
    b.submit(Side::SELL, 9995,  100);
    b.submit(Side::SELL, 10005, 50);
    b.submit(Side::SELL, 10010, 200);
    b.exit_auction_mode();

    // Po sit-and-wait → run_auction znajdzie clearing 10005 (matched=150)
    auto r = b.run_auction();
    ASSERT(r.executed,                                "auction_executed");
    ASSERT(r.clearing_price_ticks > 0,                "auction_clearing_set");
    ASSERT(r.matched_qty > 0,                         "auction_matched_qty");
    ASSERT(b.stats().total_auctions_executed == 1,    "auction_stat_counter");
}

// ──────────────────────────────────────────────
// L2 delta queue
// ──────────────────────────────────────────────

void test_delta_queue_basic() {
    Book b;
    b.enable_delta_queue(true);
    b.submit(Side::BUY,  10000, 100);
    b.submit(Side::SELL, 10100, 200);
    b.submit(Side::BUY,  9999,  50);

    Book::DeltaMessage msgs[8];
    auto n = b.pop_delta_queue(msgs, 8);
    ASSERT(n == 3,                                "delta_queue_3_msgs");
    ASSERT(msgs[0].type == 'A' && msgs[0].side == 'B' && msgs[0].price_ticks == 10000,
           "delta_msg_0_add_bid");
    ASSERT(msgs[1].type == 'A' && msgs[1].side == 'S' && msgs[1].price_ticks == 10100,
           "delta_msg_1_add_ask");
    ASSERT(msgs[2].type == 'A' && msgs[2].side == 'B' && msgs[2].price_ticks == 9999,
           "delta_msg_2_add_bid_lower");
    ASSERT(msgs[0].sequence_no < msgs[1].sequence_no &&
           msgs[1].sequence_no < msgs[2].sequence_no,
           "delta_seq_monotonic");

    // Pop drugi raz — kolejka pusta
    ASSERT(b.pop_delta_queue(msgs, 8) == 0, "delta_queue_drained");
}

void test_delta_wire_format() {
    Book::DeltaMessage d{'A', 'B', 10500, 250, 42};
    std::uint8_t buf[Book::DELTA_WIRE_SIZE];
    auto written = Book::serialize_delta(d, buf);
    ASSERT(written == Book::DELTA_WIRE_SIZE,        "delta_wire_size");

    Book::DeltaMessage d2;
    ASSERT(Book::deserialize_delta(buf, written, d2),    "delta_deserialize_ok");
    ASSERT(d2.type == 'A' && d2.side == 'B',              "delta_type_side");
    ASSERT(d2.price_ticks == 10500 && d2.new_qty == 250,  "delta_price_qty");
    ASSERT(d2.sequence_no == 42,                          "delta_seq");
}

void test_delta_on_cancel() {
    Book b;
    b.enable_delta_queue(true);
    auto id = b.submit(Side::BUY, 10000, 100);
    Book::DeltaMessage tmp[4];
    b.pop_delta_queue(tmp, 4);   // wyrzuć Add

    b.cancel(id);
    Book::DeltaMessage msgs[4];
    auto n = b.pop_delta_queue(msgs, 4);
    ASSERT(n == 1,                            "cancel_delta_1");
    ASSERT(msgs[0].type == 'D',                "cancel_delta_type_D");
    ASSERT(msgs[0].new_qty == 0,               "cancel_delta_qty_0");
}

// ──────────────────────────────────────────────
// Spread + microstructure analytics
// ──────────────────────────────────────────────

void test_spread_analytics() {
    Book b;
    b.submit(Side::BUY,  10000, 100);
    b.submit(Side::SELL, 10010, 100);
    ASSERT(b.spread_ticks() == 10,             "spread_10_ticks");
    // mid = 10005, sprd = 10. half_spread = (10*10000/2)/10005 = ~4
    const auto hsb = b.half_spread_bps();
    ASSERT(hsb >= 4 && hsb <= 6,                "half_spread_bps_reasonable");
    ASSERT(b.weighted_mid_ticks() == b.microprice_ticks(),
           "weighted_mid_alias_for_microprice");
}

// ──────────────────────────────────────────────
// Mass quote (atomic 2-sided MM submission)
// ──────────────────────────────────────────────

void test_mass_quote_atomic() {
    Book b;
    Book::Quote q[2] = {
        {Side::BUY,  10000, 100},
        {Side::SELL, 10010, 100},
    };
    std::uint64_t ids[2];
    auto n = b.mass_quote(q, 2, /*client=*/42, ids);
    ASSERT(n == 2,                                "mq_2_accepted");
    ASSERT(ids[0] > 0 && ids[1] > 0,              "mq_ids_set");
    ASSERT(b.best_bid_ticks() == 10000,           "mq_bid");
    ASSERT(b.best_ask_ticks() == 10010,           "mq_ask");
    ASSERT(b.stats().total_mass_quotes == 1,      "mq_stat");
}

void test_mass_quote_rejected_if_crosses() {
    Book b;
    b.submit(Side::SELL, 10000, 100);   // resting ask
    Book::Quote q[2] = {
        {Side::BUY,  10005, 100},        // crosses!
        {Side::SELL, 10010, 100},
    };
    auto n = b.mass_quote(q, 2, /*client=*/42, nullptr);
    ASSERT(n == 0,                           "mq_atomic_reject_all_or_none");
    // Resting sell intakt — nic z mass_quote nie weszło
    ASSERT(b.best_ask_ticks() == 10000,      "mq_book_unmodified");
}

// ──────────────────────────────────────────────
// Liquidity snapshot
// ──────────────────────────────────────────────

void test_liquidity_snapshot() {
    Book b;
    b.submit(Side::BUY,  10000, 100);
    b.submit(Side::BUY,  9999,  200);
    b.submit(Side::SELL, 10010, 50);
    b.submit(Side::SELL, 10011, 150);

    auto s = b.liquidity_snapshot();
    ASSERT(s.bid_count == 2,                    "snap_bid_count");
    ASSERT(s.ask_count == 2,                    "snap_ask_count");
    ASSERT(s.spread_ticks == 10,                "snap_spread");
    ASSERT(s.total_bid_volume == 300,           "snap_bid_vol");
    ASSERT(s.total_ask_volume == 200,           "snap_ask_vol");
}

// ──────────────────────────────────────────────
// BookCluster — multi-symbol
// ──────────────────────────────────────────────

void test_book_cluster_basic() {
    orderbook_pro::BookCluster<8, 16384, 4096> cluster;
    ASSERT(cluster.register_symbol("AAPL"),     "cluster_register_AAPL");
    ASSERT(cluster.register_symbol("MSFT"),     "cluster_register_MSFT");
    ASSERT(!cluster.register_symbol("AAPL"),    "cluster_no_duplicate");
    ASSERT(cluster.active_symbol_count() == 2,   "cluster_2_symbols");

    auto* aapl = cluster.book("AAPL");
    auto* msft = cluster.book("MSFT");
    ASSERT(aapl != nullptr && msft != nullptr,  "cluster_books_distinct");
    ASSERT(aapl != msft,                         "cluster_books_independent");

    // LEVELS=16384 — ceny muszą się mieścić < 16384
    aapl->submit(Side::BUY,  10000, 100);
    aapl->submit(Side::SELL, 10010, 100);
    msft->submit(Side::BUY,  11000, 50);
    msft->submit(Side::SELL, 11020, 50);

    // Cross-symbol aggregations
    ASSERT(cluster.total_active_orders() == 4,  "cluster_total_orders");
    // Avg spread: (10 + 20) / 2 = 15
    ASSERT(cluster.avg_spread_ticks() == 15,    "cluster_avg_spread");
}

void test_book_cluster_unknown_symbol() {
    orderbook_pro::BookCluster<4> cluster;
    cluster.register_symbol("AAPL");
    ASSERT(cluster.book("GOOG") == nullptr,     "cluster_unknown_nullptr");
}

// ──────────────────────────────────────────────
// HIDDEN orders — dark-pool semantics
// ──────────────────────────────────────────────

void test_hidden_not_in_l1_or_l2() {
    Book b;
    auto h = b.submit(Side::SELL, 10100, 500, OrderType::HIDDEN);
    ASSERT(h > 0,                                "hidden_accepted");
    // L1 — HIDDEN nie pojawia się w top of book
    ASSERT(!b.has_ask(),                          "hidden_no_visible_ask");
    auto tob = b.top_of_book();
    ASSERT(tob.best_ask_ticks == NO_ASK_TICKS,    "hidden_tob_ask_sentinel");
    // L2 depth — pusta strona ask
    DepthLevel bids[5], asks[5];
    std::int32_t bn=0, an=0;
    b.depth(5, bids, asks, &bn, &an);
    ASSERT(an == 0,                               "hidden_no_l2_ask");
    // total_volume_at_price() — visible = 0 (displayed_qty = 0)
    ASSERT(b.total_volume_at_price(10100) == 0,   "hidden_visible_zero");
}

void test_hidden_does_not_match_continuous() {
    Book b;
    // HIDDEN sell @ 10100, size 500
    b.submit(Side::SELL, 10100, 500, OrderType::HIDDEN);
    // Aggressive BUY @ 10100 size 100 — chciałby zjeść hidden, ale
    // dark-pool nie matchuje continuous → BUY zostaje w księdze
    auto buy = b.submit(Side::BUY, 10100, 100);
    ASSERT(buy > 0,                               "hidden_buy_accepted");
    ASSERT(b.stats().total_fills == 0,            "hidden_no_continuous_fill");
    ASSERT(b.best_bid_ticks() == 10100,           "hidden_buy_resting");
}

// ──────────────────────────────────────────────
// Halt / resume
// ──────────────────────────────────────────────

void test_halt_rejects_new_orders() {
    Book b;
    b.halt("LULD_BREACH");
    ASSERT(b.is_halted(),                                "halt_state_set");
    ASSERT(std::strcmp(b.halt_reason(), "LULD_BREACH") == 0, "halt_reason_set");

    RejectReason rr = RejectReason::NONE;
    auto id = b.submit(Side::BUY, 10000, 100, OrderType::LIMIT,
                        TimeInForce::DAY, 0, 0, 0, &rr);
    ASSERT(id == 0,                              "halt_rejects_submit");
    ASSERT(rr == RejectReason::HALTED,            "halt_reject_reason");
}

void test_resume_allows_orders() {
    Book b;
    b.halt("HALT");
    b.resume();
    ASSERT(!b.is_halted(),                       "resume_state_clear");
    auto id = b.submit(Side::BUY, 10000, 100);
    ASSERT(id > 0,                                "resume_allows_submit");
}

void test_halt_does_not_block_cancel() {
    Book b;
    auto id = b.submit(Side::BUY, 10000, 100);
    b.halt("EMERGENCY");
    // Cancel zlecenia POWINNO działać nawet podczas haltu (compliance:
    // trader musi móc się wycofać).
    ASSERT(b.cancel(id),                          "halt_cancel_works");
    ASSERT(!b.has_bid(),                          "halt_cancel_removed");
}

// ──────────────────────────────────────────────
// MIN_QTY constraint
// ──────────────────────────────────────────────

void test_min_qty_rejected_when_not_met() {
    Book b;
    b.submit(Side::SELL, 10100, 50);   // tylko 50 dostępne
    // BUY 100 z min_qty=75 — fillable=50 < 75 → REJECT
    RejectReason rr = RejectReason::NONE;
    auto id = b.submit(Side::BUY, 10100, 100, OrderType::LIMIT,
                        TimeInForce::DAY, 0, 0, 0, &rr, /*min_qty=*/75);
    ASSERT(id == 0,                                "min_qty_reject");
    ASSERT(rr == RejectReason::MIN_QTY_NOT_MET,    "min_qty_reason");
    // SELL pozostaje
    ASSERT(b.has_ask(),                            "min_qty_book_intact");
}

void test_min_qty_accepted_when_met() {
    Book b;
    b.submit(Side::SELL, 10100, 100);
    // BUY 100 z min_qty=50 — fillable=100 ≥ 50 → ACCEPT
    auto id = b.submit(Side::BUY, 10100, 100, OrderType::LIMIT,
                        TimeInForce::DAY, 0, 0, 0, nullptr, /*min_qty=*/50);
    ASSERT(id > 0,                                  "min_qty_accept");
    ASSERT(b.stats().total_volume == 100,           "min_qty_filled");
}

// ──────────────────────────────────────────────
// Audit log (SEC 17a-4 compliance)
// ──────────────────────────────────────────────

void test_audit_log_records_lifecycle() {
    Book b;
    b.enable_audit_log(true);
    auto id = b.submit(Side::BUY, 10000, 100);
    b.cancel(id);

    Book::AuditRecord recs[8];
    auto n = b.pop_audit_records(recs, 8);
    ASSERT(n == 2,                                  "audit_2_records");
    ASSERT(recs[0].event == EventType::ACCEPT,      "audit_0_accept");
    ASSERT(recs[1].event == EventType::CANCEL,      "audit_1_cancel");
    ASSERT(recs[0].seq_no < recs[1].seq_no,         "audit_seq_monotonic");
    ASSERT(recs[0].order_id == id,                  "audit_order_id");
    ASSERT(recs[1].order_id == id,                  "audit_cancel_id");
    // Pop ponownie — pusta
    ASSERT(b.pop_audit_records(recs, 8) == 0,       "audit_drained");
}

void test_audit_log_records_reject() {
    Book b;
    b.enable_audit_log(true);
    b.submit(Side::BUY, -1, 100);   // bad price → REJECT
    Book::AuditRecord recs[4];
    auto n = b.pop_audit_records(recs, 4);
    ASSERT(n == 1,                                  "audit_reject_1");
    ASSERT(recs[0].event == EventType::REJECT,      "audit_reject_event");
}

void test_audit_disabled_by_default() {
    Book b;
    b.submit(Side::BUY, 10000, 100);
    ASSERT(b.audit_log_size() == 0,                "audit_disabled_no_records");
}

// ──────────────────────────────────────────────
// LULD circuit breaker
// ──────────────────────────────────────────────

void test_luld_rejects_out_of_band() {
    Book b;
    b.set_luld_bands(9900, 10100, /*auto_halt=*/false);
    RejectReason rr = RejectReason::NONE;
    auto id = b.submit(Side::BUY, 10500, 100, OrderType::LIMIT,
                        TimeInForce::DAY, 0, 0, 0, &rr);
    ASSERT(id == 0,                                  "luld_reject");
    ASSERT(rr == RejectReason::LULD_BAND_BREACH,     "luld_reason");
    ASSERT(b.luld_breaches() == 1,                    "luld_breach_count");
}

void test_luld_accepts_within_band() {
    Book b;
    b.set_luld_bands(9900, 10100, false);
    auto id = b.submit(Side::BUY, 10000, 100);
    ASSERT(id > 0,                                    "luld_within_ok");
}

void test_luld_auto_halt() {
    Book b;
    b.set_luld_bands(9900, 10100, /*auto_halt=*/true);
    b.submit(Side::BUY, 10500, 100);   // out-of-band → halt
    ASSERT(b.is_halted(),                            "luld_auto_halted");
    ASSERT(std::strcmp(b.halt_reason(), "LULD_BREACH") == 0,
           "luld_halt_reason");
    // Po halt nawet good order rejected
    RejectReason rr = RejectReason::NONE;
    auto id2 = b.submit(Side::BUY, 10000, 100, OrderType::LIMIT,
                         TimeInForce::DAY, 0, 0, 0, &rr);
    ASSERT(id2 == 0,                                  "luld_halted_blocks");
    ASSERT(rr == RejectReason::HALTED,                "luld_halt_reason_post");
}

// ──────────────────────────────────────────────
// MIFID II RTS27/28 metrics
// ──────────────────────────────────────────────

void test_mifid_effective_spread() {
    Book b;
    b.enable_mifid_metrics(true);
    b.submit(Side::BUY,  10000, 100);
    b.submit(Side::SELL, 10010, 100);  // mid = 10005
    // Aggressive BUY @ 10010 — exec @ 10010, mid=10005 → diff=5
    b.submit(Side::BUY, 10010, 50);
    auto m = b.get_mifid_metrics();
    ASSERT(m.num_executions >= 1,                    "mifid_exec_count");
    ASSERT(m.total_volume > 0,                       "mifid_volume");
    ASSERT(m.sum_effective_spread > 0,               "mifid_eff_spread_positive");
}

void test_mifid_disabled_no_tracking() {
    Book b;
    b.submit(Side::SELL, 10100, 100);
    b.submit(Side::BUY, 10100, 100);  // execution
    auto m = b.get_mifid_metrics();
    ASSERT(m.num_executions == 0,                    "mifid_disabled");
}

// ──────────────────────────────────────────────
// Quote stuffing detection (SEC Rule 15c3-5)
// ──────────────────────────────────────────────

void test_stuffing_below_threshold_not_flagged() {
    Book b;
    b.set_stuffing_threshold(10);   // 10 cancels triggers flag
    for (int i = 0; i < 5; ++i) {
        auto id = b.submit(Side::BUY, 10000, 100, OrderType::LIMIT,
                            TimeInForce::DAY, 0, /*cid=*/42);
        b.cancel(id);
    }
    ASSERT(!b.is_stuffing_flagged(42),               "stuffing_under_threshold");
    ASSERT(b.total_stuffing_flags() == 0,            "stuffing_no_flags_yet");
}

void test_stuffing_above_threshold_flagged() {
    Book b;
    b.set_stuffing_threshold(5);
    for (int i = 0; i < 20; ++i) {
        auto id = b.submit(Side::BUY, 10000, 100, OrderType::LIMIT,
                            TimeInForce::DAY, 0, /*cid=*/77);
        b.cancel(id);
    }
    ASSERT(b.is_stuffing_flagged(77),                "stuffing_flag_set");
    ASSERT(b.total_stuffing_flags() >= 1,            "stuffing_total_count");
}

void test_stuffing_clear_flag() {
    Book b;
    b.set_stuffing_threshold(3);
    for (int i = 0; i < 10; ++i) {
        auto id = b.submit(Side::BUY, 10000, 100, OrderType::LIMIT,
                            TimeInForce::DAY, 0, /*cid=*/99);
        b.cancel(id);
    }
    ASSERT(b.is_stuffing_flagged(99),                "stuffing_flagged_99");
    b.clear_stuffing_flag(99);
    ASSERT(!b.is_stuffing_flagged(99),               "stuffing_flag_cleared");
}

// ──────────────────────────────────────────────
// Per-account exposure tracking
// ──────────────────────────────────────────────

void test_exposure_open_buy() {
    Book b;
    b.submit(Side::BUY, 10000, 100, OrderType::LIMIT,
              TimeInForce::DAY, 0, /*cid=*/42);
    b.submit(Side::BUY, 9999, 200, OrderType::LIMIT,
              TimeInForce::DAY, 0, /*cid=*/42);
    auto ex = b.get_account_exposure(42);
    ASSERT(ex.open_buy_qty == 300,                   "exposure_buy_accumulated");
    ASSERT(ex.open_sell_qty == 0,                    "exposure_no_sell");
    ASSERT(ex.orders_submitted == 2,                 "exposure_submit_count");
}

void test_exposure_filled_net_qty() {
    Book b;
    b.submit(Side::SELL, 10100, 100, OrderType::LIMIT,
              TimeInForce::DAY, 0, /*cid=*/77);
    b.submit(Side::BUY, 10100, 100, OrderType::LIMIT,
              TimeInForce::DAY, 0, /*cid=*/42);
    auto ex42 = b.get_account_exposure(42);
    auto ex77 = b.get_account_exposure(77);
    ASSERT(ex42.filled_net_qty == 100,               "exposure_42_long");
    ASSERT(ex77.filled_net_qty == -100,              "exposure_77_short");
    ASSERT(ex42.fills_received >= 1,                 "exposure_42_fills");
    ASSERT(ex77.fills_received >= 1,                 "exposure_77_fills");
}

void test_exposure_cancel_releases_open_qty() {
    Book b;
    auto id = b.submit(Side::BUY, 10000, 100, OrderType::LIMIT,
                        TimeInForce::DAY, 0, /*cid=*/42);
    auto ex_before = b.get_account_exposure(42);
    ASSERT(ex_before.open_buy_qty == 100,            "exposure_pre_cancel");
    b.cancel(id);
    auto ex_after = b.get_account_exposure(42);
    ASSERT(ex_after.open_buy_qty == 0,               "exposure_post_cancel");
    ASSERT(ex_after.orders_cancelled == 1,           "exposure_cancel_count");
}

// ──────────────────────────────────────────────
// Trade size distribution
// ──────────────────────────────────────────────

void test_trade_size_distribution_classifies() {
    Book b;
    // Małe: 50
    b.submit(Side::SELL, 10100, 50);
    b.submit(Side::BUY,  10100, 50);
    // Średnie: 500
    b.submit(Side::SELL, 10100, 500);
    b.submit(Side::BUY,  10100, 500);
    // Duże: 5000
    b.submit(Side::SELL, 10100, 5000);
    b.submit(Side::BUY,  10100, 5000);

    auto d = b.get_size_distribution();
    ASSERT(d.small_count >= 1,                        "size_small_counted");
    ASSERT(d.medium_count >= 1,                       "size_medium_counted");
    ASSERT(d.large_count >= 1,                        "size_large_counted");
    ASSERT(d.small_volume == 50,                       "size_small_vol");
    ASSERT(d.medium_volume == 500,                     "size_medium_vol");
    ASSERT(d.large_volume == 5000,                     "size_large_vol");
}

// ──────────────────────────────────────────────
// TWAP from trade tape
// ──────────────────────────────────────────────

void test_tape_twap() {
    Book b;
    b.submit(Side::SELL, 10000, 100);
    b.submit(Side::BUY,  10000, 100);    // trade @ 10000
    b.submit(Side::SELL, 10010, 200);
    b.submit(Side::BUY,  10010, 200);    // trade @ 10010
    // TWAP: (10000 + 10010) / 2 = 10005 (sztywno, nieważne size)
    ASSERT(b.tape_twap_ticks() == 10005,              "twap_arithmetic_mean");
    // VWAP weighted by qty: (10000*100 + 10010*200) / 300 = 10006
    ASSERT(b.tape_vwap_ticks() == 10006,              "vwap_weighted_by_qty");
}

// ──────────────────────────────────────────────
// Reference price + drift
// ──────────────────────────────────────────────

void test_reference_price_drift() {
    Book b;
    b.set_reference_price(10000);
    b.submit(Side::BUY,  10050, 100);
    b.submit(Side::SELL, 10070, 100);
    // mid = 10060, ref = 10000 → drift = 60/10000 * 10000 = 60 bps
    const auto drift = b.reference_drift_bps();
    ASSERT(drift >= 55 && drift <= 65,                "ref_drift_around_60");
}

void test_reference_price_no_ref_set() {
    Book b;
    b.submit(Side::BUY,  10000, 100);
    b.submit(Side::SELL, 10010, 100);
    ASSERT(!b.has_reference_price(),                  "ref_default_off");
    ASSERT(b.reference_drift_bps() == -1,             "ref_drift_no_ref");
}

// ──────────────────────────────────────────────
// Per-client rejection rate
// ──────────────────────────────────────────────

void test_rejection_rate() {
    Book b;
    // Klient 42: 1 OK + 2 reject (qty=0)
    b.submit(Side::BUY, 10000, 100, OrderType::LIMIT,
              TimeInForce::DAY, 0, /*cid=*/42);
    b.submit(Side::BUY, 10000, 0, OrderType::LIMIT,
              TimeInForce::DAY, 0, /*cid=*/42);
    b.submit(Side::BUY, 10000, -1, OrderType::LIMIT,
              TimeInForce::DAY, 0, /*cid=*/42);
    const double rate = b.rejection_rate(42);
    // 2 rejections / (1 accepted + 2 rejections) = 2/3 ≈ 0.667
    ASSERT(rate > 0.6 && rate < 0.7,                  "rejection_rate_two_thirds");
}

// ──────────────────────────────────────────────
// Top-of-book change tracking
// ──────────────────────────────────────────────

void test_tob_poll_initial() {
    Book b;
    b.submit(Side::BUY, 10000, 100);
    ASSERT(b.poll_tob_change(),                       "tob_initial_change");
    ASSERT(!b.poll_tob_change(),                       "tob_idempotent_after_poll");
}

void test_tob_poll_after_new_quote() {
    Book b;
    b.submit(Side::BUY, 10000, 100);
    b.poll_tob_change();   // consume initial
    // Lepszy bid → TOB zmiana
    b.submit(Side::BUY, 10010, 50);
    ASSERT(b.poll_tob_change(),                       "tob_better_bid_detected");
    ASSERT(!b.poll_tob_change(),                       "tob_drained");
}

void test_tob_poll_unchanged_when_lower_bid() {
    Book b;
    b.submit(Side::BUY, 10010, 100);
    b.poll_tob_change();
    // Gorszy bid (poniżej best) → TOB bez zmian
    b.submit(Side::BUY, 9995, 50);
    ASSERT(!b.poll_tob_change(),                       "tob_no_change_lower_bid");
}

void test_tob_counter_increments() {
    Book b;
    b.submit(Side::BUY,  10000, 100);    b.poll_tob_change();
    b.submit(Side::SELL, 10100, 100);    b.poll_tob_change();
    b.submit(Side::BUY,  10005, 50);     b.poll_tob_change();
    ASSERT(b.stats().total_tob_changes >= 3,           "tob_counter_3_min");
}

// ──────────────────────────────────────────────
// Cross-symbol arbitrage detection
// ──────────────────────────────────────────────

void test_cross_arb_detected() {
    orderbook_pro::BookCluster<8, 16384, 4096> cluster;
    cluster.register_symbol("SPY");
    cluster.register_symbol("IVV");
    auto* spy = cluster.book("SPY");
    auto* ivv = cluster.book("IVV");

    // SPY: bid 10010 → mogę sprzedać tu drogo
    spy->submit(Side::BUY,  10010, 100);
    spy->submit(Side::SELL, 10020, 100);
    // IVV: ask 10005 → mogę kupić tu tanio
    ivv->submit(Side::BUY,  10000, 100);
    ivv->submit(Side::SELL, 10005, 100);
    // ARB: kupić IVV @ 10005, sprzedać SPY @ 10010 = 5 ticki zysku

    orderbook_pro::BookCluster<8, 16384, 4096>::ArbOpportunity opps[4];
    auto n = cluster.detect_cross_arb(opps, 4);
    ASSERT(n >= 1,                                     "arb_at_least_one");
    ASSERT(opps[0].spread_ticks == 5,                  "arb_spread_5_ticks");
    ASSERT(std::strcmp(opps[0].long_symbol, "IVV") == 0,   "arb_long_IVV");
    ASSERT(std::strcmp(opps[0].short_symbol, "SPY") == 0,  "arb_short_SPY");
    ASSERT(cluster.count_cross_arb() == n,             "arb_count_matches");
}

void test_cross_arb_none_when_aligned() {
    orderbook_pro::BookCluster<8, 16384, 4096> cluster;
    cluster.register_symbol("XYZ");
    cluster.register_symbol("ABC");
    auto* xyz = cluster.book("XYZ");
    auto* abc = cluster.book("ABC");

    xyz->submit(Side::BUY,  10000, 100);
    xyz->submit(Side::SELL, 10010, 100);
    // ABC top of book: bid below XYZ ask → brak arb
    abc->submit(Side::BUY,  10000, 100);
    abc->submit(Side::SELL, 10010, 100);

    ASSERT(cluster.count_cross_arb() == 0,             "arb_none_when_aligned");
}

// ──────────────────────────────────────────────
// Sweep-to-fill metrics
// ──────────────────────────────────────────────

void test_sweep_single_level() {
    Book b;
    b.submit(Side::SELL, 10100, 100);
    b.submit(Side::BUY,  10100, 100);   // 1-level sweep
    ASSERT(b.stats().total_sweeps == 1,             "sweep_count_1");
    ASSERT(b.stats().multi_level_sweeps == 0,        "sweep_single_no_multi");
    ASSERT(b.avg_levels_per_sweep() == 1.0,          "sweep_avg_1");
    ASSERT(b.multi_level_sweep_ratio() == 0.0,       "sweep_ratio_0");
}

void test_sweep_multi_level() {
    Book b;
    b.submit(Side::SELL, 10100, 50);
    b.submit(Side::SELL, 10110, 50);
    b.submit(Side::SELL, 10120, 50);
    b.submit(Side::BUY,  10120, 150);   // walks 3 levels
    ASSERT(b.stats().total_sweeps == 1,             "sweep_one_event");
    ASSERT(b.stats().multi_level_sweeps == 1,        "sweep_multi_counted");
    ASSERT(b.stats().sum_levels_touched == 3,        "sweep_3_levels");
    ASSERT(b.avg_levels_per_sweep() == 3.0,          "sweep_avg_3");
    ASSERT(b.multi_level_sweep_ratio() == 1.0,       "sweep_ratio_1");
}

// ──────────────────────────────────────────────
// Multi-level imbalance
// ──────────────────────────────────────────────

void test_imbalance_top_3_levels() {
    Book b;
    // 3 bid levels: 100+200+300 = 600 → bid-heavy
    b.submit(Side::BUY, 10000, 100);
    b.submit(Side::BUY, 9999,  200);
    b.submit(Side::BUY, 9998,  300);
    // 3 ask levels: 50+50+100 = 200
    b.submit(Side::SELL, 10010, 50);
    b.submit(Side::SELL, 10011, 50);
    b.submit(Side::SELL, 10012, 100);
    // depth-3 imbalance = (600 - 200) / 800 × 10000 = 5000
    const auto imb = b.imbalance_bps_n(3);
    ASSERT(imb >= 4800 && imb <= 5200,              "imbalance_n3_buy_heavy_5000");
    // depth-1 (TOB only) = (100 - 50)/150 × 10000 ≈ 3333
    const auto imb1 = b.imbalance_bps_n(1);
    ASSERT(imb1 >= 3000 && imb1 <= 3500,            "imbalance_n1_tob");
}

// ──────────────────────────────────────────────
// Order age statistics
// ──────────────────────────────────────────────

void test_age_stats_record_on_cancel() {
    Book b;
    const std::uint64_t before = b.total_completed_lifecycles();
    auto id = b.submit(Side::BUY, 10000, 100);
    b.cancel(id);
    ASSERT(b.total_completed_lifecycles() == before + 1, "age_lifecycle_inc");
    // avg/max same units — sanity (no negative since u64)
    ASSERT(b.avg_age_ns_at_completion() <= b.max_age_ns_observed(),
                                                        "age_avg_le_max");
}

void test_age_stats_record_on_fill() {
    Book b;
    b.submit(Side::SELL, 10100, 100);
    b.submit(Side::BUY,  10100, 100);   // fill removes maker
    ASSERT(b.total_completed_lifecycles() >= 1,     "age_fill_lifecycle");
}

// ──────────────────────────────────────────────
// Modify priority preservation rules
// ──────────────────────────────────────────────

void test_modify_qty_down_preserves_priority() {
    Book b;
    auto id1 = b.submit(Side::BUY, 10000, 100);
    b.submit(Side::BUY, 10000, 50);   // za nami w FIFO
    (void)b.modify(id1, 10000, 60);   // qty DOWN at same price
    ASSERT(b.stats().priority_preserved_mods == 1, "mod_prio_preserved_1");
    ASSERT(b.stats().priority_lost_mods == 0,       "mod_prio_lost_0");
    // After mod, id1 still on front of FIFO with qty=60
    ASSERT(b.queue_position(id1) == 0,              "mod_keeps_front_pos");
}

void test_modify_price_change_loses_priority() {
    Book b;
    auto id1 = b.submit(Side::BUY, 10000, 100);
    (void)b.modify(id1, 10001, 100);  // price change → new submit
    ASSERT(b.stats().priority_lost_mods == 1,       "mod_price_change_lost");
    ASSERT(b.stats().priority_preserved_mods == 0,  "mod_price_change_no_pres");
}

void test_modify_qty_up_loses_priority() {
    Book b;
    auto id1 = b.submit(Side::BUY, 10000, 100);
    (void)b.modify(id1, 10000, 200);  // qty UP → tail
    ASSERT(b.stats().priority_lost_mods == 1,       "mod_qty_up_lost");
}

void test_priority_loss_ratio() {
    Book b;
    auto a = b.submit(Side::BUY, 10000, 100);
    auto c = b.submit(Side::BUY, 10001, 100);
    (void)b.modify(a, 10000, 50);     // preserved
    (void)b.modify(c, 10002, 100);    // lost
    const double r = b.priority_loss_ratio();
    ASSERT(r > 0.4 && r < 0.6,                      "prio_loss_ratio_half");
}

// ──────────────────────────────────────────────
// TCA: quoted + effective spread
// ──────────────────────────────────────────────

void test_quoted_spread_sampler() {
    Book b;
    b.submit(Side::BUY,  10000, 10);
    b.submit(Side::SELL, 10005, 10);
    b.sample_quoted_spread();
    b.sample_quoted_spread();
    ASSERT(b.stats().total_quoted_spread_samples == 2, "qspread_samples_2");
    ASSERT(b.mean_quoted_spread_ticks() == 5.0,        "qspread_mean_5");
}

void test_quoted_spread_skips_one_sided() {
    Book b;
    b.submit(Side::BUY, 10000, 10);   // tylko bid
    b.sample_quoted_spread();          // skips
    ASSERT(b.stats().total_quoted_spread_samples == 0, "qspread_no_one_sided");
}

void test_effective_spread_recorded_on_fill() {
    Book b;
    b.submit(Side::SELL, 10010, 100);
    b.submit(Side::BUY,  10000, 100);  // ustawia mid = 10005
    // taker BUY na 10010, fillowany na 10010, mid_pre=10005 → eff = 2×5 = 10
    b.submit(Side::BUY,  10010, 100);
    ASSERT(b.stats().total_effective_spread_samples >= 1, "eff_sample_recorded");
    ASSERT(b.mean_effective_spread_ticks() > 0.0,         "eff_mean_positive");
}

// ──────────────────────────────────────────────
// Cancel-to-trade ratio (CTR)
// ──────────────────────────────────────────────

void test_ctr_zero_without_fills() {
    Book b;
    auto id = b.submit(Side::BUY, 10000, 100);
    b.cancel(id);
    ASSERT(b.cancel_to_trade_ratio() == 0.0,        "ctr_zero_no_fills");
}

void test_ctr_after_cancel_and_fill() {
    Book b;
    auto id1 = b.submit(Side::BUY, 10000, 100);
    b.cancel(id1);
    // generate a trade
    b.submit(Side::SELL, 10100, 50);
    b.submit(Side::BUY,  10100, 50);
    const double ctr = b.cancel_to_trade_ratio();
    ASSERT(ctr > 0.0,                                "ctr_positive");
}

// ──────────────────────────────────────────────
// Market impact estimator
// ──────────────────────────────────────────────

void test_predicted_vwap_buy_single_level() {
    Book b;
    b.submit(Side::BUY,  9999, 100);   // ustaw mid
    b.submit(Side::SELL, 10010, 200);
    // Predict buy 100 → wszystko z 10010 → VWAP = 10010
    ASSERT(b.predicted_vwap_ticks(Side::BUY, 100) == 10010, "vwap_single_lvl");
}

void test_predicted_vwap_buy_multi_level() {
    Book b;
    b.submit(Side::BUY,  9999, 100);
    b.submit(Side::SELL, 10010, 50);
    b.submit(Side::SELL, 10012, 50);
    // 100 → 50@10010 + 50@10012 = (500500+500600)/100 = 10011
    ASSERT(b.predicted_vwap_ticks(Side::BUY, 100) == 10011, "vwap_multi_avg");
}

void test_predicted_vwap_partial_liquidity() {
    Book b;
    b.submit(Side::BUY,  9999, 100);
    b.submit(Side::SELL, 10010, 30);
    // Buy 100 ale tylko 30 dostępne — VWAP across just those 30
    ASSERT(b.predicted_vwap_ticks(Side::BUY, 100) == 10010, "vwap_partial");
}

void test_predicted_slippage_buy() {
    Book b;
    b.submit(Side::BUY,  10000, 100);   // bid
    b.submit(Side::SELL, 10010, 100);   // ask. mid=10005
    // Buy 100 fills @ 10010 → slippage = 10010 - 10005 = 5
    ASSERT(b.predicted_slippage_ticks(Side::BUY, 100) == 5, "slip_buy_5");
}

void test_depth_within_offset() {
    Book b;
    b.submit(Side::BUY,  10000, 100);
    b.submit(Side::SELL, 10010, 50);
    b.submit(Side::SELL, 10012, 30);
    b.submit(Side::SELL, 10020, 100);    // poza mid+10 zakresem
    // mid = 10005, offset 10 → cap = 10015. 50+30 = 80.
    ASSERT(b.depth_within_offset(Side::BUY, 10) == 80, "depth_offset_buy");
}

// ──────────────────────────────────────────────
// Quote life + spread compression
// ──────────────────────────────────────────────

void test_quote_life_after_two_polls() {
    Book b;
    b.submit(Side::BUY, 10000, 100);
    b.submit(Side::SELL, 10010, 100);
    (void)b.poll_tob_change();         // count=1, life=0
    b.submit(Side::BUY, 10001, 50);    // bid changed
    (void)b.poll_tob_change();         // count=2, life > 0
    ASSERT(b.stats().total_tob_changes == 2, "tob_changes_2");
    ASSERT(b.total_tob_life_ns() > 0,        "qlife_positive");
}

void test_spread_compression_threshold() {
    Book b;
    b.set_spread_compression_threshold(3);
    b.submit(Side::BUY,  10000, 100);
    b.submit(Side::SELL, 10005, 100);   // spread=5, nie compressed
    (void)b.poll_tob_change();
    b.submit(Side::SELL, 10002, 100);   // spread=2, compressed
    (void)b.poll_tob_change();
    ASSERT(b.spread_compression_count() == 1, "spread_compress_1");
}

// ──────────────────────────────────────────────
// Order flow imbalance / VPIN / quote flicker
// ──────────────────────────────────────────────

void test_flow_imbalance_buy_heavy() {
    Book b;
    b.submit(Side::SELL, 10100, 100);
    b.submit(Side::SELL, 10101, 100);
    b.submit(Side::BUY,  10100, 100);   // taker BUY 100
    b.submit(Side::BUY,  10101, 100);   // taker BUY 100
    ASSERT(b.taker_buy_volume()  == 200, "tbv_200");
    ASSERT(b.taker_sell_volume() == 0,   "tsv_0");
    ASSERT(b.flow_imbalance_bps() == 10000, "flow_imb_full_buy");
    ASSERT(b.vpin_bps() == 10000,        "vpin_full");
}

void test_vpin_balanced() {
    Book b;
    b.submit(Side::SELL, 10100, 100);
    b.submit(Side::BUY,  10100, 100);   // taker BUY 100
    b.submit(Side::BUY,  9900,  100);
    b.submit(Side::SELL, 9900,  100);   // taker SELL 100
    ASSERT(b.taker_buy_volume()  == 100,    "tbv_100");
    ASSERT(b.taker_sell_volume() == 100,    "tsv_100");
    ASSERT(b.vpin_bps() == 0,               "vpin_balanced_0");
}

void test_quote_flicker_no_trade() {
    Book b;
    b.submit(Side::BUY, 10000, 100);
    b.submit(Side::SELL, 10010, 100);
    (void)b.poll_tob_change();              // count=1, no flicker
    b.submit(Side::BUY, 10001, 50);         // TOB change, no trade
    (void)b.poll_tob_change();              // count=2, flicker++
    ASSERT(b.quote_flicker_count() == 1,    "flicker_1");
}

void test_quote_flicker_zero_when_trade_intervenes() {
    Book b;
    b.submit(Side::BUY, 10000, 100);
    b.submit(Side::SELL, 10010, 100);
    (void)b.poll_tob_change();
    // Trade happens between polls (zmienia TOB)
    b.submit(Side::BUY, 10010, 100);        // fill consumes ask, TOB shift
    (void)b.poll_tob_change();
    ASSERT(b.quote_flicker_count() == 0,    "flicker_zero_w_trade");
}

void test_volume_at_price_accumulates() {
    Book b;
    b.submit(Side::SELL, 10100, 100);
    b.submit(Side::SELL, 10100, 50);
    b.submit(Side::BUY,  10100, 80);        // takes from first maker
    b.submit(Side::BUY,  10100, 70);        // takes 20 + 50
    ASSERT(b.volume_at_price(10100) == 150, "vap_accum_150");
}

void test_point_of_control() {
    Book b;
    b.submit(Side::SELL, 10100, 50);  b.submit(Side::BUY, 10100, 50);
    b.submit(Side::SELL, 10200, 200); b.submit(Side::BUY, 10200, 200);
    b.submit(Side::SELL, 10300, 30);  b.submit(Side::BUY, 10300, 30);
    ASSERT(b.point_of_control_ticks() == 10200, "poc_at_max_vol");
}

// ──────────────────────────────────────────────
// TOB stability, time-weighted spread, tape stats, iterator
// ──────────────────────────────────────────────

void test_tob_stability_streak() {
    Book b;
    b.submit(Side::BUY,  10000, 100);
    b.submit(Side::SELL, 10010, 100);
    (void)b.poll_tob_change();        // changed → streak=0
    (void)b.poll_tob_change();        // unchanged → streak=1
    (void)b.poll_tob_change();        // unchanged → streak=2
    ASSERT(b.current_tob_unchanged_streak() == 2,        "streak_2");
    ASSERT(b.max_tob_unchanged_streak_observed() == 2,   "max_streak_2");
    b.submit(Side::BUY, 10001, 50);
    (void)b.poll_tob_change();        // changed → reset
    ASSERT(b.current_tob_unchanged_streak() == 0,        "streak_reset");
    ASSERT(b.max_tob_unchanged_streak_observed() == 2,   "max_persists");
}

void test_time_weighted_spread_accumulates() {
    Book b;
    b.submit(Side::BUY,  10000, 100);
    b.submit(Side::SELL, 10005, 100);
    b.sample_time_weighted_spread();   // first sample — sets baseline
    // mała pętla zajmująca czas
    for (volatile int i = 0; i < 1000; ++i) {}
    b.sample_time_weighted_spread();
    ASSERT(b.mean_time_weighted_spread_ticks() >= 0.0,
                                                       "twas_nonneg");
}

void test_tape_statistics_basic() {
    Book b;
    b.submit(Side::SELL, 10100, 50);
    b.submit(Side::BUY,  10100, 50);   // trade 1: qty=50, px=10100
    b.submit(Side::SELL, 10110, 100);
    b.submit(Side::BUY,  10110, 100);  // trade 2: qty=100, px=10110
    auto ts = b.tape_statistics();
    ASSERT(ts.n_samples == 2,                       "tape_n_2");
    ASSERT(ts.min_qty == 50,                        "tape_minq_50");
    ASSERT(ts.max_qty == 100,                       "tape_maxq_100");
    ASSERT(ts.min_price_ticks == 10100,             "tape_minp");
    ASSERT(ts.max_price_ticks == 10110,             "tape_maxp");
    ASSERT(ts.mean_qty == 75.0,                     "tape_meanq_75");
    ASSERT(ts.price_stddev_ticks > 0.0,             "tape_stddev_pos");
}

void test_for_each_order_visits_all() {
    Book b;
    b.submit(Side::BUY,  10000, 10);
    b.submit(Side::BUY,  9999,  20);
    b.submit(Side::SELL, 10010, 5);
    b.submit(Side::SELL, 10020, 15);
    std::int32_t count = 0;
    std::int32_t qty_sum = 0;
    b.for_each_order([&](const auto& o) {
        ++count;
        qty_sum += o.total_qty;
    });
    ASSERT(count == 4,                              "iter_4_orders");
    ASSERT(qty_sum == 50,                           "iter_sum_qty_50");
}

// ──────────────────────────────────────────────
// Event seq + hidden ratio + resting count
// ──────────────────────────────────────────────

void test_event_seq_monotonic() {
    Book b;
    const auto s0 = b.last_event_seq_num();
    b.submit(Side::BUY, 10000, 100);
    const auto s1 = b.last_event_seq_num();
    b.submit(Side::SELL, 10010, 50);
    const auto s2 = b.last_event_seq_num();
    ASSERT(s1 > s0,                                  "seq_inc_1");
    ASSERT(s2 > s1,                                  "seq_inc_2");
}

void test_hidden_ratio_zero_for_limit() {
    Book b;
    b.submit(Side::BUY,  10000, 100);
    b.submit(Side::SELL, 10010, 100);
    ASSERT(b.hidden_liquidity_ratio() == 0.0,        "hidden_ratio_zero");
}

void test_hidden_ratio_with_iceberg() {
    Book b;
    b.submit(Side::SELL, 10100, 1000, OrderType::ICEBERG,
             TimeInForce::DAY, 0, 0, /*displayed=*/100);
    // visible total_qty at 10100 = 100 (displayed); hidden = 900
    const double r = b.hidden_liquidity_ratio();
    // ratio = 900 / 1000 = 0.9
    ASSERT(r > 0.85 && r < 0.95,                     "hidden_ratio_iceberg_0.9");
}

void test_resting_order_count_buy_side() {
    Book b;
    b.submit(Side::BUY, 10000, 10);
    b.submit(Side::BUY, 10000, 20);
    b.submit(Side::BUY, 9998,  30);
    ASSERT(b.resting_order_count(Side::BUY)  == 3,   "rest_count_buy_3");
    ASSERT(b.resting_order_count(Side::SELL) == 0,   "rest_count_sell_0");
}

// ──────────────────────────────────────────────
// Per-account aggressive/passive + CTR
// ──────────────────────────────────────────────

void test_aggressive_volume_taker() {
    Book b;
    constexpr std::uint64_t MAKER = 11;
    constexpr std::uint64_t TAKER = 22;
    b.submit(Side::SELL, 10100, 100, OrderType::LIMIT, TimeInForce::DAY,
             /*order_id=*/0, /*client_id=*/MAKER);
    b.submit(Side::BUY,  10100, 100, OrderType::LIMIT, TimeInForce::DAY,
             0, TAKER);
    const auto m_ex = b.get_account_exposure(MAKER);
    const auto t_ex = b.get_account_exposure(TAKER);
    ASSERT(t_ex.aggressive_volume == 100, "taker_agg_100");
    ASSERT(t_ex.passive_volume    == 0,   "taker_pass_0");
    ASSERT(m_ex.passive_volume    == 100, "maker_pass_100");
    ASSERT(m_ex.aggressive_volume == 0,   "maker_agg_0");
}

void test_aggressive_ratio_mixed() {
    Book b;
    constexpr std::uint64_t ACC = 33;
    // First: account is maker (passive 50)
    b.submit(Side::SELL, 10100, 50, OrderType::LIMIT, TimeInForce::DAY, 0, ACC);
    b.submit(Side::BUY,  10100, 50, OrderType::LIMIT, TimeInForce::DAY, 0, 99);
    // Second: account is taker (aggressive 50)
    b.submit(Side::SELL, 10100, 50, OrderType::LIMIT, TimeInForce::DAY, 0, 99);
    b.submit(Side::BUY,  10100, 50, OrderType::LIMIT, TimeInForce::DAY, 0, ACC);
    // 50 agg + 50 pass → ratio = 0.5
    const double r = b.aggressive_ratio_for(ACC);
    ASSERT(r > 0.45 && r < 0.55, "agg_ratio_half");
}

void test_cancel_to_fill_ratio_per_account() {
    Book b;
    constexpr std::uint64_t ACC = 44;
    // Submit + cancel × 3
    for (int i = 0; i < 3; ++i) {
        auto id = b.submit(Side::BUY, 10000 + i, 100, OrderType::LIMIT,
                            TimeInForce::DAY, 0, ACC);
        b.cancel(id);
    }
    // One fill: maker SELL, then taker BUY by ACC
    b.submit(Side::SELL, 10100, 50, OrderType::LIMIT, TimeInForce::DAY, 0, 99);
    b.submit(Side::BUY,  10100, 50, OrderType::LIMIT, TimeInForce::DAY, 0, ACC);
    const double r = b.cancel_to_fill_ratio_for(ACC);
    // 3 cancels / 1 fill = 3.0
    ASSERT(r >= 2.9 && r <= 3.1, "per_acct_ctr_3");
}

// ──────────────────────────────────────────────
// BookHealth dashboard
// ──────────────────────────────────────────────

void test_book_health_snapshot() {
    Book b;
    b.submit(Side::BUY,  10000, 100);
    b.submit(Side::SELL, 10010, 100);
    b.submit(Side::SELL, 10010, 100);   // taker BUY does nothing, but agg flow
    b.submit(Side::BUY,  10010, 100);   // taker BUY → fill
    auto h = b.health_snapshot();
    ASSERT(h.spread_ticks >= 0,                "health_spread_nn");
    ASSERT(h.total_fills >= 1,                  "health_fills_ge_1");
    ASSERT(h.last_event_seq_num > 0,            "health_seq_nonzero");
    ASSERT(h.total_orders_added >= 3,           "health_added_ge_3");
}

// ──────────────────────────────────────────────
// Trade arrival rate + realized vol + spread bias + queue replenish
// ──────────────────────────────────────────────

void test_trades_per_second_returns_nonneg() {
    Book b;
    b.submit(Side::SELL, 10100, 100);
    b.submit(Side::BUY,  10100, 50);
    // some work between trades — daje delta ts_ns
    for (volatile int i = 0; i < 1000; ++i) {}
    b.submit(Side::SELL, 10100, 100);
    b.submit(Side::BUY,  10100, 50);
    ASSERT(b.trades_per_second() >= 0.0,             "trades_per_sec_nn");
}

void test_realized_vol_zero_when_flat() {
    Book b;
    b.submit(Side::SELL, 10100, 100);
    b.submit(Side::BUY,  10100, 50);
    b.submit(Side::SELL, 10100, 100);
    b.submit(Side::BUY,  10100, 50);   // all at 10100 — log return = 0
    ASSERT(b.realized_volatility_log_returns() == 0.0, "rv_zero_flat");
}

void test_realized_vol_positive_when_varied() {
    Book b;
    b.submit(Side::SELL, 10100, 100);
    b.submit(Side::BUY,  10100, 100);
    b.submit(Side::SELL, 10200, 100);
    b.submit(Side::BUY,  10200, 100);
    ASSERT(b.realized_volatility_log_returns() > 0.0, "rv_pos_varied");
}

void test_spread_bias_bid_side() {
    Book c;
    c.submit(Side::BUY,  10000, 100);
    c.submit(Side::SELL, 10005, 100);   // mid = 10002, ask_off=3, bid_off=2
    c.poll_tob_micro();                  // bid_off < ask_off → bid_side++
    ASSERT(c.spread_bias_bid_side() == 1, "bias_bid_side_1");
}

void test_spread_bias_neutral() {
    Book b;
    b.submit(Side::BUY,  10000, 100);
    b.submit(Side::SELL, 10010, 100);   // mid=10005, oba offset=5 → neutral
    b.poll_tob_micro();
    ASSERT(b.spread_bias_neutral() == 1, "bias_neutral_1");
}

void test_queue_replenish_bid() {
    Book b;
    b.submit(Side::BUY,  10000, 100);
    b.submit(Side::SELL, 10010, 100);
    b.poll_tob_micro();                         // initial obs
    b.submit(Side::BUY,  10000, 50);            // bid qty grows: 100 → 150
    b.poll_tob_micro();                         // replenish!
    ASSERT(b.queue_replenish_bid_count() == 1, "queue_replenish_bid_1");
    ASSERT(b.queue_consume_bid_count()   == 0, "queue_consume_bid_0");
}

// ──────────────────────────────────────────────
// Kyle's lambda + latency arb + cluster aggregations
// ──────────────────────────────────────────────

void test_kyle_lambda_zero_without_trades() {
    Book b;
    ASSERT(b.kyle_lambda() == 0.0,                "lambda_zero_empty");
}

void test_kyle_lambda_nonzero_after_price_move() {
    Book b;
    b.submit(Side::SELL, 10100, 100);
    b.submit(Side::BUY,  10100, 100);   // trade 1: px=10100
    b.submit(Side::SELL, 10200, 100);
    b.submit(Side::BUY,  10200, 100);   // trade 2: px=10200, dp=+100, v=+100
    // λ = (100*100) / 100² = 1.0
    const double L = b.kyle_lambda();
    ASSERT(L > 0.5 && L < 1.5,                    "lambda_around_1");
}

void test_latency_arb_off_by_default() {
    Book b;
    b.submit(Side::SELL, 10100, 50);
    b.submit(Side::BUY,  10100, 50);
    b.submit(Side::SELL, 10101, 50);
    b.submit(Side::BUY,  10101, 50);
    ASSERT(b.latency_arb_same_side_fast_count() == 0, "larb_off_zero");
}

void test_latency_arb_detected_same_side_back_to_back() {
    Book b;
    b.set_latency_arb_window_ns(1'000'000'000ULL);  // 1 second — bardzo szeroki
    b.submit(Side::SELL, 10100, 50);
    b.submit(Side::BUY,  10100, 50);    // BUY fill #1
    b.submit(Side::SELL, 10101, 50);
    b.submit(Side::BUY,  10101, 50);    // BUY fill #2 — same side, within window
    ASSERT(b.latency_arb_same_side_fast_count() >= 1, "larb_same_side_caught");
}

void test_cluster_aggregations() {
    using Cluster = orderbook_pro::BookCluster<4, 16384, 2048>;
    Cluster cluster;
    cluster.register_symbol("AAPL");
    cluster.register_symbol("MSFT");
    auto* a = cluster.book("AAPL");
    auto* m = cluster.book("MSFT");
    if (!a || !m) { ASSERT(false, "cluster_books_present"); return; }
    a->submit(Side::SELL, 10100, 100);
    a->submit(Side::BUY,  10100, 100);   // fill
    m->submit(Side::SELL, 10200, 50);
    m->submit(Side::BUY,  10200, 50);    // fill
    ASSERT(cluster.cluster_total_fills() == 2,        "cluster_fills_2");
    ASSERT(cluster.cluster_total_volume() == 150,     "cluster_vol_150");
}

// ──────────────────────────────────────────────
// Per-side last fill ts + iceberg refreshes + wall
// ──────────────────────────────────────────────

void test_last_buy_fill_ts_set() {
    Book b;
    ASSERT(b.last_buy_fill_ts_ns()  == 0,            "buy_ts_initial_0");
    ASSERT(b.last_sell_fill_ts_ns() == 0,            "sell_ts_initial_0");
    b.submit(Side::SELL, 10100, 50);
    b.submit(Side::BUY,  10100, 50);    // taker BUY
    ASSERT(b.last_buy_fill_ts_ns()  > 0,             "buy_ts_after_fill");
    ASSERT(b.last_sell_fill_ts_ns() == 0,            "sell_ts_unchanged");
}

void test_iceberg_refresh_counter() {
    Book b;
    auto ice = b.submit(Side::SELL, 10100, 1000, OrderType::ICEBERG,
                         TimeInForce::DAY, 0, 0, /*displayed=*/100);
    (void)ice;
    ASSERT(b.iceberg_refresh_count() == 0,           "ice_refresh_0_initial");
    // Take 100 (displayed) — should refresh from hidden
    b.submit(Side::BUY, 10100, 100);
    ASSERT(b.iceberg_refresh_count() >= 1,           "ice_refresh_after_take");
}

void test_largest_resting_order_qty() {
    Book b;
    b.submit(Side::BUY, 10000, 50);
    b.submit(Side::BUY, 9999,  200);     // wall
    b.submit(Side::SELL, 10010, 80);
    ASSERT(b.largest_resting_order_qty() == 200,    "wall_200");
}

// ──────────────────────────────────────────────
// Per-reason / per-TIF / per-OrderType breakdowns
// ──────────────────────────────────────────────

void test_rejections_by_reason_counts() {
    Book b;
    RejectReason rr = RejectReason::NONE;
    b.submit(Side::BUY, 10000, 0, OrderType::LIMIT, TimeInForce::DAY, 0, 0, 0, &rr);
    b.submit(Side::BUY, 10000, 0, OrderType::LIMIT, TimeInForce::DAY, 0, 0, 0, &rr);
    ASSERT(b.rejections_by_reason(RejectReason::QTY_ZERO_OR_NEGATIVE) == 2,
                                                       "rej_qty_zero_2");
    ASSERT(b.most_common_reject_reason() == RejectReason::QTY_ZERO_OR_NEGATIVE,
                                                       "most_common_qty_zero");
}

void test_accepts_by_tif_counts() {
    Book b;
    b.submit(Side::BUY, 10000, 100, OrderType::LIMIT, TimeInForce::DAY);
    b.submit(Side::BUY, 9999,  100, OrderType::LIMIT, TimeInForce::GTC);
    b.submit(Side::BUY, 9998,  100, OrderType::LIMIT, TimeInForce::GTC);
    ASSERT(b.accepts_by_tif(TimeInForce::DAY) == 1, "tif_day_1");
    ASSERT(b.accepts_by_tif(TimeInForce::GTC) == 2, "tif_gtc_2");
    ASSERT(b.accepts_by_tif(TimeInForce::FOK) == 0, "tif_fok_0");
}

void test_accepts_by_type_counts() {
    Book b;
    b.submit(Side::BUY, 10000, 100, OrderType::LIMIT);
    b.submit(Side::BUY, 9999,  100, OrderType::LIMIT);
    b.submit(Side::SELL, 11000, 1000, OrderType::ICEBERG,
             TimeInForce::DAY, 0, 0, /*displayed=*/100);
    ASSERT(b.accepts_by_type(OrderType::LIMIT)   == 2, "type_limit_2");
    ASSERT(b.accepts_by_type(OrderType::ICEBERG) == 1, "type_iceberg_1");
}

void test_acceptance_ratio() {
    Book b;
    b.submit(Side::BUY, 10000, 100);  // accept
    b.submit(Side::BUY, 10000, 100);  // accept
    b.submit(Side::BUY, 10000, 0);    // reject (qty zero)
    // 2/3 ≈ 0.667
    const double r = b.acceptance_ratio();
    ASSERT(r > 0.6 && r < 0.7, "accept_ratio_0.67");
}

// ──────────────────────────────────────────────
// Depth concentration + pending pegs + book averages
// ──────────────────────────────────────────────

void test_depth_concentration_top1_full() {
    Book b;
    b.submit(Side::BUY, 10000, 100);    // only 1 level
    ASSERT(b.depth_concentration_bps(Side::BUY, 1) == 10000,
                                                    "depth_conc_top1_full");
}

void test_depth_concentration_top1_half() {
    Book b;
    b.submit(Side::BUY, 10000, 100);
    b.submit(Side::BUY, 9999,  100);
    // top-1 has 100/200 = 5000 bps
    ASSERT(b.depth_concentration_bps(Side::BUY, 1) == 5000,
                                                    "depth_conc_top1_5000");
}

void test_depth_concentration_top3_full() {
    Book b;
    b.submit(Side::BUY, 10000, 100);
    b.submit(Side::BUY, 9999,  100);
    b.submit(Side::BUY, 9998,  100);
    // top-3 = all 3 = 10000 bps
    ASSERT(b.depth_concentration_bps(Side::BUY, 3) == 10000,
                                                    "depth_conc_top3_full");
}

void test_peg_orders_count_zero_initially() {
    Book b;
    ASSERT(b.peg_orders_count() == 0,               "peg_count_0");
}

void test_avg_resting_qty_per_order() {
    Book b;
    b.submit(Side::BUY, 10000, 100);
    b.submit(Side::BUY, 9999,  300);
    // total qty 400, 2 orders → avg = 200
    ASSERT(b.avg_resting_qty_per_order() == 200.0,  "avg_resting_200");
}

void test_active_price_levels() {
    Book b;
    b.submit(Side::BUY, 10000, 100);
    b.submit(Side::BUY, 10000, 50);     // same level
    b.submit(Side::BUY, 9999,  100);    // different level
    b.submit(Side::SELL, 10010, 100);
    ASSERT(b.active_price_levels() == 3, "active_levels_3");
}

// ──────────────────────────────────────────────
// Mid ring + momentum + fill bands
// ──────────────────────────────────────────────

void test_mid_ring_starts_empty() {
    Book b;
    ASSERT(b.mid_ring_samples() == 0,                "mid_ring_empty");
    ASSERT(b.mid_momentum_ticks() == 0,              "mid_mom_zero_empty");
}

void test_mid_ring_samples_after_quotes() {
    Book b;
    b.submit(Side::BUY,  10000, 100);
    b.submit(Side::SELL, 10010, 100);
    b.sample_mid_to_ring();              // mid = 10005
    b.sample_mid_to_ring();              // same mid
    ASSERT(b.mid_ring_samples() == 2,                "mid_ring_2");
}

void test_mid_momentum_positive() {
    Book b;
    b.submit(Side::BUY,  10000, 100);
    b.submit(Side::SELL, 10010, 100);
    b.sample_mid_to_ring();              // mid=10005 (oldest)
    // raise the mid
    b.submit(Side::BUY,  10005, 100);    // best_bid → 10005
    b.sample_mid_to_ring();              // mid=(10005+10010)/2=10007
    ASSERT(b.mid_momentum_ticks() >= 1,              "mid_mom_positive");
}

void test_fill_band_compliance() {
    Book b;
    b.set_fill_band_threshold_ticks(3);
    b.submit(Side::BUY,  10000, 100);
    b.submit(Side::SELL, 10001, 100);    // mid=10000.5 → mid_ticks=10000
    // Fill within band: BUY at 10001 (|10001-10000|=1 ≤ 3)
    b.submit(Side::BUY,  10001, 100);
    ASSERT(b.fills_within_band() >= 1,               "band_within_1");
    ASSERT(b.fill_band_compliance_ratio() > 0.0,     "band_compliance_pos");
}

// ──────────────────────────────────────────────
// Per-side Kyle's lambda + spread regime + TOB skew + mid-VWAP
// ──────────────────────────────────────────────

void test_kyle_lambda_per_side_split() {
    Book b;
    // Buy taker drives price up: trade1 @10100, trade2 @10200 (BUY both)
    b.submit(Side::SELL, 10100, 100);
    b.submit(Side::BUY,  10100, 100);
    b.submit(Side::SELL, 10200, 100);
    b.submit(Side::BUY,  10200, 100);
    // buy-side λ accumulated; sell-side λ remains 0
    ASSERT(b.kyle_lambda_buy_abs() > 0.0,         "lambda_buy_pos");
    ASSERT(b.kyle_lambda_sell()    == 0.0,         "lambda_sell_zero");
}

void test_spread_regime_classifier() {
    Book b;
    b.set_spread_regime_thresholds(2, 6);
    // spread=1 → tight
    b.submit(Side::BUY,  10000, 100);
    b.submit(Side::SELL, 10001, 100);
    b.sample_spread_regime();
    // spread=10 → wide. Resetuj asks
    Book c;
    c.set_spread_regime_thresholds(2, 6);
    c.submit(Side::BUY,  10000, 100);
    c.submit(Side::SELL, 10010, 100);
    c.sample_spread_regime();
    ASSERT(b.spread_regime_tight_count() == 1,      "regime_tight_1");
    ASSERT(c.spread_regime_wide_count() == 1,       "regime_wide_1");
}

void test_tob_skewness_bid_heavy() {
    Book b;
    b.submit(Side::BUY,  10000, 200);
    b.submit(Side::SELL, 10010, 100);
    // skew = (200-100)/300 × 10000 ≈ 3333
    const auto skew = b.tob_skewness_bps();
    ASSERT(skew > 3000 && skew < 3500,              "skew_bid_3333");
}

void test_tob_skewness_balanced() {
    Book b;
    b.submit(Side::BUY,  10000, 100);
    b.submit(Side::SELL, 10010, 100);
    ASSERT(b.tob_skewness_bps() == 0,               "skew_balanced_0");
}

void test_mid_minus_vwap_after_trade() {
    Book b;
    b.submit(Side::SELL, 10100, 50);
    b.submit(Side::BUY,  10100, 50);            // VWAP = 10100
    b.submit(Side::BUY,  10050, 50);            // adds bid → mid = (10050+? )/2 — no ask
    b.submit(Side::SELL, 10110, 50);            // best_ask 10110, mid=(10050+10110)/2=10080
    const auto d = b.mid_minus_tape_vwap_ticks();
    // d should be small (within 50 ticks)
    ASSERT(d > -200 && d < 200,                     "mid_vs_vwap_bounded");
}

// ──────────────────────────────────────────────
// Mid trend + one-sided book + spread histogram + top-K
// ──────────────────────────────────────────────

void test_mid_trend_unknown_initially() {
    Book b;
    ASSERT(b.classify_mid_trend() == Book::MidTrend::UNKNOWN, "trend_unknown");
}

void test_mid_trend_up_after_rise() {
    Book b;
    b.submit(Side::BUY,  10000, 100);
    b.submit(Side::SELL, 10010, 100);
    b.sample_mid_to_ring();
    b.submit(Side::BUY,  10006, 100);    // bid up
    b.sample_mid_to_ring();
    ASSERT(b.classify_mid_trend() == Book::MidTrend::UP, "trend_up");
}

void test_one_sided_bid_only_counted() {
    Book b;
    b.submit(Side::BUY, 10000, 100);     // no ask
    b.poll_tob_micro();
    ASSERT(b.one_sided_bid_only_count() == 1, "one_sided_bid_only_1");
    ASSERT(b.one_sided_ask_only_count() == 0, "one_sided_ask_only_0");
}

void test_spread_histogram_buckets() {
    Book b;
    b.submit(Side::BUY,  10000, 100);
    b.submit(Side::SELL, 10003, 100);    // spread=3
    b.sample_spread_to_histogram();
    b.submit(Side::SELL, 10001, 100);    // best_ask=10001, spread=1
    b.sample_spread_to_histogram();
    ASSERT(b.spread_histogram_bin(3) >= 1, "hist_bin_3_ge_1");
    ASSERT(b.spread_histogram_bin(1) >= 1, "hist_bin_1_ge_1");
    ASSERT(b.spread_histogram_total() == 2, "hist_total_2");
}

void test_spread_histogram_median() {
    Book b;
    b.submit(Side::BUY, 10000, 100);
    b.submit(Side::SELL, 10005, 100);    // spread=5
    for (int i = 0; i < 3; ++i) b.sample_spread_to_histogram();
    // Median of three 5s = 5
    ASSERT(b.spread_histogram_median_ticks() == 5, "hist_median_5");
}

void test_top_k_largest_orders() {
    Book b;
    b.submit(Side::BUY, 10000, 100);
    b.submit(Side::BUY, 9999,  300);
    b.submit(Side::BUY, 9998,  50);
    b.submit(Side::SELL, 10010, 200);
    std::int32_t top3[3]{};
    const auto n = b.top_k_resting_qty(top3, 3);
    ASSERT(n == 3,                            "top_k_n_3");
    ASSERT(top3[0] == 300,                    "top_k_max_300");
    ASSERT(top3[1] == 200,                    "top_k_second_200");
    ASSERT(top3[2] == 100,                    "top_k_third_100");
}

// ──────────────────────────────────────────────
// Price change distribution + toxicity composite + cluster vw spread
// ──────────────────────────────────────────────

void test_price_change_hist_zero_bin() {
    Book b;
    b.submit(Side::SELL, 10100, 50);
    b.submit(Side::BUY,  10100, 50);   // trade1 @10100
    b.submit(Side::SELL, 10100, 50);
    b.submit(Side::BUY,  10100, 50);   // trade2 @10100; Δp=0 → bin 4
    ASSERT(b.price_change_hist_bin(4) >= 1, "pc_hist_zero_bin");
    ASSERT(b.price_change_hist_zero_fraction() > 0.0, "pc_zero_frac_pos");
}

void test_price_change_hist_up_bin() {
    Book b;
    b.submit(Side::SELL, 10100, 50);
    b.submit(Side::BUY,  10100, 50);
    b.submit(Side::SELL, 10102, 50);
    b.submit(Side::BUY,  10102, 50);   // Δp=+2 → bin 6
    ASSERT(b.price_change_hist_bin(6) >= 1, "pc_hist_plus2_bin");
}

void test_price_change_hist_tail() {
    Book b;
    b.submit(Side::SELL, 10100, 50);
    b.submit(Side::BUY,  10100, 50);
    b.submit(Side::SELL, 10200, 50);
    b.submit(Side::BUY,  10200, 50);   // Δp=+100 → clipped to +4 → bin 8
    ASSERT(b.price_change_hist_bin(8) >= 1, "pc_hist_tail_bin8");
    ASSERT(b.price_change_hist_tail_fraction() > 0.0, "pc_tail_frac_pos");
}

void test_toxicity_composite_zero_quiet() {
    Book b;
    ASSERT(b.toxicity_composite_score_bps() == 0, "tox_zero_empty");
}

void test_toxicity_composite_after_activity() {
    Book b;
    b.submit(Side::SELL, 10100, 100);
    b.submit(Side::BUY,  10100, 100);
    b.submit(Side::SELL, 10200, 100);
    b.submit(Side::BUY,  10200, 100);  // one-sided BUY flow → VPIN=10000
    const auto score = b.toxicity_composite_score_bps();
    ASSERT(score > 0,                                "tox_pos_after_activity");
}

void test_cluster_volume_weighted_spread() {
    using Cluster = orderbook_pro::BookCluster<4, 16384, 2048>;
    Cluster cluster;
    cluster.register_symbol("AAPL");
    cluster.register_symbol("MSFT");
    auto* a = cluster.book("AAPL");
    auto* m = cluster.book("MSFT");
    if (!a || !m) { ASSERT(false, "vw_spread_books_present"); return; }
    // AAPL: spread 2 ticks, volume 100
    a->submit(Side::BUY,  10000, 100);
    a->submit(Side::SELL, 10002, 100);
    a->submit(Side::SELL, 10000, 100);   // crosses → trade. vol=100
    // MSFT: spread 10 ticks, volume 50
    m->submit(Side::BUY,  10000, 50);
    m->submit(Side::SELL, 10010, 50);
    m->submit(Side::SELL, 10000, 50);    // crosses → vol=50
    // Spread po fillach: AAPL=2, MSFT=10. Volumes: 100, 50.
    // Weighted avg = (2*100 + 10*50) / 150 = (200+500)/150 = 700/150 ≈ 4.67
    const double w = cluster.volume_weighted_avg_spread_ticks();
    ASSERT(w >= 0.0 && w <= 100.0,                   "cluster_vw_spread_bounded");
}

// ──────────────────────────────────────────────
// Implementation shortfall (TCA)
// ──────────────────────────────────────────────

void test_is_zero_without_fills() {
    Book b;
    ASSERT(b.cumulative_implementation_shortfall_ticks_qty() == 0, "is_zero_init");
    ASSERT(b.mean_implementation_shortfall_ticks_per_share() == 0.0,
                                                                   "is_mean_zero");
}

void test_is_buy_pays_adverse() {
    Book b;
    // Build market: bid 10000, ask 10010, mid=10005
    b.submit(Side::BUY,  10000, 100);
    b.submit(Side::SELL, 10010, 100);
    // BUY taker @10010: decision_mid was 10005 at submit, fill @10010 → IS = +5×100 = +500
    b.submit(Side::BUY,  10010, 100);
    const auto is_total = b.cumulative_implementation_shortfall_ticks_qty();
    ASSERT(is_total >= 400 && is_total <= 600,                     "is_buy_adverse_500");
    const auto mean_per_share = b.mean_implementation_shortfall_ticks_per_share();
    ASSERT(mean_per_share >= 4.0 && mean_per_share <= 6.0,         "is_per_share_5");
}

void test_is_sell_negative_when_paid_below_mid() {
    Book b;
    b.submit(Side::BUY,  10000, 100);
    b.submit(Side::SELL, 10010, 100);
    // SELL taker hits bid @10000, mid=10005 at submit → IS = +5×100 = +500 (positive cost)
    b.submit(Side::SELL, 10000, 100);
    const auto is_total = b.cumulative_implementation_shortfall_ticks_qty();
    ASSERT(is_total >= 400 && is_total <= 600,                     "is_sell_pays_500");
}

// ──────────────────────────────────────────────
// Per-side VWAP + inter-trade gap + largest single + last-N VWAP
// ──────────────────────────────────────────────

void test_per_side_vwap_split() {
    Book b;
    b.submit(Side::SELL, 10100, 100);
    b.submit(Side::BUY,  10100, 100);     // taker BUY @10100
    b.submit(Side::BUY,  9900,  100);
    b.submit(Side::SELL, 9900,  100);     // taker SELL @9900
    ASSERT(b.buy_vwap_ticks()  == 10100, "buy_vwap_10100");
    ASSERT(b.sell_vwap_ticks() == 9900,  "sell_vwap_9900");
    ASSERT(b.buy_vs_sell_vwap_spread_ticks() == 200, "vwap_spread_200");
}

void test_per_side_vwap_zero_no_trades() {
    Book b;
    ASSERT(b.buy_vwap_ticks()  == 0,     "buy_vwap_zero_init");
    ASSERT(b.sell_vwap_ticks() == 0,     "sell_vwap_zero_init");
}

void test_inter_trade_gap_after_two_trades() {
    Book b;
    b.submit(Side::SELL, 10100, 50);
    b.submit(Side::BUY,  10100, 50);
    for (volatile int i = 0; i < 1000; ++i) {}
    b.submit(Side::SELL, 10100, 50);
    b.submit(Side::BUY,  10100, 50);
    ASSERT(b.inter_trade_gap_sample_count() >= 1, "gap_samples_ge_1");
    // mean ≤ max (sanity — both unsigned, niezależne od konkretnych wartości)
    ASSERT(b.inter_trade_gap_mean_ns() <= b.inter_trade_gap_max_ns(),
                                                   "gap_mean_le_max");
}

void test_largest_single_trade_qty() {
    Book b;
    b.submit(Side::SELL, 10100, 50);
    b.submit(Side::BUY,  10100, 50);
    b.submit(Side::SELL, 10101, 200);
    b.submit(Side::BUY,  10101, 200);    // biggest single
    b.submit(Side::SELL, 10102, 30);
    b.submit(Side::BUY,  10102, 30);
    ASSERT(b.largest_single_trade_qty() == 200, "largest_trade_200");
}

void test_last_n_vwap_rolling() {
    Book b;
    b.submit(Side::SELL, 10100, 100);
    b.submit(Side::BUY,  10100, 100);   // trade @10100
    b.submit(Side::SELL, 10200, 100);
    b.submit(Side::BUY,  10200, 100);   // trade @10200
    // Last-2 VWAP = (10100×100 + 10200×100) / 200 = 10150
    ASSERT(b.last_n_vwap_ticks(2) == 10150, "lastN_vwap_10150");
    ASSERT(b.last_n_vwap_ticks(1) == 10200, "lastN_vwap_1_10200");
}

// ──────────────────────────────────────────────
// First-fill latency + burst detector
// ──────────────────────────────────────────────

void test_first_fill_latency_zero_init() {
    Book b;
    ASSERT(b.first_fill_latency_count()  == 0,    "ffl_count_0");
    ASSERT(b.first_fill_latency_mean_ns() == 0,    "ffl_mean_0");
    ASSERT(b.first_fill_latency_min_ns() == 0,     "ffl_min_0");
}

void test_first_fill_latency_recorded() {
    Book b;
    b.submit(Side::SELL, 10100, 100);
    b.submit(Side::BUY,  10100, 100);    // both filled at once
    ASSERT(b.first_fill_latency_count() >= 1,     "ffl_count_ge_1");
    // mean >= 0 (instant fills can be 0 ns)
    ASSERT(b.first_fill_latency_max_ns() >= b.first_fill_latency_mean_ns(),
                                                   "ffl_max_ge_mean");
}

void test_burst_detector_off_by_default() {
    Book b;
    b.submit(Side::BUY, 10000, 100);
    b.submit(Side::BUY, 9999,  100);
    ASSERT(b.burst_runs_count() == 0,             "burst_off_0");
}

void test_burst_detector_catches_rapid_submits() {
    Book b;
    b.set_burst_window_ns(1'000'000'000ULL);   // 1 second window
    b.submit(Side::BUY, 10000, 100);
    b.submit(Side::BUY, 9999,  100);
    b.submit(Side::BUY, 9998,  100);
    ASSERT(b.burst_runs_count() >= 1,             "burst_run_caught");
    ASSERT(b.burst_current_run_count() >= 2,      "burst_run_size_ge_2");
}

// ──────────────────────────────────────────────
// Completion histogram + TWAP-of-mid + mean trade qty
// ──────────────────────────────────────────────

void test_completion_filled_fully() {
    Book b;
    b.submit(Side::SELL, 10100, 100);
    b.submit(Side::BUY,  10100, 100);   // SELL maker fully filled
    ASSERT(b.completion_filled_fully() >= 1,            "comp_filled_ge_1");
}

void test_completion_cancelled_unfilled() {
    Book b;
    auto id = b.submit(Side::BUY, 10000, 100);
    b.cancel(id);
    ASSERT(b.completion_cancelled_unfilled() == 1,      "comp_cxl_unfilled_1");
    ASSERT(b.completion_cancelled_partial() == 0,       "comp_cxl_partial_0");
}

void test_completion_cancelled_partial() {
    Book b;
    auto id = b.submit(Side::BUY, 10000, 100);  // maker
    b.submit(Side::SELL, 10000, 50);             // partial fill maker
    b.cancel(id);
    ASSERT(b.completion_cancelled_partial() == 1,       "comp_cxl_partial_1");
}

void test_fill_rate_ratio() {
    Book b;
    b.submit(Side::SELL, 10100, 50);
    b.submit(Side::BUY,  10100, 50);    // 1 fully filled
    auto id = b.submit(Side::BUY, 10000, 100);
    b.cancel(id);                        // 1 cancelled
    // 3 added, 1 filled fully → ratio ~ 0.33
    const double r = b.fill_rate_ratio();
    ASSERT(r > 0.0 && r < 1.0,                          "fill_rate_in_unit");
}

void test_twmid_sample_accumulates() {
    Book b;
    b.submit(Side::BUY,  10000, 100);
    b.submit(Side::SELL, 10010, 100);
    b.sample_time_weighted_mid();
    for (volatile int i = 0; i < 1000; ++i) {}
    b.sample_time_weighted_mid();
    ASSERT(b.time_weighted_mid_total_dt_ns() > 0,       "twmid_dt_pos");
    ASSERT(b.mean_time_weighted_mid_ticks() > 0.0,      "twmid_mean_pos");
}

void test_mean_trade_qty() {
    Book b;
    b.submit(Side::SELL, 10100, 100);
    b.submit(Side::BUY,  10100, 100);    // qty 100
    b.submit(Side::SELL, 10101, 200);
    b.submit(Side::BUY,  10101, 200);    // qty 200
    // 2 fills, vol 300 → mean = 150
    ASSERT(b.mean_trade_qty() == 150.0,                 "mean_trade_qty_150");
}

// ──────────────────────────────────────────────
// Per-side maker fills + mean notional + cluster imbalance + top-K levels
// ──────────────────────────────────────────────

void test_maker_fills_sell_side_counted() {
    Book b;
    b.submit(Side::SELL, 10100, 100);   // maker SELL
    b.submit(Side::BUY,  10100, 100);    // taker BUY → SELL maker fully filled
    ASSERT(b.maker_fills_sell_side() >= 1, "maker_fills_sell_1");
    ASSERT(b.maker_fills_buy_side() == 0,   "maker_fills_buy_0");
}

void test_maker_fills_buy_side_counted() {
    Book b;
    b.submit(Side::BUY,  10000, 100);   // maker BUY
    b.submit(Side::SELL, 10000, 100);    // taker SELL → BUY maker fully filled
    ASSERT(b.maker_fills_buy_side() >= 1,  "maker_fills_buy_1");
}

void test_mean_fill_notional() {
    Book b;
    b.submit(Side::SELL, 10100, 100);
    b.submit(Side::BUY,  10100, 100);    // notional = 10100×100 = 1010000
    b.submit(Side::SELL, 10200, 50);
    b.submit(Side::BUY,  10200, 50);     // notional = 10200×50 = 510000
    // total 2 fills, total notional 1520000 → mean = 760000
    const double m = b.mean_fill_notional_ticks();
    ASSERT(m > 750000.0 && m < 770000.0,    "mean_fill_notional_760k");
}

void test_cluster_flow_imbalance() {
    using Cluster = orderbook_pro::BookCluster<4, 16384, 2048>;
    Cluster cluster;
    cluster.register_symbol("AAPL");
    cluster.register_symbol("MSFT");
    auto* a = cluster.book("AAPL");
    auto* m = cluster.book("MSFT");
    if (!a || !m) { ASSERT(false, "cluster_imb_books"); return; }
    a->submit(Side::SELL, 10100, 100);
    a->submit(Side::BUY,  10100, 100);   // taker BUY 100
    m->submit(Side::SELL, 10200, 50);
    m->submit(Side::BUY,  10200, 50);    // taker BUY 50
    // Across cluster: buy_vol=150, sell_vol=0 → imbalance = 10000
    ASSERT(cluster.cluster_flow_imbalance_bps() == 10000, "cluster_imb_full_buy");
}

void test_top_k_active_levels_by_qty() {
    Book b;
    b.submit(Side::BUY, 10000, 100);
    b.submit(Side::BUY, 9999,  300);   // top
    b.submit(Side::BUY, 9998,  50);
    b.submit(Side::SELL, 10010, 200);  // second
    std::int32_t prices[3]{}, qtys[3]{};
    const auto n = b.top_k_active_levels_by_qty(prices, qtys, 3);
    ASSERT(n == 3,                          "topk_lvl_n_3");
    ASSERT(qtys[0] == 300 && prices[0] == 9999,  "topk_lvl_max_300_at_9999");
    ASSERT(qtys[1] == 200 && prices[1] == 10010, "topk_lvl_second_200");
    ASSERT(qtys[2] == 100 && prices[2] == 10000, "topk_lvl_third_100");
}

// ──────────────────────────────────────────────
// Depth pyramid + cumulative resting vol + best qty histogram
// ──────────────────────────────────────────────

void test_depth_pyramid_steepness_steep() {
    Book b;
    b.submit(Side::BUY, 10000, 1000);   // best
    b.submit(Side::BUY, 9999,  100);
    b.submit(Side::BUY, 9998,  10);
    // 1→3 spadek: (1000 - 10)/1000 × 10000 = 9900
    const auto s = b.depth_pyramid_steepness_bps(Side::BUY, 3);
    ASSERT(s > 9000,                          "pyramid_steep");
}

void test_depth_pyramid_flat() {
    Book b;
    b.submit(Side::BUY, 10000, 100);
    b.submit(Side::BUY, 9999,  100);
    b.submit(Side::BUY, 9998,  100);
    // 1→3: (100-100)/100 = 0
    const auto s = b.depth_pyramid_steepness_bps(Side::BUY, 3);
    ASSERT(s == 0,                            "pyramid_flat_0");
}

void test_cumulative_resting_volume_buy() {
    Book b;
    b.submit(Side::BUY, 10000, 100);
    b.submit(Side::BUY, 9999,  200);
    b.submit(Side::BUY, 9998,  50);
    ASSERT(b.cumulative_resting_volume(Side::BUY)  == 350, "cum_rest_buy_350");
    ASSERT(b.cumulative_resting_volume(Side::SELL) == 0,    "cum_rest_sell_0");
}

void test_best_qty_histogram_samples() {
    Book b;
    b.submit(Side::BUY,  10000, 100);
    b.submit(Side::SELL, 10010, 50);
    b.sample_best_qty_histogram();
    b.sample_best_qty_histogram();
    // qty 100 → bin ≈ log2(100) = 6 (100 → 50 → 25 → 12 → 6 → 3 → 1 = 6 shifts)
    std::uint64_t total_bid = 0;
    for (std::size_t i = 0; i < 16; ++i) total_bid += b.best_bid_qty_hist_bin(i);
    ASSERT(total_bid == 2,                                  "bid_hist_total_2");
}

// ──────────────────────────────────────────────
// EMA imbalance + microprice ring + signed-vol EMA
// ──────────────────────────────────────────────

void test_ema_imbalance_init() {
    Book b;
    b.submit(Side::BUY,  10000, 100);
    b.submit(Side::SELL, 10010, 50);
    b.sample_ema_imbalance();
    // First sample: bid=100, ask=50, imb = (100-50)/150 × 10000 = 3333
    ASSERT(b.ema_imbalance_bps() > 3000.0 && b.ema_imbalance_bps() < 3500.0,
                                                    "ema_imb_init_3333");
}

void test_ema_imbalance_smooths() {
    Book b;
    b.submit(Side::BUY,  10000, 100);
    b.submit(Side::SELL, 10010, 50);
    b.sample_ema_imbalance();
    const double first = b.ema_imbalance_bps();
    // Reduce bid → imbalance moves toward 0
    auto id = b.submit(Side::BUY, 10000, 200);    // bid grows MORE one-sided
    (void)id;
    b.sample_ema_imbalance();
    // EMA powinien się przesunąć w kierunku nowego sygnału, ale wolniej
    const double second = b.ema_imbalance_bps();
    ASSERT(second != first,                         "ema_smoothed_moves");
}

void test_microprice_ring_after_two_samples() {
    Book b;
    b.submit(Side::BUY,  10000, 100);
    b.submit(Side::SELL, 10010, 100);
    b.sample_microprice_to_ring();
    b.sample_microprice_to_ring();
    ASSERT(b.microprice_ring_samples() == 2,        "mp_ring_2");
}

void test_signed_volume_ema_after_buy_trade() {
    Book b;
    b.submit(Side::SELL, 10100, 100);
    b.submit(Side::BUY,  10100, 100);   // taker BUY 100 → ema = +100
    ASSERT(b.ema_signed_volume_ready(),             "sv_ema_ready");
    ASSERT(b.ema_signed_volume() > 0.0,             "sv_ema_positive_buy");
}

// ──────────────────────────────────────────────
// Cont-Kukanov OFI + trade clustering Fano + maker survival
// ──────────────────────────────────────────────

void test_ofi_first_sample_baseline() {
    Book b;
    b.submit(Side::BUY,  10000, 100);
    b.submit(Side::SELL, 10010, 100);
    b.sample_ofi_ck();                       // establishes baseline
    ASSERT(b.ofi_cumulative() == 0,       "ofi_first_baseline_0");
    ASSERT(b.ofi_samples() == 1,           "ofi_samples_1");
}

void test_ofi_bid_increases_positive_flow() {
    Book b;
    b.submit(Side::BUY,  10000, 100);
    b.submit(Side::SELL, 10010, 100);
    b.sample_ofi_ck();
    b.submit(Side::BUY,  10000, 50);     // bid qty grows (100 → 150)
    b.sample_ofi_ck();
    // dw_b = 150 - 100 = +50 (unchanged price), dw_a = 100 - 100 = 0 → OFI = +50
    ASSERT(b.ofi_cumulative() == 50,      "ofi_bid_grow_50");
}

void test_ofi_bid_price_up_strong_positive() {
    Book b;
    b.submit(Side::BUY,  10000, 100);
    b.submit(Side::SELL, 10010, 100);
    b.sample_ofi_ck();
    b.submit(Side::BUY,  10001, 80);     // new best bid price 10001 (qty 80)
    b.sample_ofi_ck();
    // dw_b = +80 (price up), dw_a = 0 → OFI = +80
    ASSERT(b.ofi_cumulative() == 80,      "ofi_bid_price_up");
}

void test_trade_clustering_fano_one_sample() {
    Book b;
    b.submit(Side::SELL, 10100, 50);
    b.submit(Side::BUY,  10100, 50);
    // 1 trade — gap_var_count_ < 2 → returns 0
    ASSERT(b.trade_clustering_fano() == 0.0, "fano_zero_single_trade");
}

void test_trade_clustering_fano_after_trades() {
    Book b;
    for (int i = 0; i < 5; ++i) {
        b.submit(Side::SELL, 10100, 50);
        b.submit(Side::BUY,  10100, 50);
        for (volatile int j = 0; j < 200; ++j) {}
    }
    // 4 gaps recorded → fano computed (>= 0)
    ASSERT(b.trade_clustering_fano() >= 0.0,   "fano_nonneg");
}

void test_maker_survival_after_two_polls() {
    Book b;
    b.submit(Side::BUY, 10000, 100);
    b.submit(Side::BUY, 9999,  100);
    b.sample_maker_survival();        // poll 1: snapshot 2 ids
    // None cancelled — wszystkie powinny przeżyć
    b.sample_maker_survival();        // poll 2: 2/2 survivors
    ASSERT(b.maker_survival_total_polls() == 2,  "survival_polls_2");
    ASSERT(b.maker_survival_ratio() == 1.0,       "survival_ratio_1.0");
}

// ──────────────────────────────────────────────
// Markov chain + queue depth at arrival + trade momentum
// ──────────────────────────────────────────────

void test_markov_trending_all_buys() {
    Book b;
    for (int i = 0; i < 4; ++i) {
        b.submit(Side::SELL, 10100 + i, 50);
        b.submit(Side::BUY,  10100 + i, 50);
    }
    // 4 BUY trades → 3 BB transitions, 0 BS
    ASSERT(b.markov_count_BB() == 3,                "markov_BB_3");
    ASSERT(b.markov_count_BS() == 0,                "markov_BS_0");
    ASSERT(b.markov_prob_buy_given_buy() == 1.0,    "P(B|B)=1.0");
}

void test_markov_alternating() {
    Book b;
    b.submit(Side::SELL, 10100, 50);
    b.submit(Side::BUY,  10100, 50);    // BUY
    b.submit(Side::BUY,  9900,  50);
    b.submit(Side::SELL, 9900,  50);    // SELL → BS transition
    b.submit(Side::SELL, 10101, 50);
    b.submit(Side::BUY,  10101, 50);    // BUY → SB transition
    ASSERT(b.markov_count_BB() == 0,                "alt_BB_0");
    ASSERT(b.markov_count_BS() == 1,                "alt_BS_1");
    ASSERT(b.markov_count_SB() == 1,                "alt_SB_1");
}

void test_queue_depth_at_arrival_empty_level() {
    Book b;
    b.submit(Side::BUY, 10000, 100);   // pierwszy na level → qd=0
    ASSERT(b.queue_depth_arrival_samples() == 1,    "qda_samples_1");
    ASSERT(b.mean_queue_depth_at_arrival() == 0.0,  "qda_mean_0");
    ASSERT(b.max_queue_depth_at_arrival() == 0,     "qda_max_0");
}

void test_queue_depth_at_arrival_crowded() {
    Book b;
    b.submit(Side::BUY, 10000, 100);   // qd=0
    b.submit(Side::BUY, 10000, 50);    // qd=1
    b.submit(Side::BUY, 10000, 30);    // qd=2
    ASSERT(b.max_queue_depth_at_arrival() == 2,     "qda_max_2");
    // mean = (0+1+2)/3 = 1.0
    ASSERT(b.mean_queue_depth_at_arrival() == 1.0,  "qda_mean_1.0");
}

void test_trade_momentum_last_n_all_buys() {
    Book b;
    for (int i = 0; i < 3; ++i) {
        b.submit(Side::SELL, 10100 + i, 50);
        b.submit(Side::BUY,  10100 + i, 50);
    }
    ASSERT(b.trade_momentum_last_n(3) == 3,         "momentum_+3_all_buys");
}

void test_trade_momentum_balanced() {
    Book b;
    b.submit(Side::SELL, 10100, 50);
    b.submit(Side::BUY,  10100, 50);   // BUY
    b.submit(Side::BUY,  9900,  50);
    b.submit(Side::SELL, 9900,  50);   // SELL
    ASSERT(b.trade_momentum_last_n(2) == 0,         "momentum_0_balanced");
}

// ──────────────────────────────────────────────
// ACF lag-1 + slippage guard + NBBO audit + per-side cancel
// ──────────────────────────────────────────────

void test_acf_lag1_zero_without_data() {
    Book b;
    ASSERT(b.inter_trade_gap_autocorr_lag1() == 0.0, "acf_zero_empty");
}

void test_acf_lag1_after_trades() {
    Book b;
    for (int i = 0; i < 5; ++i) {
        b.submit(Side::SELL, 10100 + i, 50);
        b.submit(Side::BUY,  10100 + i, 50);
        for (volatile int j = 0; j < 200; ++j) {}
    }
    // ACF in [-1, 1] approximately
    const double acf = b.inter_trade_gap_autocorr_lag1();
    ASSERT(acf >= -1.5 && acf <= 1.5, "acf_bounded");
}

void test_slippage_guard_violations_zero_off() {
    Book b;
    b.submit(Side::SELL, 10100, 100);
    b.submit(Side::BUY,  10000, 100);    // mid=10050
    b.submit(Side::BUY,  10100, 100);     // taker BUY fills @10100, diff=50
    ASSERT(b.slippage_guard_violations() == 0, "slip_off_0");
}

void test_slippage_guard_violations_caught() {
    Book b;
    b.set_slippage_guard_threshold_ticks(10);
    b.submit(Side::SELL, 10100, 100);
    b.submit(Side::BUY,  10000, 100);     // mid=10050
    b.submit(Side::BUY,  10100, 100);      // diff=|10100-10050|=50 > 10
    ASSERT(b.slippage_guard_violations() >= 1, "slip_caught_1");
}

void test_nbbo_violations_zero_normal() {
    Book b;
    b.submit(Side::SELL, 10100, 100);
    b.submit(Side::BUY,  10000, 100);
    b.submit(Side::BUY,  10100, 100);     // normal fill
    ASSERT(b.nbbo_violations_count() == 0, "nbbo_normal_0");
}

void test_cancellations_per_side() {
    Book b;
    auto a = b.submit(Side::BUY, 10000, 100);
    auto c = b.submit(Side::SELL, 11000, 50);
    b.cancel(a);
    b.cancel(c);
    ASSERT(b.cancellations_by_buy()  == 1, "cxl_buy_1");
    ASSERT(b.cancellations_by_sell() == 1, "cxl_sell_1");
}

// ──────────────────────────────────────────────
// Lee-Ready + per-account VWAP + book integrity + partial-rest depth
// ──────────────────────────────────────────────

void test_lee_ready_buy_classified() {
    Book b;
    b.submit(Side::BUY,  10000, 100);
    b.submit(Side::SELL, 10100, 100);
    b.submit(Side::BUY,  10100, 100);   // taker BUY @ ask; mid=10050 → BUY
    ASSERT(b.lee_ready_classified_total() == 1, "lr_total_1");
    ASSERT(b.lee_ready_classified_buy()   == 1, "lr_buy_1");
    ASSERT(b.lee_ready_accuracy() == 1.0,        "lr_acc_1.0");
}

void test_lee_ready_sell_classified() {
    Book b;
    b.submit(Side::BUY,  10000, 100);
    b.submit(Side::SELL, 10100, 100);
    b.submit(Side::SELL, 10000, 100);   // taker SELL @ bid; mid=10050 → SELL
    ASSERT(b.lee_ready_classified_sell() == 1,   "lr_sell_1");
    ASSERT(b.lee_ready_accuracy() == 1.0,         "lr_acc_sell_1.0");
}

void test_account_vwap_single_fill() {
    Book b;
    b.submit(Side::SELL, 10100, 100, OrderType::LIMIT, TimeInForce::DAY, 0, /*client*/7);
    b.submit(Side::BUY,  10100, 100, OrderType::LIMIT, TimeInForce::DAY, 0, /*client*/8);
    ASSERT(b.account_vwap_ticks(7)  == 10100, "acct7_vwap_10100");
    ASSERT(b.account_vwap_ticks(8)  == 10100, "acct8_vwap_10100");
    ASSERT(b.account_vwap_ticks(99) == 0,     "acct99_vwap_0");
}

void test_account_vwap_two_fills() {
    Book b;
    b.submit(Side::SELL, 10100, 100, OrderType::LIMIT, TimeInForce::DAY, 0, 7);
    b.submit(Side::BUY,  10100, 100, OrderType::LIMIT, TimeInForce::DAY, 0, 8);
    b.submit(Side::SELL, 10200, 100, OrderType::LIMIT, TimeInForce::DAY, 0, 7);
    b.submit(Side::BUY,  10200, 100, OrderType::LIMIT, TimeInForce::DAY, 0, 8);
    // (10100×100 + 10200×100)/200 = 10150
    ASSERT(b.account_vwap_ticks(8) == 10150,  "acct8_vwap_10150");
}

void test_book_integrity_clean() {
    Book b;
    ASSERT(b.audit_book_integrity() == 0, "integrity_empty_0");
    b.submit(Side::BUY, 10000, 100);
    b.submit(Side::BUY, 10000, 50);
    b.submit(Side::SELL, 10010, 80);
    ASSERT(b.audit_book_integrity() == 0, "integrity_after_submits_0");
    b.submit(Side::BUY, 10010, 30);      // partial fill maker ask
    ASSERT(b.audit_book_integrity() == 0, "integrity_after_fill_0");
    auto id = b.submit(Side::BUY, 9999, 10);
    b.cancel(id);
    ASSERT(b.audit_book_integrity() == 0, "integrity_after_cancel_0");
}

void test_partial_fill_rest_displays_remaining() {
    Book b;
    b.submit(Side::SELL, 10100, 30);
    // Taker BUY 100 @ 10100: fill 30, resztka 70 wchodzi do księgi
    b.submit(Side::BUY, 10100, 100);
    // L2 depth na 10100 (bid side) powinno pokazywać 70, nie 100
    ASSERT(b.total_volume_at_price(10100) == 70, "partial_rest_shows_70");
    ASSERT(b.audit_book_integrity() == 0,         "partial_rest_integrity");
}

// ──────────────────────────────────────────────
// Hidden accounting drift + auction depth (regresje #39)
// ──────────────────────────────────────────────

void test_hidden_no_drift_after_partial_cancel() {
    Book b;
    auto id = b.submit(Side::BUY, 10000, 100);   // maker
    b.submit(Side::SELL, 10000, 30);              // partial fill 30
    b.cancel(id);                                  // cancel resztki 70
    // Stara formuła T-D w unlink driftowała total_hidden do -30 → ratio 1.0
    ASSERT(b.hidden_liquidity_ratio() == 0.0, "hidden_no_drift_0");
    ASSERT(b.audit_book_integrity() == 0,      "hidden_drift_audit_0");
}

void test_iceberg_partial_cancel_hidden_consistent() {
    Book b;
    auto ice = b.submit(Side::SELL, 10100, 1000, OrderType::ICEBERG,
                         TimeInForce::DAY, 0, 0, /*displayed=*/100);
    b.submit(Side::BUY, 10100, 150);   // 100 displayed + refresh + 50
    b.cancel(ice);
    ASSERT(b.hidden_liquidity_ratio() == 0.0, "ice_partial_cxl_ratio_0");
    ASSERT(b.audit_book_integrity() == 0,      "ice_partial_cxl_audit_0");
}

void test_auction_partial_fill_depth_correct() {
    Book b;
    b.enter_auction_mode();
    b.submit(Side::BUY,  10000, 100);
    b.submit(Side::SELL, 10000, 60);
    b.exit_auction_mode();
    auto r = b.run_auction();
    ASSERT(r.executed && r.matched_qty == 60,    "auction_pf_matched_60");
    // BUY partial: 60 filled, 40 resztki → depth == 40 (stary kod: 100)
    ASSERT(b.total_volume_at_price(10000) == 40, "auction_pf_depth_40");
    ASSERT(b.audit_book_integrity() == 0,         "auction_pf_audit_0");
}

void test_auction_full_fill_clean_book() {
    Book b;
    b.enter_auction_mode();
    b.submit(Side::BUY,  10000, 50);
    b.submit(Side::SELL, 10000, 50);
    b.exit_auction_mode();
    auto r = b.run_auction();
    ASSERT(r.executed && r.matched_qty == 50,    "auction_ff_matched_50");
    ASSERT(b.total_volume_at_price(10000) == 0,  "auction_ff_depth_0");
    ASSERT(b.audit_book_integrity() == 0,         "auction_ff_audit_0");
}

// ──────────────────────────────────────────────
// Snapshot round-trip integrity + next_id + Hurst (#40)
// ──────────────────────────────────────────────

void test_snapshot_roundtrip_audit_clean() {
    Book b;
    b.submit(Side::BUY, 10000, 100);
    b.submit(Side::SELL, 10000, 30);    // partial fill maker (F=30, D=70)
    b.submit(Side::SELL, 10100, 1000, OrderType::ICEBERG,
             TimeInForce::DAY, 0, 0, /*displayed=*/100);
    std::vector<std::uint8_t> buf(b.snapshot_size_estimate() + 64);
    const auto written = b.serialize_snapshot(buf.data(), buf.size());
    ASSERT(written > 0,                            "rt_snap_written");
    Book b2;
    ASSERT(b2.load_snapshot(buf.data(), written),  "rt_snap_loaded");
    ASSERT(b2.audit_book_integrity() == 0,          "rt_snap_audit_0");
    ASSERT(b2.total_volume_at_price(10000) == 70,   "rt_snap_depth_70");
    ASSERT(b2.total_volume_at_price(10100) == 100,  "rt_snap_ice_100");
}

void test_snapshot_next_id_no_collision() {
    Book b;
    auto id1 = b.submit(Side::BUY, 10000, 100);    // auto id
    std::vector<std::uint8_t> buf(b.snapshot_size_estimate() + 64);
    const auto written = b.serialize_snapshot(buf.data(), buf.size());
    Book b2;
    ASSERT(b2.load_snapshot(buf.data(), written),  "id_snap_loaded");
    auto id2 = b2.submit(Side::BUY, 9999, 50);     // auto id — musi być nowy
    ASSERT(id2 != 0,                                "id_new_accepted");
    ASSERT(id2 != id1,                              "id_no_collision");
    ASSERT(b2.find_order(id1) != nullptr,           "id_old_still_reachable");
    ASSERT(b2.find_order(id2) != nullptr,           "id_new_reachable");
}

void test_hurst_zero_few_trades() {
    Book b;
    b.submit(Side::SELL, 10100, 50);
    b.submit(Side::BUY,  10100, 50);
    ASSERT(b.hurst_rs_estimate() == 0.0, "hurst_zero_lt8");
}

void test_hurst_flat_returns_half() {
    Book b;
    for (int i = 0; i < 10; ++i) {
        b.submit(Side::SELL, 10100, 50);
        b.submit(Side::BUY,  10100, 50);
    }
    ASSERT(b.hurst_rs_estimate() == 0.5, "hurst_flat_0.5");
}

void test_hurst_trending_bounded() {
    Book b;
    for (int i = 0; i < 12; ++i) {
        b.submit(Side::SELL, 10100 + i * 10, 50);
        b.submit(Side::BUY,  10100 + i * 10, 50);
    }
    // Monotoniczny trend → H > 0.5 (persistence); single-window crude,
    // więc tylko luźne bounds
    const double h = b.hurst_rs_estimate();
    ASSERT(h > 0.5 && h < 1.2, "hurst_trend_gt_half");
}

// ──────────────────────────────────────────────
// Modify in-place accounting (regresje #41)
// ──────────────────────────────────────────────

void test_modify_down_iceberg_hidden_consistent() {
    Book b;
    auto ice = b.submit(Side::SELL, 10100, 1000, OrderType::ICEBERG,
                         TimeInForce::DAY, 0, 0, /*displayed=*/100);
    // Decrease 1000 → 500: 100 z displayed, 400 z hidden reserve.
    // Stary kod nie ruszał total_hidden → audit łapał violation.
    ASSERT(b.modify(ice, 10100, 500) == ice,        "mod_ice_same_id");
    ASSERT(b.audit_book_integrity() == 0,            "mod_ice_audit_0");
    auto* o = b.find_order(ice);
    ASSERT(o && o->total_qty == 500,                 "mod_ice_total_500");
}

void test_modify_down_iceberg_neighbor_depth() {
    Book b;
    auto ice = b.submit(Side::SELL, 10100, 1000, OrderType::ICEBERG,
                         TimeInForce::DAY, 0, 0, /*displayed=*/100);
    b.submit(Side::SELL, 10100, 50);                 // sąsiad na tym samym levelu
    (void)b.modify(ice, 10100, 500);
    // Stary kod odejmował min(delta, level_total)=150 od level total →
    // depth 0 i zgubione 50 sąsiada; ma być 50
    ASSERT(b.total_volume_at_price(10100) == 50,     "mod_ice_neighbor_50");
    ASSERT(b.audit_book_integrity() == 0,             "mod_ice_neighbor_audit");
}

void test_modify_down_limit_depth_still_correct() {
    Book b;
    auto id = b.submit(Side::BUY, 10000, 100);
    b.submit(Side::BUY, 10000, 50);
    (void)b.modify(id, 10000, 60);                    // delta 40, czysty displayed
    ASSERT(b.total_volume_at_price(10000) == 110,     "mod_limit_depth_110");
    ASSERT(b.audit_book_integrity() == 0,              "mod_limit_audit_0");
}

// ──────────────────────────────────────────────
// OCO — One-Cancels-Other (#42)
// ──────────────────────────────────────────────

void test_oco_link_and_partner() {
    Book b;
    auto a = b.submit(Side::SELL, 10100, 100);
    auto c = b.submit(Side::BUY,  9900,  50);
    ASSERT(b.link_oco(a, c),                       "oco_link_ok");
    ASSERT(b.oco_partner_of(a) == c,                "oco_partner_a");
    ASSERT(b.oco_partner_of(c) == a,                "oco_partner_c");
    ASSERT(b.active_oco_pairs() == 1,               "oco_pairs_1");
}

void test_oco_link_rejects_bad_args() {
    Book b;
    auto a = b.submit(Side::SELL, 10100, 100);
    ASSERT(!b.link_oco(a, a),                       "oco_no_self_link");
    ASSERT(!b.link_oco(a, 9999),                    "oco_no_ghost_partner");
    ASSERT(!b.link_oco(0, a),                       "oco_no_zero_id");
}

void test_oco_fill_cancels_partner() {
    Book b;
    auto a = b.submit(Side::SELL, 10100, 100);   // take-profit leg
    auto c = b.submit(Side::BUY,  9900,  50);    // stop-side leg
    b.link_oco(a, c);
    b.submit(Side::BUY, 10100, 100);             // taker — full fill nogi a
    ASSERT(b.find_order(a) == nullptr,              "oco_leg_a_filled");
    ASSERT(b.find_order(c) == nullptr,              "oco_leg_c_auto_cxl");
    ASSERT(b.active_oco_pairs() == 0,               "oco_pairs_0_after");
    ASSERT(b.oco_triggered_cancels() == 1,          "oco_trig_1");
    ASSERT(b.audit_book_integrity() == 0,            "oco_audit_0");
}

void test_oco_cancel_cancels_partner() {
    Book b;
    auto a = b.submit(Side::SELL, 10100, 100);
    auto c = b.submit(Side::BUY,  9900,  50);
    b.link_oco(a, c);
    b.cancel(a);
    ASSERT(b.find_order(c) == nullptr,              "oco_cxl_partner_gone");
    ASSERT(b.oco_triggered_cancels() == 1,          "oco_cxl_trig_1");
    ASSERT(b.active_oco_pairs() == 0,               "oco_cxl_pairs_0");
}

void test_oco_unlink_breaks_pair() {
    Book b;
    auto a = b.submit(Side::SELL, 10100, 100);
    auto c = b.submit(Side::BUY,  9900,  50);
    b.link_oco(a, c);
    ASSERT(b.unlink_oco(a),                         "oco_unlink_ok");
    b.submit(Side::BUY, 10100, 100);                // full fill nogi a
    // Po unlink noga c przeżywa fill nogi a
    ASSERT(b.find_order(c) != nullptr,              "oco_unlinked_survives");
    ASSERT(b.oco_triggered_cancels() == 0,          "oco_unlink_no_trig");
}

// ──────────────────────────────────────────────
// Bracket orders + pending STOP cancel (#43)
// ──────────────────────────────────────────────

void test_cancel_pending_stop_direct() {
    Book b;
    auto sid = b.submit_stop(Side::SELL, 9900, 0, 100);
    ASSERT(sid != 0 && b.stop_orders_count() == 1, "stop_pending_1");
    // Stara wersja: cancel_internal wymagał is_active() → STOP NEW niemożliwy
    // do anulowania (kill switch go pomijał)
    ASSERT(b.cancel(sid),                          "stop_cancellable");
    ASSERT(b.stop_orders_count() == 0,             "stop_removed");
    ASSERT(b.find_order(sid) == nullptr,           "stop_not_in_index");
}

void test_mass_cancel_includes_pending_stops() {
    Book b;
    b.submit_stop(Side::SELL, 9900, 0, 100, 0, /*client*/5);
    b.submit(Side::BUY, 10000, 50, OrderType::LIMIT, TimeInForce::DAY, 0, 5);
    const auto n = b.mass_cancel(5);
    ASSERT(n == 2,                                 "mass_cxl_inc_stop_2");
    ASSERT(b.stop_orders_count() == 0,             "mass_cxl_stop_gone");
}

void test_bracket_arms_on_immediate_fill() {
    Book b;
    b.submit(Side::SELL, 10100, 100);              // liquidity dla entry
    auto e = b.submit_bracket(Side::BUY, 10100, 100, /*tp*/10200, /*sl*/9900);
    ASSERT(e != 0,                                 "bracket_entry_ok");
    ASSERT(b.brackets_armed() == 1,                "bracket_armed_now");
    ASSERT(b.active_oco_pairs() == 1,              "bracket_oco_pair");
    ASSERT(b.stop_orders_count() == 1,             "bracket_sl_pending");
    ASSERT(b.total_volume_at_price(10200) == 100,  "bracket_tp_resting");
    ASSERT(b.audit_book_integrity() == 0,           "bracket_audit_0");
}

void test_bracket_arms_on_later_maker_fill() {
    Book b;
    auto e = b.submit_bracket(Side::BUY, 10000, 100, 10200, 9900);
    ASSERT(e != 0,                                 "bracket2_entry_ok");
    ASSERT(b.pending_bracket_specs() == 1,         "bracket2_spec_waiting");
    ASSERT(b.brackets_armed() == 0,                "bracket2_not_armed_yet");
    b.submit(Side::SELL, 10000, 100);              // taker fills entry maker
    ASSERT(b.pending_bracket_specs() == 0,         "bracket2_spec_consumed");
    ASSERT(b.brackets_armed() == 1,                "bracket2_armed");
    ASSERT(b.stop_orders_count() == 1,             "bracket2_sl_pending");
}

void test_bracket_tp_fill_cancels_sl() {
    Book b;
    b.submit(Side::SELL, 10100, 100);
    b.submit_bracket(Side::BUY, 10100, 100, 10200, 9900);   // armed od razu
    b.submit(Side::BUY, 10200, 100);               // taker fills TP
    // OCO: full fill TP → pending SL stop auto-cancelled (ścieżka STOP/NEW)
    ASSERT(b.stop_orders_count() == 0,             "bracket_sl_auto_cxl");
    ASSERT(b.oco_triggered_cancels() == 1,         "bracket_oco_trig_1");
    ASSERT(b.active_oco_pairs() == 0,              "bracket_pairs_0");
}

void test_bracket_cancel_entry_disarms() {
    Book b;
    auto e = b.submit_bracket(Side::BUY, 10000, 100, 10200, 9900);
    b.cancel(e);
    ASSERT(b.pending_bracket_specs() == 0,         "bracket_cxl_disarmed");
    ASSERT(b.brackets_armed() == 0,                "bracket_cxl_no_arm");
    ASSERT(b.stop_orders_count() == 0,             "bracket_cxl_no_sl");
}

// ──────────────────────────────────────────────
// Trailing stop (#44)
// ──────────────────────────────────────────────

void test_trailing_stop_initial_trigger_from_last_trade() {
    Book b;
    b.submit(Side::SELL, 10000, 10);
    b.submit(Side::BUY,  10000, 10);    // last_trade = 10000
    auto ts = b.submit_trailing_stop(Side::SELL, /*offset*/50, /*limit*/0, 100);
    ASSERT(ts != 0,                                 "trail_accepted");
    auto* o = b.find_order(ts);
    ASSERT(o && o->stop_trigger_ticks == 9950,      "trail_init_trig_9950");
    ASSERT(b.trailing_stops_count() == 1,           "trail_count_1");
}

void test_trailing_stop_ratchets_up_on_rally() {
    Book b;
    b.submit(Side::SELL, 10000, 10);
    b.submit(Side::BUY,  10000, 10);
    auto ts = b.submit_trailing_stop(Side::SELL, 50, 0, 100);   // trigger 9950
    b.submit(Side::SELL, 10100, 10);
    b.submit(Side::BUY,  10100, 10);    // rally → last_trade 10100
    b.check_stop_triggers();             // ratchet: 10100-50 = 10050
    auto* o = b.find_order(ts);
    ASSERT(o && o->stop_trigger_ticks == 10050,     "trail_ratchet_10050");
    ASSERT(b.trailing_ratchet_count() == 1,         "trail_ratchet_cnt_1");
    ASSERT(b.stop_orders_count() == 1,              "trail_still_pending");
}

void test_trailing_stop_triggers_on_drop() {
    Book b;
    b.submit(Side::SELL, 10000, 10);
    b.submit(Side::BUY,  10000, 10);
    b.submit_trailing_stop(Side::SELL, 50, 0, 100); // trigger 9950
    b.submit(Side::SELL, 10100, 10);
    b.submit(Side::BUY,  10100, 10);    // rally
    b.check_stop_triggers();             // ratchet → 10050
    b.submit(Side::BUY, 10040, 100);    // bid — liquidity dla market stopa
    b.submit(Side::SELL, 10050, 10);
    b.submit(Side::BUY,  10050, 10);    // drop → last_trade 10050
    b.check_stop_triggers();             // 10050 <= 10050 → TRIGGER → market SELL
    ASSERT(b.stop_orders_count() == 0,              "trail_triggered");
    ASSERT(b.stats().total_stop_triggers == 1,      "trail_trig_stat");
    // Market SELL 100 zjadł bid 10040
    ASSERT(b.total_volume_at_price(10040) == 0,     "trail_filled_bid");
}

void test_trailing_stop_buy_ratchets_down() {
    Book b;
    b.submit(Side::SELL, 10000, 10);
    b.submit(Side::BUY,  10000, 10);
    auto ts = b.submit_trailing_stop(Side::BUY, 30, 0, 100);    // trigger 10030
    b.submit(Side::SELL, 9900, 10);
    b.submit(Side::BUY,  9900, 10);     // drop → low-water 9900
    b.check_stop_triggers();             // ratchet down: 9900+30 = 9930
    auto* o = b.find_order(ts);
    ASSERT(o && o->stop_trigger_ticks == 9930,      "trail_buy_ratchet_9930");
    ASSERT(b.stop_orders_count() == 1,              "trail_buy_pending");
}

void test_trailing_stop_rejects_without_reference() {
    Book b;   // pusta księga — brak last_trade i brak mid
    RejectReason rr = RejectReason::NONE;
    auto ts = b.submit_trailing_stop(Side::SELL, 50, 0, 100, 0, 0, &rr);
    ASSERT(ts == 0,                                 "trail_no_ref_rejected");
    ASSERT(rr == RejectReason::PRICE_OUT_OF_RANGE,  "trail_no_ref_reason");
}

// ──────────────────────────────────────────────
// Market-On-Close (#46)
// ──────────────────────────────────────────────

void test_moc_queue_and_cancel() {
    Book b;
    auto m1 = b.submit_moc(Side::BUY, 100);
    ASSERT(m1 != 0,                              "moc_accepted");
    ASSERT(b.moc_queue_size() == 1,              "moc_q_1");
    ASSERT(b.cancel_moc(m1),                     "moc_cxl_ok");
    ASSERT(b.moc_queue_size() == 0,              "moc_q_0");
    ASSERT(b.submit_moc(Side::BUY, 0) == 0,      "moc_reject_qty0");
}

void test_moc_fills_at_clearing() {
    Book b;
    b.enter_auction_mode();
    b.submit(Side::SELL, 10005, 100);
    b.submit(Side::BUY,  10005, 50);
    b.exit_auction_mode();
    b.submit_moc(Side::BUY, 50);
    auto r = b.run_closing_auction();
    ASSERT(r.executed,                           "moc_cross_executed");
    ASSERT(r.matched_qty == 100,                 "moc_matched_100");
    ASSERT(b.moc_cancelled_unfilled() == 0,      "moc_no_leftover");
    ASSERT(b.total_volume_at_price(10005) == 0,  "moc_book_clean");
    ASSERT(b.audit_book_integrity() == 0,         "moc_audit_0");
}

void test_moc_remainder_cancelled() {
    Book b;
    b.enter_auction_mode();
    b.submit(Side::SELL, 10005, 30);    // tylko 30 kontra-liquidity
    b.exit_auction_mode();
    b.submit_moc(Side::BUY, 100);
    auto r = b.run_closing_auction();
    ASSERT(r.executed,                           "moc_pf_executed");
    ASSERT(r.matched_qty == 30,                  "moc_pf_matched_30");
    // Resztka 70 nie restuje — market semantics
    ASSERT(b.moc_cancelled_unfilled() == 1,      "moc_pf_leftover_cxl");
    ASSERT(b.total_volume_at_price(10005) == 0,  "moc_pf_book_clean");
    ASSERT(b.audit_book_integrity() == 0,         "moc_pf_audit_0");
}

void test_moc_empty_book_all_cancelled() {
    Book b;
    b.submit_moc(Side::BUY, 100);
    b.submit_moc(Side::SELL, 50);
    auto r = b.run_closing_auction();
    ASSERT(!r.executed,                          "moc_empty_no_exec");
    ASSERT(b.moc_queue_size() == 0,              "moc_empty_q_cleared");
    ASSERT(b.moc_cancelled_unfilled() == 2,      "moc_empty_cxl_2");
}

// ──────────────────────────────────────────────
// NOII (indicative auction) + Limit-On-Close (#47)
// ──────────────────────────────────────────────

void test_noii_indicative_without_execution() {
    Book b;
    b.enter_auction_mode();
    b.submit(Side::BUY,  10005, 100);
    b.submit(Side::SELL, 10005, 60);
    b.exit_auction_mode();
    auto noii = b.indicative_auction_info();
    ASSERT(noii.executed,                         "noii_would_cross");
    ASSERT(noii.clearing_price_ticks == 10005,    "noii_clearing_10005");
    ASSERT(noii.matched_qty == 60,                "noii_matched_60");
    ASSERT(noii.surplus_bid_qty == 40,            "noii_surplus_bid_40");
    // Księga NIETKNIĘTA — oba ordery nadal resting (100 + 60 displayed)
    ASSERT(b.total_volume_at_price(10005) == 160, "noii_book_untouched");
    // Faktyczny cross zgadza się z indicative
    auto r = b.run_auction();
    ASSERT(r.executed && r.matched_qty == 60 &&
           r.clearing_price_ticks == 10005,       "noii_matches_actual");
}

void test_loc_queue_and_cancel() {
    Book b;
    auto l = b.submit_loc(Side::SELL, 10005, 50);
    ASSERT(l != 0 && b.loc_queue_size() == 1,     "loc_q_1");
    ASSERT(b.cancel_loc(l),                       "loc_cxl_ok");
    ASSERT(b.loc_queue_size() == 0,               "loc_q_0");
    ASSERT(b.submit_loc(Side::SELL, 10005, 0) == 0, "loc_reject_qty0");
}

void test_loc_fills_and_remainder_cancelled() {
    Book b;
    b.enter_auction_mode();
    b.submit(Side::BUY, 10005, 50);     // kontra tylko 50
    b.exit_auction_mode();
    b.submit_loc(Side::SELL, 10005, 80);
    auto r = b.run_closing_auction();
    ASSERT(r.executed && r.matched_qty == 50,     "loc_cross_50");
    // Resztka 30 nie restuje — on-close semantics
    ASSERT(b.loc_cancelled_unfilled() == 1,       "loc_leftover_cxl");
    ASSERT(b.total_volume_at_price(10005) == 0,   "loc_book_clean");
    ASSERT(b.audit_book_integrity() == 0,          "loc_audit_0");
}

void test_loc_provides_price_discovery_for_moc() {
    Book b;   // pusta księga — LOC daje cenę, MOC crossuje z nim
    b.submit_loc(Side::SELL, 10005, 50);
    b.submit_moc(Side::BUY, 50);
    auto r = b.run_closing_auction();
    ASSERT(r.executed && r.matched_qty == 50 &&
           r.clearing_price_ticks == 10005,       "loc_moc_cross_10005");
    ASSERT(b.moc_cancelled_unfilled() == 0,       "loc_moc_no_leftover");
    ASSERT(b.loc_cancelled_unfilled() == 0,       "loc_no_leftover");
    ASSERT(b.audit_book_integrity() == 0,          "loc_moc_audit_0");
}

// ──────────────────────────────────────────────
// SSR (uptick rule) + reduce-only (#48)
// ──────────────────────────────────────────────

void test_ssr_blocks_short_at_or_below_bid() {
    Book b;
    b.submit(Side::BUY, 10000, 100);
    b.set_ssr_active(true);
    RejectReason rr = RejectReason::NONE;
    ASSERT(b.submit_short(10000, 50, 0, 0, &rr) == 0, "ssr_block_at_bid");
    ASSERT(rr == RejectReason::SSR_RESTRICTED,         "ssr_reason");
    ASSERT(b.submit_short(9999, 50) == 0,              "ssr_block_below_bid");
    auto ok = b.submit_short(10001, 50);
    ASSERT(ok != 0,                                    "ssr_allows_above_bid");
    ASSERT(b.total_volume_at_price(10001) == 50,       "ssr_short_rests");
}

void test_ssr_inactive_allows_short_into_bid() {
    Book b;
    b.submit(Side::BUY, 10000, 100);
    auto id = b.submit_short(10000, 50);    // SSR off → crossuje bid
    ASSERT(id != 0,                                    "ssr_off_accepted");
    ASSERT(b.stats().total_fills == 1,                 "ssr_off_filled");
}

void test_ssr_circuit_breaker_trips_on_decline() {
    Book b;
    b.arm_ssr_circuit_breaker(10000, 1000);    // 10% → trigger 9000
    ASSERT(!b.ssr_active(),                            "ssr_not_yet");
    b.submit(Side::SELL, 9500, 10);
    b.submit(Side::BUY,  9500, 10);             // trade 9500 > 9000 → no trip
    ASSERT(!b.ssr_active(),                            "ssr_still_off_9500");
    b.submit(Side::SELL, 9000, 10);
    b.submit(Side::BUY,  9000, 10);             // trade 9000 ≤ 9000 → TRIP
    ASSERT(b.ssr_active(),                             "ssr_tripped");
    ASSERT(b.ssr_trips() == 1,                         "ssr_trips_1");
}

void test_reduce_only_clamps_to_position() {
    Book b;
    b.submit(Side::SELL, 10100, 100, OrderType::LIMIT, TimeInForce::DAY, 0, 9);
    b.submit(Side::BUY,  10100, 100, OrderType::LIMIT, TimeInForce::DAY, 0, 7);
    // client 7 long 100 → reduce-only SELL 150 clamped do 100
    auto id = b.submit_reduce_only(Side::SELL, 10200, 150, 7);
    ASSERT(id != 0,                                    "ro_accepted");
    auto* o = b.find_order(id);
    ASSERT(o && o->total_qty == 100,                   "ro_clamped_100");
}

void test_reduce_only_rejects_without_position() {
    Book b;
    RejectReason rr = RejectReason::NONE;
    ASSERT(b.submit_reduce_only(Side::SELL, 10000, 50, 7, &rr) == 0,
                                                       "ro_no_pos_rejected");
    ASSERT(rr == RejectReason::REDUCE_ONLY_NO_POSITION, "ro_reason");
    ASSERT(b.rejections_by_reason(RejectReason::REDUCE_ONLY_NO_POSITION) == 1,
                                                       "ro_tally");
    // Zły kierunek: long position + reduce-only BUY też odpada
    Book c;
    c.submit(Side::SELL, 10100, 100, OrderType::LIMIT, TimeInForce::DAY, 0, 9);
    c.submit(Side::BUY,  10100, 100, OrderType::LIMIT, TimeInForce::DAY, 0, 7);
    ASSERT(c.submit_reduce_only(Side::BUY, 10000, 50, 7) == 0,
                                                       "ro_wrong_dir_rejected");
}

// ──────────────────────────────────────────────
// Replay determinism (#49)
// ──────────────────────────────────────────────

void test_replay_determinism_identical_state() {
    // Ta sama sekwencja operacji na dwóch księgach musi dać identyczny stan.
    // Chroni przed nondeterminizmem (np. iteracja po unordered_map wpływająca
    // na wynik matchingu). Scenariusz przechodzi przez: limit, partial taker,
    // iceberg, modify (price change), cancel, OCO fill-cancels-partner.
    auto scenario = [](Book& b) {
        b.submit(Side::BUY,  10000, 100);
        b.submit(Side::SELL, 10010, 80);
        b.submit(Side::BUY,  10010, 30);                  // partial taker
        auto x = b.submit(Side::BUY, 9995, 50);
        b.submit(Side::SELL, 10100, 1000, OrderType::ICEBERG,
                 TimeInForce::DAY, 0, 0, /*displayed=*/100);
        (void)b.modify(x, 9996, 40);                      // price change, ten sam id
        b.submit(Side::SELL, 10000, 40);                  // hits bid
        b.cancel(x);
        auto a1 = b.submit(Side::SELL, 10005, 20);
        auto a2 = b.submit(Side::BUY,  9990, 20);
        b.link_oco(a1, a2);
        b.submit(Side::BUY, 10005, 20);                   // fill a1 → OCO kasuje a2
    };
    Book b1;
    Book b2;
    scenario(b1);
    scenario(b2);
    ASSERT(b1.best_bid_ticks() == b2.best_bid_ticks(),       "det_bid_eq");
    ASSERT(b1.best_ask_ticks() == b2.best_ask_ticks(),       "det_ask_eq");
    ASSERT(b1.stats().total_fills == b2.stats().total_fills, "det_fills_eq");
    ASSERT(b1.stats().total_volume == b2.stats().total_volume, "det_vol_eq");
    ASSERT(b1.stats().total_orders_added ==
           b2.stats().total_orders_added,                     "det_added_eq");
    ASSERT(b1.active_orders() == b2.active_orders(),          "det_active_eq");
    for (std::int32_t px : {9990, 9995, 9996, 10000, 10005, 10010, 10100}) {
        ASSERT(b1.total_volume_at_price(px) ==
               b2.total_volume_at_price(px),                  "det_depth_eq");
    }
    ASSERT(b1.audit_book_integrity() == 0,                    "det_audit1_0");
    ASSERT(b2.audit_book_integrity() == 0,                    "det_audit2_0");
}

// ──────────────────────────────────────────────
// Iceberg custom refresh size (#51)
// ──────────────────────────────────────────────

void test_iceberg_refresh_uses_original_display() {
    Book b;
    b.submit(Side::SELL, 10100, 1000, OrderType::ICEBERG,
             TimeInForce::DAY, 0, 0, /*displayed=*/250);
    ASSERT(b.total_volume_at_price(10100) == 250,  "ice_init_250");
    b.submit(Side::BUY, 10100, 250);    // zjada cały displayed
    // Refresh do ORYGINALNEGO display size 250 (stary kod: hardcoded 100)
    ASSERT(b.total_volume_at_price(10100) == 250,  "ice_refresh_250");
    ASSERT(b.iceberg_refresh_count() == 1,         "ice_refresh_cnt_1");
    ASSERT(b.audit_book_integrity() == 0,           "ice_refresh_audit_0");
}

void test_iceberg_display_clamped_to_qty() {
    Book b;
    // displayed > qty — stary kod wpuszczał displayed=200 przy total=100
    // (level total 200, hidden -100 → audit violation)
    b.submit(Side::SELL, 10100, 100, OrderType::ICEBERG,
             TimeInForce::DAY, 0, 0, /*displayed=*/200);
    ASSERT(b.total_volume_at_price(10100) == 100,  "ice_clamp_100");
    ASSERT(b.audit_book_integrity() == 0,           "ice_clamp_audit_0");
}

void test_iceberg_snapshot_preserves_refresh_size() {
    Book b;
    b.submit(Side::SELL, 10100, 1000, OrderType::ICEBERG,
             TimeInForce::DAY, 0, 0, /*displayed=*/250);
    std::vector<std::uint8_t> buf(b.snapshot_size_estimate() + 64);
    const auto written = b.serialize_snapshot(buf.data(), buf.size());
    Book b2;
    ASSERT(b2.load_snapshot(buf.data(), written),  "ice_snap_loaded");
    b2.submit(Side::BUY, 10100, 250);   // zjada displayed w odtworzonej księdze
    // Refresh size przetrwał round-trip (snapshot v2)
    ASSERT(b2.total_volume_at_price(10100) == 250, "ice_snap_refresh_250");
    ASSERT(b2.audit_book_integrity() == 0,          "ice_snap_audit_0");
}

// ──────────────────────────────────────────────
// Mid-peg (#52)
// ──────────────────────────────────────────────

void test_peg_mid_initial_price() {
    Book b;
    b.submit(Side::BUY,  10000, 100);
    b.submit(Side::SELL, 10010, 100);    // mid = 10005
    auto id = b.submit_peg_mid(Side::BUY, -2, 50);   // 10005 - 2 = 10003
    ASSERT(id != 0,                                "pegmid_accepted");
    ASSERT(b.total_volume_at_price(10003) == 50,   "pegmid_at_10003");
    ASSERT(b.best_bid_ticks() == 10003,            "pegmid_becomes_bid");
}

void test_peg_mid_reprices_on_mid_change() {
    Book b;
    b.submit(Side::BUY,  10000, 100);
    b.submit(Side::SELL, 10010, 100);
    b.submit_peg_mid(Side::BUY, -2, 50);   // @ 10003
    b.submit(Side::BUY, 10007, 30);        // best_bid 10007 → mid 10008
    b.reprice_pegs();                       // target 10008-2 = 10006
    ASSERT(b.total_volume_at_price(10006) == 50,   "pegmid_moved_10006");
    ASSERT(b.total_volume_at_price(10003) == 0,    "pegmid_left_10003");
    ASSERT(b.audit_book_integrity() == 0,           "pegmid_audit_0");
}

void test_peg_mid_rejects_without_tob() {
    Book b;
    RejectReason rr = RejectReason::NONE;
    ASSERT(b.submit_peg_mid(Side::BUY, -2, 50, 0, 0, &rr) == 0,
                                                    "pegmid_no_tob_rejected");
    ASSERT(rr == RejectReason::PRICE_OUT_OF_RANGE,  "pegmid_no_tob_reason");
}

// ──────────────────────────────────────────────
// Iceberg refresh jitter (#53)
// ──────────────────────────────────────────────

void test_iceberg_jitter_within_band() {
    Book b;
    b.set_iceberg_refresh_jitter_bps(2000);   // ±20%
    b.submit(Side::SELL, 10100, 1000, OrderType::ICEBERG,
             TimeInForce::DAY, 0, 0, /*displayed=*/100);
    b.submit(Side::BUY, 10100, 100);    // zjada displayed → refresh z jitterem
    const std::int32_t refilled = b.total_volume_at_price(10100);
    ASSERT(refilled >= 80 && refilled <= 120,      "ice_jitter_band_80_120");
    ASSERT(b.audit_book_integrity() == 0,           "ice_jitter_audit_0");
}

void test_iceberg_jitter_deterministic_replay() {
    auto scenario = [](Book& b) -> std::int32_t {
        b.set_iceberg_refresh_jitter_bps(2000);
        b.submit(Side::SELL, 10100, 1000, OrderType::ICEBERG,
                 TimeInForce::DAY, 0, 0, 100);
        b.submit(Side::BUY, 10100, 100);
        return b.total_volume_at_price(10100);
    };
    Book b1;
    Book b2;
    ASSERT(scenario(b1) == scenario(b2),            "ice_jitter_replay_eq");
}

void test_iceberg_jitter_off_keeps_exact_size() {
    Book b;   // jitter default 0
    b.submit(Side::SELL, 10100, 1000, OrderType::ICEBERG,
             TimeInForce::DAY, 0, 0, 250);
    b.submit(Side::BUY, 10100, 250);
    ASSERT(b.total_volume_at_price(10100) == 250,   "ice_jitter_off_exact");
}

// ──────────────────────────────────────────────
// Auction imbalance extension (#54)
// ──────────────────────────────────────────────

void test_auction_extension_on_imbalance() {
    Book b;
    b.enter_auction_mode();
    b.submit(Side::BUY,  10005, 200);
    b.submit(Side::SELL, 10005, 50);     // surplus_bid = 150
    b.exit_auction_mode();
    auto r = b.try_run_auction(/*threshold*/100);   // 150 > 100 → extension
    ASSERT(!r.executed,                             "ext_not_crossed");
    ASSERT(r.surplus_bid_qty == 150,                "ext_surplus_150");
    ASSERT(r.clearing_price_ticks == 10005,         "ext_indicative_px");
    ASSERT(b.auction_extensions_count() == 1,       "ext_counter_1");
    // Księga NIETKNIĘTA — 200 + 50 displayed nadal na levelu
    ASSERT(b.total_volume_at_price(10005) == 250,   "ext_book_intact");
}

void test_auction_extension_then_cross() {
    Book b;
    b.enter_auction_mode();
    b.submit(Side::BUY,  10005, 200);
    b.submit(Side::SELL, 10005, 50);
    b.exit_auction_mode();
    (void)b.try_run_auction(100);        // extension
    b.enter_auction_mode();
    b.submit(Side::SELL, 10005, 150);    // kontra dosypana w extension
    b.exit_auction_mode();
    auto r = b.try_run_auction(100);     // surplus 0 → cross
    ASSERT(r.executed,                              "ext2_crossed");
    ASSERT(r.matched_qty == 200,                    "ext2_matched_200");
    ASSERT(b.auction_extensions_count() == 1,       "ext2_counter_still_1");
    ASSERT(b.total_volume_at_price(10005) == 0,     "ext2_book_clean");
    ASSERT(b.audit_book_integrity() == 0,            "ext2_audit_0");
}

void test_auction_extension_passes_below_threshold() {
    Book b;
    b.enter_auction_mode();
    b.submit(Side::BUY,  10005, 120);
    b.submit(Side::SELL, 10005, 100);    // surplus 20 ≤ threshold
    b.exit_auction_mode();
    auto r = b.try_run_auction(100);
    ASSERT(r.executed && r.matched_qty == 100,      "ext3_cross_direct");
    ASSERT(b.auction_extensions_count() == 0,       "ext3_no_extension");
}

// ──────────────────────────────────────────────
// Pegged-with-limit (#55)
// ──────────────────────────────────────────────

void test_peg_cap_limits_buy_reprice() {
    Book b;
    b.submit(Side::BUY,  10000, 100);
    b.submit(Side::SELL, 10020, 100);
    b.submit_peg(Side::BUY, 0, 50, 0, 0, nullptr, /*cap*/10003);
    b.submit(Side::BUY, 10005, 30);     // best_bid → 10005
    b.reprice_pegs();                    // target 10005, clamp → 10003
    ASSERT(b.total_volume_at_price(10003) == 50,  "peg_cap_buy_at_10003");
    ASSERT(b.audit_book_integrity() == 0,          "peg_cap_buy_audit_0");
}

void test_peg_cap_initial_clamp() {
    Book b;
    b.submit(Side::BUY,  10010, 100);
    b.submit(Side::SELL, 10030, 100);
    // initial = best_bid 10010, cap 10005 → wchodzi od razu na 10005
    b.submit_peg(Side::BUY, 0, 50, 0, 0, nullptr, /*cap*/10005);
    ASSERT(b.total_volume_at_price(10005) == 50,  "peg_cap_init_10005");
}

void test_peg_cap_sell_floor() {
    Book b;
    b.submit(Side::BUY,  10000, 100);
    b.submit(Side::SELL, 10020, 100);
    b.submit_peg(Side::SELL, 0, 50, 0, 0, nullptr, /*cap*/10015);
    b.submit(Side::SELL, 10010, 30);    // best_ask → 10010
    b.reprice_pegs();                    // target 10010, podłoga → 10015
    ASSERT(b.total_volume_at_price(10015) == 50,  "peg_cap_sell_at_10015");
    ASSERT(b.audit_book_integrity() == 0,          "peg_cap_sell_audit_0");
}

// ──────────────────────────────────────────────
// Snapshot: pending STOP + PEG rebuild + clear leak (#57)
// ──────────────────────────────────────────────

void test_snapshot_with_pending_stop_roundtrip() {
    Book b;
    b.submit(Side::BUY, 10000, 100);
    b.submit_stop(Side::SELL, /*trigger*/9900, /*limit*/9890, 50);
    ASSERT(b.stop_orders_count() == 1,              "stop_snap_pending_1");
    std::vector<std::uint8_t> buf(b.snapshot_size_estimate() + 64);
    const auto written = b.serialize_snapshot(buf.data(), buf.size());
    ASSERT(written > 0,                             "stop_snap_written");
    Book b2;
    // Stary kod: header count zawierał stopa, ale rekord nie był pisany —
    // len check failował i load zwracał false
    ASSERT(b2.load_snapshot(buf.data(), written),   "stop_snap_load_ok");
    ASSERT(b2.stop_orders_count() == 1,             "stop_snap_restored");
    ASSERT(b2.total_volume_at_price(10000) == 100,  "stop_snap_book_intact");
    // Trigger działa w odtworzonej księdze
    b2.submit(Side::SELL, 10000, 100);    // zjada bid (trade @ 10000, bez triggera)
    b2.submit(Side::BUY,  9900, 5);
    b2.submit(Side::SELL, 9900, 5);       // trade @ 9900 → warunek triggera
    b2.check_stop_triggers();
    ASSERT(b2.stop_orders_count() == 0,             "stop_snap_triggered");
    ASSERT(b2.total_volume_at_price(9890) == 50,    "stop_snap_resubmit_9890");
    ASSERT(b2.audit_book_integrity() == 0,           "stop_snap_audit_0");
}

void test_snapshot_restores_peg_reprice() {
    Book b;
    b.submit(Side::BUY,  10000, 100);
    b.submit(Side::SELL, 10020, 100);
    b.submit_peg(Side::BUY, 0, 50);     // @ 10000
    std::vector<std::uint8_t> buf(b.snapshot_size_estimate() + 64);
    const auto written = b.serialize_snapshot(buf.data(), buf.size());
    Book b2;
    ASSERT(b2.load_snapshot(buf.data(), written),   "peg_snap_load_ok");
    // Stary kod: peg trafiał do levels, ale nie do peg_orders_ → martwy peg
    ASSERT(b2.peg_orders_count() == 1,              "peg_snap_in_vector");
    b2.submit(Side::BUY, 10005, 30);
    b2.reprice_pegs();
    // Peg podążył: 10000 → 10005 (dołącza do limita 30)
    ASSERT(b2.total_volume_at_price(10005) == 80,   "peg_snap_reprices_80");
    ASSERT(b2.audit_book_integrity() == 0,           "peg_snap_audit_0");
}

void test_clear_frees_pending_stops() {
    Book b;
    b.submit_stop(Side::SELL, 9900, 0, 50);
    ASSERT(b.active_orders() == 1,                  "clear_stop_alloc_1");
    b.clear();
    // Stary kod: clear zwalniał tylko ordery z levels — pending stop
    // przeciekał (slot ani w free-list, ani osiągalny)
    ASSERT(b.active_orders() == 0,                  "clear_stop_freed");
    ASSERT(b.stop_orders_count() == 0,              "clear_stop_vector_empty");
}

// ──────────────────────────────────────────────
// Rate limiting per account (#58)
// ──────────────────────────────────────────────

void test_rate_limit_burst_capacity() {
    Book b;
    b.set_account_rate_limit(7, 3.0, 0.001);   // 3 tokeny, znikomy refill
    ASSERT(b.submit(Side::BUY, 10000, 10, OrderType::LIMIT,
                    TimeInForce::DAY, 0, 7) != 0,   "rl_1_ok");
    ASSERT(b.submit(Side::BUY, 9999,  10, OrderType::LIMIT,
                    TimeInForce::DAY, 0, 7) != 0,   "rl_2_ok");
    ASSERT(b.submit(Side::BUY, 9998,  10, OrderType::LIMIT,
                    TimeInForce::DAY, 0, 7) != 0,   "rl_3_ok");
    RejectReason rr = RejectReason::NONE;
    ASSERT(b.submit(Side::BUY, 9997, 10, OrderType::LIMIT,
                    TimeInForce::DAY, 0, 7, 0, &rr) == 0, "rl_4_rejected");
    ASSERT(rr == RejectReason::RATE_LIMITED,        "rl_reason");
    ASSERT(b.rate_limited_rejects() == 1,           "rl_counter_1");
    // Inne konto i cid 0 — bez limitu
    ASSERT(b.submit(Side::BUY, 9996, 10, OrderType::LIMIT,
                    TimeInForce::DAY, 0, 8) != 0,   "rl_other_acct_ok");
    ASSERT(b.submit(Side::BUY, 9995, 10) != 0,      "rl_cid0_ok");
}

void test_rate_limit_stop_trigger_bypasses() {
    Book b;
    b.set_account_rate_limit(7, 1.0, 0.001);   // 1 token
    b.submit_stop(Side::SELL, 9900, 9890, 50, 0, 7);
    ASSERT(b.submit(Side::BUY, 10000, 10, OrderType::LIMIT,
                    TimeInForce::DAY, 0, 7) != 0,   "rl_token_used");
    // Token wyczerpany — ale trigger stopa to internal resubmit → bypass
    b.submit(Side::SELL, 10000, 10);    // cid 0 zjada bid 7 → trade @ 10000
    b.submit(Side::BUY,  9900, 5);
    b.submit(Side::SELL, 9900, 5);      // trade @ 9900 → warunek triggera
    b.check_stop_triggers();
    ASSERT(b.stop_orders_count() == 0,              "rl_stop_triggered");
    ASSERT(b.total_volume_at_price(9890) == 50,     "rl_stop_resubmitted");
}

void test_reject_qty_zero() {
    Book b;
    RejectReason rr = RejectReason::NONE;
    auto id = b.submit(Side::BUY, 10000, 0, OrderType::LIMIT,
                        TimeInForce::DAY, 0, 0, 0, &rr);
    ASSERT(id == 0,                                     "reject_qty_zero");
    ASSERT(rr == RejectReason::QTY_ZERO_OR_NEGATIVE,    "reject_reason_qty");
}


// ──────────────────────────────────────────────
// STOP order triggers
// ──────────────────────────────────────────────

void test_stop_buy_trigger() {
    Book b;
    // Resting sell @ 10100 (płynność dla triggered STOP)
    b.submit(Side::SELL, 10100, 200);
    // STOP BUY: trigger@10050, limit=10100
    auto stop_id = b.submit_stop(Side::BUY, /*trigger=*/10050,
                                   /*limit=*/10100, 100);
    ASSERT(stop_id > 0,                              "stop_buy_accepted");
    ASSERT(b.stop_orders_count() == 1,               "stop_count_1");
    ASSERT(b.stats().total_fills == 0,               "stop_pre_trigger_no_fills");

    // Symuluj trade @ 10050 — triggeruje STOP
    b.submit(Side::SELL, 10050, 50);
    b.submit(Side::BUY,  10050, 50);   // fill → last_trade=10050
    ASSERT(b.last_trade_ticks() == 10050, "stop_last_trade_recorded");

    b.check_stop_triggers();
    ASSERT(b.stop_orders_count() == 0,                "stop_consumed");
    ASSERT(b.stats().total_stop_triggers == 1,        "stop_trigger_count");
    // STOP stał się LIMIT 10100, wykonał vs resting sell
    ASSERT(b.stats().total_volume >= 100,             "stop_executed_qty");
}

// ──────────────────────────────────────────────
// PEG orders
// ──────────────────────────────────────────────

void test_peg_initial_and_reprice() {
    Book b;
    b.submit(Side::BUY,  10000, 100);
    b.submit(Side::SELL, 10100, 100);
    // PEG buy @ best_bid - 1 (passive)
    auto peg = b.submit_peg(Side::BUY, /*offset=*/-1, 50);
    ASSERT(peg > 0,                                "peg_accepted");
    ASSERT(b.peg_orders_count() == 1,              "peg_count_1");
    const auto* po = b.find_order(peg);
    ASSERT(po && po->price_ticks == 9999,           "peg_initial_price");

    // Top of book się zmienia — bid podnosi się do 10001
    b.submit(Side::BUY, 10001, 200);
    ASSERT(b.best_bid_ticks() == 10001,             "best_bid_moved_up");
    b.reprice_pegs();
    const auto* po2 = b.find_order(peg);
    ASSERT(po2 && po2->price_ticks == 10000,         "peg_repriced_to_10000");
    ASSERT(b.stats().total_peg_reprices == 1,        "peg_reprice_counter");
}

// ──────────────────────────────────────────────
// Mass cancel
// ──────────────────────────────────────────────

void test_mass_cancel_by_client() {
    Book b;
    b.submit(Side::BUY, 10000, 100, OrderType::LIMIT, TimeInForce::DAY, 0, /*cid=*/42);
    b.submit(Side::BUY, 9999,  200, OrderType::LIMIT, TimeInForce::DAY, 0, /*cid=*/42);
    b.submit(Side::SELL,10100, 150, OrderType::LIMIT, TimeInForce::DAY, 0, /*cid=*/42);
    b.submit(Side::BUY, 10000, 100, OrderType::LIMIT, TimeInForce::DAY, 0, /*cid=*/99);

    auto cancelled = b.mass_cancel(42);
    ASSERT(cancelled == 3,                          "mass_cancel_3_for_42");
    ASSERT(b.stats().total_mass_cancels == 3,       "mass_cancel_stat");
    // client 99 ocalał
    ASSERT(b.best_bid_ticks() == 10000,             "mass_cancel_other_intact");
}

// ──────────────────────────────────────────────
// GTD expiry
// ──────────────────────────────────────────────

void test_gtd_expiry_sweep() {
    Book b;
    // Submit GTD (tu via plain submit + manualny expire_ts; expire_gtd zerknie)
    auto id1 = b.submit(Side::BUY, 10000, 100, OrderType::LIMIT, TimeInForce::GTD);
    auto id2 = b.submit(Side::BUY, 9999,  100, OrderType::LIMIT, TimeInForce::GTD);
    (void)id2;
    // Ręcznie ustaw expire_ts_ns w jednym (test path — w realu robi to caller
    // poprzez bardziej rozbudowane submit które przyjmuje expire_ts).
    auto* o1 = const_cast<Order*>(b.find_order(id1));
    if (o1) o1->expire_ts_ns = 1000;  // bardzo dawne ts

    auto expired = b.expire_gtd(/*now_ns=*/5000);
    ASSERT(expired == 1,                            "gtd_1_expired");
    ASSERT(b.stats().total_orders_expired == 1,     "gtd_expired_stat");
}

// ──────────────────────────────────────────────
// Walk-the-book preview
// ──────────────────────────────────────────────

void test_walk_the_book() {
    Book b;
    b.submit(Side::SELL, 10100, 50);
    b.submit(Side::SELL, 10110, 50);
    b.submit(Side::SELL, 10120, 100);

    // BUY 100 @ limit 10115 → consume 50 @ 10100 + 50 @ 10110 = 100 filled
    auto r = b.walk_the_book(Side::BUY, 100, 10115);
    ASSERT(r.fillable_qty == 100,                       "walk_filled_qty");
    // avg = (10100*50 + 10110*50)/100 = 10105
    ASSERT(r.avg_price_ticks == 10105,                   "walk_avg_price");
    ASSERT(r.levels_touched == 2,                        "walk_levels_2");
    ASSERT(r.worst_price_ticks == 10110,                 "walk_worst_price");
    // Sprawdź że to PREVIEW — book nietknięty
    ASSERT(b.total_volume_at_price(10100) == 50,         "walk_no_modify_book");
}

// ──────────────────────────────────────────────
// OFI (Order Flow Imbalance)
// ──────────────────────────────────────────────

void test_ofi_signal() {
    Book b;
    b.submit(Side::BUY,  10000, 100);
    b.submit(Side::SELL, 10100, 100);
    b.sample_ofi();  // baseline

    // Buy pressure: dodatkowa qty na BID
    b.submit(Side::BUY, 10000, 200);
    auto delta = b.sample_ofi();
    ASSERT(delta > 0,                          "ofi_buy_pressure_positive");
    ASSERT(b.cumulative_ofi() > 0,             "ofi_cumulative_positive");
}

// ──────────────────────────────────────────────
// Volume profile
// ──────────────────────────────────────────────

void test_volume_profile() {
    Book b;
    b.submit(Side::BUY, 10000, 100);
    b.submit(Side::BUY, 9999,  200);
    b.submit(Side::BUY, 9995,  300);
    b.submit(Side::SELL,10100, 50);

    DepthLevel prof[10];
    auto n = b.volume_profile(9990, 10100, prof, 10);
    ASSERT(n == 4,                              "vol_profile_4_levels");
    // Sortowane rosnąco: 9995, 9999, 10000, 10100
    ASSERT(prof[0].price_ticks == 9995 && prof[0].qty == 300, "vol_p0");
    ASSERT(prof[1].price_ticks == 9999 && prof[1].qty == 200, "vol_p1");
    ASSERT(prof[2].price_ticks == 10000 && prof[2].qty == 100, "vol_p2");
    ASSERT(prof[3].price_ticks == 10100 && prof[3].qty == 50,  "vol_p3");
}

// ──────────────────────────────────────────────
// Fee accounting
// ──────────────────────────────────────────────

void test_fee_accounting() {
    Book b;
    b.set_fee_bps(/*taker_bps=*/30, /*maker_bps=*/-10);  // taker pays 30, maker -10 rebate
    b.submit(Side::SELL, 10100, 100);  // maker
    b.submit(Side::BUY,  10100, 100);  // taker

    // notional_ticks = 10100 * 100 = 1010000
    // taker_basis = 1010000 * 30 = 30300000
    // maker_basis = 1010000 * (-10) = -10100000
    ASSERT(b.total_taker_fees_basis() == 30300000, "fee_taker_basis");
    ASSERT(b.total_maker_fees_basis() == -10100000, "fee_maker_rebate");
}

void test_reject_duplicate_id() {
    Book b;
    auto id = b.submit(Side::BUY, 10000, 100, OrderType::LIMIT,
                        TimeInForce::DAY, /*order_id=*/777);
    ASSERT(id == 777, "dup_first_accepted");
    RejectReason rr = RejectReason::NONE;
    auto dup = b.submit(Side::BUY, 9999, 100, OrderType::LIMIT,
                        TimeInForce::DAY, /*order_id=*/777, 0, 0, &rr);
    ASSERT(dup == 0,                            "dup_rejected");
    ASSERT(rr == RejectReason::DUPLICATE_ID,    "dup_reason");
}


// ──────────────────────────────────────────────
// Benchmark — submit + cancel cycle
// ──────────────────────────────────────────────

void benchmark(int iterations) {
    Book b;
    std::vector<std::int64_t> add_lat;
    std::vector<std::int64_t> cancel_lat;
    add_lat.reserve(iterations);
    cancel_lat.reserve(iterations);

    // Wstępna populacja — kilka levelów dla realnego match flow
    for (int i = 0; i < 20; ++i) {
        b.submit(Side::BUY,  10000 - i,  100);
        b.submit(Side::SELL, 10100 + i,  100);
    }

    const auto t0_total = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; ++i) {
        const std::int32_t side_tick = (i & 1) ? (10000 - (i % 50)) : (10100 + (i % 50));
        const Side s = (i & 1) ? Side::BUY : Side::SELL;

        const auto t0 = std::chrono::high_resolution_clock::now();
        const auto id = b.submit(s, side_tick, 10 + (i % 100));
        const auto t1 = std::chrono::high_resolution_clock::now();
        add_lat.push_back(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());

        if (id != 0 && (i & 3) == 0) {   // co 4-te zlecenie cancel
            const auto c0 = std::chrono::high_resolution_clock::now();
            b.cancel(id);
            const auto c1 = std::chrono::high_resolution_clock::now();
            cancel_lat.push_back(
                std::chrono::duration_cast<std::chrono::nanoseconds>(c1 - c0).count());
        }
    }
    const auto t1_total = std::chrono::high_resolution_clock::now();
    const auto total_us = std::chrono::duration_cast<std::chrono::microseconds>(
        t1_total - t0_total).count();

    auto pct = [](std::vector<std::int64_t>& v, double p) -> std::int64_t {
        if (v.empty()) return 0;
        std::sort(v.begin(), v.end());
        return v[static_cast<std::size_t>(p * (v.size() - 1))];
    };

    std::printf("\n=== FullOrderBook benchmark (n=%d) ===\n", iterations);
    std::printf("  Total time:     %lld us\n", static_cast<long long>(total_us));
    std::printf("  Throughput:     %.2f Mops/sec\n",
                iterations / (static_cast<double>(total_us) / 1e6) / 1e6);
    std::printf("  add p50:        %lld ns\n", static_cast<long long>(pct(add_lat, 0.50)));
    std::printf("  add p95:        %lld ns\n", static_cast<long long>(pct(add_lat, 0.95)));
    std::printf("  add p99:        %lld ns\n", static_cast<long long>(pct(add_lat, 0.99)));
    std::printf("  add p99.9:      %lld ns\n", static_cast<long long>(pct(add_lat, 0.999)));
    if (!cancel_lat.empty()) {
        std::printf("  cancel p50:     %lld ns\n", static_cast<long long>(pct(cancel_lat, 0.50)));
        std::printf("  cancel p99:     %lld ns\n", static_cast<long long>(pct(cancel_lat, 0.99)));
    }
    std::printf("  Final stats:\n");
    std::printf("    orders_added:     %lu\n", b.stats().total_orders_added);
    std::printf("    orders_cancelled: %lu\n", b.stats().total_orders_cancelled);
    std::printf("    total_fills:      %lu\n", b.stats().total_fills);
    std::printf("    total_volume:     %lu\n", b.stats().total_volume);
    std::printf("    pool peak used:   %lu / %lu\n",
                b.pool_used_high_water(), b.pool_capacity());

    // Burst aktywności PO pomiarach latencji (nie zaburza percentyli) —
    // benchmark sam nie crossuje (fills=0), a sekcja analytics potrzebuje
    // trade'ów żeby pokazać metryki w akcji.
    for (int i = 0; i < 200; ++i) {
        const std::int32_t px = 10050 + (i % 7) - 3;
        b.submit(Side::SELL, px, 20 + (i % 30));
        b.submit(Side::BUY,  px, 20 + (i % 30));
        if ((i & 15) == 0) {
            (void)b.poll_tob_change();
            b.poll_tob_micro();
            b.sample_mid_to_ring();
        }
    }
    std::printf("  Analytics snapshot (po 200-trade burst):\n");
    std::printf("    vpin_bps:           %u\n",   b.vpin_bps());
    std::printf("    flow_imbalance_bps: %d\n",   b.flow_imbalance_bps());
    std::printf("    kyle_lambda:        %.4f\n", b.kyle_lambda());
    std::printf("    lee_ready_accuracy: %.2f\n", b.lee_ready_accuracy());
    std::printf("    trades_per_second:  %.0f\n", b.trades_per_second());
    std::printf("    fano_clustering:    %.2f\n", b.trade_clustering_fano());
    std::printf("    hurst_rs:           %.2f\n", b.hurst_rs_estimate());
    std::printf("    mean_trade_qty:     %.1f\n", b.mean_trade_qty());
    std::printf("    realized_vol:       %.4f\n", b.realized_volatility_log_returns());
    std::printf("    toxicity_score_bps: %u\n",   b.toxicity_composite_score_bps());
    std::printf("    active_levels:      %d\n",   b.active_price_levels());
    std::printf("    audit_violations:   %lu\n",  b.audit_book_integrity());
}


int main(int argc, char* argv[]) {
    std::printf("=== FullOrderBook (orderbook_pro) tests ===\n");

    test_basic_add_and_match();
    test_cancel();
    test_modify_priority_preserve_down_only();
    test_ioc_takes_what_can_then_cancels();
    test_fok_kills_if_not_fully_fillable();
    test_post_only_rejects_when_would_cross();
    test_iceberg_displays_partial();
    test_top_of_book();
    test_microprice_skews_toward_smaller_side();
    test_imbalance_bps();
    test_depth_l2();
    test_queue_position_fifo();
    test_trade_tape_and_vwap();
    test_stp_cancel_newest();
    test_snapshot_roundtrip();
    test_reject_qty_zero();
    test_reject_duplicate_id();
    test_stop_buy_trigger();
    test_peg_initial_and_reprice();
    test_mass_cancel_by_client();
    test_gtd_expiry_sweep();
    test_walk_the_book();
    test_ofi_signal();
    test_volume_profile();
    test_fee_accounting();
    test_auction_clearing_price();
    test_delta_queue_basic();
    test_delta_wire_format();
    test_delta_on_cancel();
    test_spread_analytics();
    test_mass_quote_atomic();
    test_mass_quote_rejected_if_crosses();
    test_liquidity_snapshot();
    test_book_cluster_basic();
    test_book_cluster_unknown_symbol();
    test_hidden_not_in_l1_or_l2();
    test_hidden_does_not_match_continuous();
    test_halt_rejects_new_orders();
    test_resume_allows_orders();
    test_halt_does_not_block_cancel();
    test_min_qty_rejected_when_not_met();
    test_min_qty_accepted_when_met();
    test_audit_log_records_lifecycle();
    test_audit_log_records_reject();
    test_audit_disabled_by_default();
    test_luld_rejects_out_of_band();
    test_luld_accepts_within_band();
    test_luld_auto_halt();
    test_mifid_effective_spread();
    test_mifid_disabled_no_tracking();
    test_stuffing_below_threshold_not_flagged();
    test_stuffing_above_threshold_flagged();
    test_stuffing_clear_flag();
    test_exposure_open_buy();
    test_exposure_filled_net_qty();
    test_exposure_cancel_releases_open_qty();
    test_trade_size_distribution_classifies();
    test_tape_twap();
    test_reference_price_drift();
    test_reference_price_no_ref_set();
    test_rejection_rate();
    test_tob_poll_initial();
    test_tob_poll_after_new_quote();
    test_tob_poll_unchanged_when_lower_bid();
    test_tob_counter_increments();
    test_cross_arb_detected();
    test_cross_arb_none_when_aligned();
    test_sweep_single_level();
    test_sweep_multi_level();
    test_imbalance_top_3_levels();
    test_age_stats_record_on_cancel();
    test_age_stats_record_on_fill();
    test_modify_qty_down_preserves_priority();
    test_modify_price_change_loses_priority();
    test_modify_qty_up_loses_priority();
    test_priority_loss_ratio();
    test_quoted_spread_sampler();
    test_quoted_spread_skips_one_sided();
    test_effective_spread_recorded_on_fill();
    test_ctr_zero_without_fills();
    test_ctr_after_cancel_and_fill();
    test_predicted_vwap_buy_single_level();
    test_predicted_vwap_buy_multi_level();
    test_predicted_vwap_partial_liquidity();
    test_predicted_slippage_buy();
    test_depth_within_offset();
    test_quote_life_after_two_polls();
    test_spread_compression_threshold();
    test_flow_imbalance_buy_heavy();
    test_vpin_balanced();
    test_quote_flicker_no_trade();
    test_quote_flicker_zero_when_trade_intervenes();
    test_volume_at_price_accumulates();
    test_point_of_control();
    test_tob_stability_streak();
    test_time_weighted_spread_accumulates();
    test_tape_statistics_basic();
    test_for_each_order_visits_all();
    test_event_seq_monotonic();
    test_hidden_ratio_zero_for_limit();
    test_hidden_ratio_with_iceberg();
    test_resting_order_count_buy_side();
    test_aggressive_volume_taker();
    test_aggressive_ratio_mixed();
    test_cancel_to_fill_ratio_per_account();
    test_book_health_snapshot();
    test_trades_per_second_returns_nonneg();
    test_realized_vol_zero_when_flat();
    test_realized_vol_positive_when_varied();
    test_spread_bias_bid_side();
    test_spread_bias_neutral();
    test_queue_replenish_bid();
    test_kyle_lambda_zero_without_trades();
    test_kyle_lambda_nonzero_after_price_move();
    test_latency_arb_off_by_default();
    test_latency_arb_detected_same_side_back_to_back();
    test_cluster_aggregations();
    test_last_buy_fill_ts_set();
    test_iceberg_refresh_counter();
    test_largest_resting_order_qty();
    test_rejections_by_reason_counts();
    test_accepts_by_tif_counts();
    test_accepts_by_type_counts();
    test_acceptance_ratio();
    test_depth_concentration_top1_full();
    test_depth_concentration_top1_half();
    test_depth_concentration_top3_full();
    test_peg_orders_count_zero_initially();
    test_avg_resting_qty_per_order();
    test_active_price_levels();
    test_mid_ring_starts_empty();
    test_mid_ring_samples_after_quotes();
    test_mid_momentum_positive();
    test_fill_band_compliance();
    test_kyle_lambda_per_side_split();
    test_spread_regime_classifier();
    test_tob_skewness_bid_heavy();
    test_tob_skewness_balanced();
    test_mid_minus_vwap_after_trade();
    test_mid_trend_unknown_initially();
    test_mid_trend_up_after_rise();
    test_one_sided_bid_only_counted();
    test_spread_histogram_buckets();
    test_spread_histogram_median();
    test_top_k_largest_orders();
    test_price_change_hist_zero_bin();
    test_price_change_hist_up_bin();
    test_price_change_hist_tail();
    test_toxicity_composite_zero_quiet();
    test_toxicity_composite_after_activity();
    test_cluster_volume_weighted_spread();
    test_is_zero_without_fills();
    test_is_buy_pays_adverse();
    test_is_sell_negative_when_paid_below_mid();
    test_per_side_vwap_split();
    test_per_side_vwap_zero_no_trades();
    test_inter_trade_gap_after_two_trades();
    test_largest_single_trade_qty();
    test_last_n_vwap_rolling();
    test_first_fill_latency_zero_init();
    test_first_fill_latency_recorded();
    test_burst_detector_off_by_default();
    test_burst_detector_catches_rapid_submits();
    test_completion_filled_fully();
    test_completion_cancelled_unfilled();
    test_completion_cancelled_partial();
    test_fill_rate_ratio();
    test_twmid_sample_accumulates();
    test_mean_trade_qty();
    test_maker_fills_sell_side_counted();
    test_maker_fills_buy_side_counted();
    test_mean_fill_notional();
    test_cluster_flow_imbalance();
    test_top_k_active_levels_by_qty();
    test_depth_pyramid_steepness_steep();
    test_depth_pyramid_flat();
    test_cumulative_resting_volume_buy();
    test_best_qty_histogram_samples();
    test_ema_imbalance_init();
    test_ema_imbalance_smooths();
    test_microprice_ring_after_two_samples();
    test_signed_volume_ema_after_buy_trade();
    test_ofi_first_sample_baseline();
    test_ofi_bid_increases_positive_flow();
    test_ofi_bid_price_up_strong_positive();
    test_trade_clustering_fano_one_sample();
    test_trade_clustering_fano_after_trades();
    test_maker_survival_after_two_polls();
    test_markov_trending_all_buys();
    test_markov_alternating();
    test_queue_depth_at_arrival_empty_level();
    test_queue_depth_at_arrival_crowded();
    test_trade_momentum_last_n_all_buys();
    test_trade_momentum_balanced();
    test_acf_lag1_zero_without_data();
    test_acf_lag1_after_trades();
    test_slippage_guard_violations_zero_off();
    test_slippage_guard_violations_caught();
    test_nbbo_violations_zero_normal();
    test_cancellations_per_side();
    test_lee_ready_buy_classified();
    test_lee_ready_sell_classified();
    test_account_vwap_single_fill();
    test_account_vwap_two_fills();
    test_book_integrity_clean();
    test_partial_fill_rest_displays_remaining();
    test_hidden_no_drift_after_partial_cancel();
    test_iceberg_partial_cancel_hidden_consistent();
    test_auction_partial_fill_depth_correct();
    test_auction_full_fill_clean_book();
    test_snapshot_roundtrip_audit_clean();
    test_snapshot_next_id_no_collision();
    test_hurst_zero_few_trades();
    test_hurst_flat_returns_half();
    test_hurst_trending_bounded();
    test_modify_down_iceberg_hidden_consistent();
    test_modify_down_iceberg_neighbor_depth();
    test_modify_down_limit_depth_still_correct();
    test_oco_link_and_partner();
    test_oco_link_rejects_bad_args();
    test_oco_fill_cancels_partner();
    test_oco_cancel_cancels_partner();
    test_oco_unlink_breaks_pair();
    test_cancel_pending_stop_direct();
    test_mass_cancel_includes_pending_stops();
    test_bracket_arms_on_immediate_fill();
    test_bracket_arms_on_later_maker_fill();
    test_bracket_tp_fill_cancels_sl();
    test_bracket_cancel_entry_disarms();
    test_trailing_stop_initial_trigger_from_last_trade();
    test_trailing_stop_ratchets_up_on_rally();
    test_trailing_stop_triggers_on_drop();
    test_trailing_stop_buy_ratchets_down();
    test_trailing_stop_rejects_without_reference();
    test_moc_queue_and_cancel();
    test_moc_fills_at_clearing();
    test_moc_remainder_cancelled();
    test_moc_empty_book_all_cancelled();
    test_noii_indicative_without_execution();
    test_loc_queue_and_cancel();
    test_loc_fills_and_remainder_cancelled();
    test_loc_provides_price_discovery_for_moc();
    test_ssr_blocks_short_at_or_below_bid();
    test_ssr_inactive_allows_short_into_bid();
    test_ssr_circuit_breaker_trips_on_decline();
    test_reduce_only_clamps_to_position();
    test_reduce_only_rejects_without_position();
    test_replay_determinism_identical_state();
    test_iceberg_refresh_uses_original_display();
    test_iceberg_display_clamped_to_qty();
    test_iceberg_snapshot_preserves_refresh_size();
    test_peg_mid_initial_price();
    test_peg_mid_reprices_on_mid_change();
    test_peg_mid_rejects_without_tob();
    test_iceberg_jitter_within_band();
    test_iceberg_jitter_deterministic_replay();
    test_iceberg_jitter_off_keeps_exact_size();
    test_auction_extension_on_imbalance();
    test_auction_extension_then_cross();
    test_auction_extension_passes_below_threshold();
    test_peg_cap_limits_buy_reprice();
    test_peg_cap_initial_clamp();
    test_peg_cap_sell_floor();
    test_snapshot_with_pending_stop_roundtrip();
    test_snapshot_restores_peg_reprice();
    test_clear_frees_pending_stops();
    test_rate_limit_burst_capacity();
    test_rate_limit_stop_trigger_bypasses();

    std::printf("\n%d/%d tests passed", tests_passed, tests_total);
    if (tests_failed > 0) std::printf("  (%d FAILED)", tests_failed);
    std::printf("\n");

    int iterations = (argc > 1) ? std::atoi(argv[1]) : 100000;
    if (iterations > 0) benchmark(iterations);

    return (tests_failed == 0) ? 0 : 1;
}
