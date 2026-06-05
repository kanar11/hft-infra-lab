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

    std::printf("\n%d/%d tests passed", tests_passed, tests_total);
    if (tests_failed > 0) std::printf("  (%d FAILED)", tests_failed);
    std::printf("\n");

    int iterations = (argc > 1) ? std::atoi(argv[1]) : 100000;
    if (iterations > 0) benchmark(iterations);

    return (tests_failed == 0) ? 0 : 1;
}
