/*
 * MarketSim — a market data simulator for the end-to-end HFT pipeline.
 *
 * Generates synthetic ITCH messages, parses them through the ITCH parser, routes
 * orders through the OMS with risk checks, tracks P&L. This is the "glue" of all
 * the lab's components into one binary demo — it shows the whole thing works.
 *
 * Pipeline: ITCH Generator → ITCH Parser → Strategy → Router → Risk → OMS → P&L
 *
 * The RiskManager (opt-in via use_risk / the --risk flag) is the pre-trade gatekeeper:
 * every order passes check_order() BEFORE it reaches the OMS (SEC 15c3-5).
 * REJECT/KILL → the order does not go; fills feed the positions and P&L in the RiskManager
 * (circuit breaker / drawdown). See do_fill() and the [3/4] loop.
 *
 * Two variants:
 *   - run_pipeline()           — everything on one thread, sequentially.
 *   - run_pipeline_threaded()  — gen / parse-strategy / oms on separate
 *                                threads via an SPSC queue (lockfree).
 *
 * Performance (lab): ~573K msg/sec end-to-end (full pipeline).
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <chrono>
#include <algorithm>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

// All pipeline modules
#include "../itch-parser/itch_parser.hpp"
#include "../oms/oms.hpp"
#include "../risk/risk_manager.hpp"
#include "../orderbook/orderbook_pro.hpp"
#include "../config/config_loader.hpp"
#include "../strategy/mean_reversion.hpp"
#include "../strategy/market_maker.hpp"
#include "../router/smart_router.hpp"
#include "../lockfree/spsc_queue.hpp"

#include <deque>
#include <unordered_map>


// MarketDataGenerator — generates synthetic binary ITCH messages.

// A simple LCG (deterministic, zero heap allocation — important on the hot path).
struct FastRNG {
    uint64_t state;

    explicit FastRNG(uint64_t seed = 42) noexcept : state(seed) {}

    uint64_t next() noexcept {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return state >> 16;
    }

    int    rand_int(int max) noexcept { return static_cast<int>(next() % max); }
    double rand_double()      noexcept { return static_cast<double>(next() & 0xFFFFFFFF) / 4294967296.0; }
    double rand_range(double lo, double hi) noexcept { return lo + rand_double() * (hi - lo); }
};


// Symbol info for the generator.
struct StockInfo {
    char   symbol[9];
    double base_price;
};

static const StockInfo STOCKS[] = {
    {"AAPL",    175.00}, {"MSFT",    410.00}, {"GOOGL",   155.00}, {"AMZN",    185.00},
    {"TSLA",    245.00}, {"META",    500.00}, {"NVDA",    880.00}, {"JPM",     195.00},
};
static constexpr int NUM_STOCKS = 8;
static const int SHARE_SIZES[] = {10, 25, 50, 100, 200, 500};
static constexpr int NUM_SHARE_SIZES = 6;


// Active-order tracking (fixed array, zero heap on the hot path).
struct ActiveOrder {
    int64_t order_ref;
    char    stock[9];
    char    side;       // 'B' or 'S'
    double  price;
    int32_t shares;
    bool    active;
};

static constexpr int MAX_ACTIVE_ORDERS = 8192;

// Pre-built raw ITCH message buffer (max size is a 'P' frame with match_num).
static constexpr int MAX_MSG_SIZE = 64;

// A generated message — raw bytes + length.
struct GeneratedMessage {
    uint8_t data[MAX_MSG_SIZE];
    int     length;
};


class MarketDataGenerator {
    FastRNG rng_;
    int64_t seq_;
    int64_t order_ref_;
    ActiveOrder active_orders_[MAX_ACTIVE_ORDERS];
    int active_count_;
    // Scratch buffer for random_active_order — a class field so each call
    // doesn't put 32 KB on the stack. Overwritten on every use, never
    // read before being filled, so no init needed.
    int active_indices_[MAX_ACTIVE_ORDERS];
    // Symbol universe — points at pre-built StockInfo. Defaults to the hardcoded
    // STOCKS[] fallback when no config was provided.
    const StockInfo* stocks_;
    int              n_stocks_;

    // ITCH uses network byte order (big-endian) — 32/64-bit packers.
    static void pack_be64(uint8_t* buf, int64_t val) noexcept {
        for (int i = 7; i >= 0; --i) { buf[i] = val & 0xFF; val >>= 8; }
    }
    static void pack_be32(uint8_t* buf, int32_t val) noexcept {
        for (int i = 3; i >= 0; --i) { buf[i] = val & 0xFF; val >>= 8; }
    }

    int64_t next_ts() noexcept {
        ++seq_;
        return 34200000000000LL + seq_ * 1000000LL;  // 9:30 AM + seq * 1ms
    }

    // Pick a random active-order index, -1 if none.
    int random_active_order() noexcept {
        if (active_count_ == 0) return -1;
        int n = 0;
        for (int i = 0; i < MAX_ACTIVE_ORDERS && n < active_count_; ++i) {
            if (active_orders_[i].active) active_indices_[n++] = i;
        }
        if (n == 0) return -1;
        return active_indices_[rng_.rand_int(n)];
    }

public:
    explicit MarketDataGenerator(uint64_t seed = 42) noexcept
        : rng_(seed), seq_(0), order_ref_(1000), active_count_(0), active_indices_{},
          stocks_(STOCKS), n_stocks_(NUM_STOCKS) {
        for (int i = 0; i < MAX_ACTIVE_ORDERS; ++i)
            active_orders_[i].active = false;
    }

    // set_stocks: override the symbol universe with tickers from the config.
    // Ownership of the pointer stays with the caller — usually points
    // at a long-lived StockInfo vector built from HFTConfig::simulator::stocks.
    void set_stocks(const StockInfo* arr, int n) noexcept {
        if (arr && n > 0) { stocks_ = arr; n_stocks_ = n; }
    }

    int active_order_count() const noexcept { return active_count_; }

    // Generate an Add Order ('A') message.
    GeneratedMessage generate_add_order() noexcept {
        GeneratedMessage msg = {};
        const StockInfo& stock = stocks_[rng_.rand_int(n_stocks_)];
        double price = stock.base_price + rng_.rand_range(-2.0, 2.0);
        price = std::round(price * 100.0) / 100.0;
        char side = rng_.rand_int(2) ? 'S' : 'B';
        int shares = SHARE_SIZES[rng_.rand_int(NUM_SHARE_SIZES)];
        order_ref_++;

        // Record the active order.
        for (int i = 0; i < MAX_ACTIVE_ORDERS; ++i) {
            if (!active_orders_[i].active) {
                active_orders_[i].order_ref = order_ref_;
                // memcpy instead of strncpy — g++ -Wstringop-truncation flags
                // strncpy(dst, src, 8) as "may not be null-terminated", even
                // though there's an explicit stock[8] = '\0' a line below. memcpy gives identical
                // bytes and does not trigger the warning.
                std::memcpy(active_orders_[i].stock, stock.symbol, 8);
                active_orders_[i].stock[8] = '\0';
                active_orders_[i].side = side;
                active_orders_[i].price = price;
                active_orders_[i].shares = shares;
                active_orders_[i].active = true;
                active_count_++;
                break;
            }
        }

        // Pack the frame: 'A' + timestamp(8) + order_ref(8) + side(1) + shares(4) + stock(8) + price(4)
        uint8_t* d = msg.data;
        d[0] = 'A';
        pack_be64(d + 1, next_ts());
        pack_be64(d + 9, order_ref_);
        d[17] = static_cast<uint8_t>(side);
        pack_be32(d + 18, shares);
        std::memset(d + 22, ' ', 8);
        std::memcpy(d + 22, stock.symbol, std::strlen(stock.symbol));
        pack_be32(d + 30, static_cast<int32_t>(price * 10000));
        msg.length = 34;
        return msg;
    }

    // Generate an Order Executed ('E') message.
    GeneratedMessage generate_execute() noexcept {
        int idx = random_active_order();
        if (idx < 0) return generate_add_order();

        ActiveOrder& order = active_orders_[idx];
        int exec_shares = std::min(order.shares, SHARE_SIZES[rng_.rand_int(3)]);  // 10, 25, or 50
        int64_t match_num = rng_.rand_int(90000) + 10000;

        order.shares -= exec_shares;
        if (order.shares <= 0) {
            order.active = false;
            active_count_--;
        }

        GeneratedMessage msg = {};
        uint8_t* d = msg.data;
        d[0] = 'E';
        pack_be64(d + 1, next_ts());
        pack_be64(d + 9, order.order_ref);
        pack_be32(d + 17, exec_shares);
        pack_be64(d + 21, match_num);
        msg.length = 29;
        return msg;
    }

    // Generate an Order Cancelled ('C') message.
    GeneratedMessage generate_cancel() noexcept {
        int idx = random_active_order();
        if (idx < 0) return generate_add_order();

        ActiveOrder& order = active_orders_[idx];
        int cancelled = order.shares;
        order.active = false;
        active_count_--;

        GeneratedMessage msg = {};
        uint8_t* d = msg.data;
        d[0] = 'C';
        pack_be64(d + 1, next_ts());
        pack_be64(d + 9, order.order_ref);
        pack_be32(d + 17, cancelled);
        msg.length = 21;
        return msg;
    }

    // Generate a Trade ('P') message.
    GeneratedMessage generate_trade() noexcept {
        const StockInfo& stock = stocks_[rng_.rand_int(n_stocks_)];
        double price = stock.base_price + rng_.rand_range(-1.0, 1.0);
        price = std::round(price * 100.0) / 100.0;
        int shares = SHARE_SIZES[rng_.rand_int(3) + 3];  // 100, 200, 500
        char side = rng_.rand_int(2) ? 'S' : 'B';
        int64_t match_num = rng_.rand_int(90000) + 10000;

        GeneratedMessage msg = {};
        uint8_t* d = msg.data;
        d[0] = 'P';
        pack_be64(d + 1, next_ts());
        pack_be64(d + 9, 0);  // order_ref = 0 for trades
        d[17] = static_cast<uint8_t>(side);
        pack_be32(d + 18, shares);
        std::memset(d + 22, ' ', 8);
        std::memcpy(d + 22, stock.symbol, std::strlen(stock.symbol));
        pack_be32(d + 30, static_cast<int32_t>(price * 10000));
        pack_be64(d + 34, match_num);
        msg.length = 42;
        return msg;
    }

    // Generate System Event (S) message
    GeneratedMessage generate_system_event(char event_code) noexcept {
        GeneratedMessage msg = {};
        uint8_t* d = msg.data;
        d[0] = 'S';
        pack_be64(d + 1, next_ts());
        d[9] = static_cast<uint8_t>(event_code);
        msg.length = 10;
        return msg;
    }

    // Mixed messages by a probability distribution (45% A, 25% E, 15% P, 15% C).
    GeneratedMessage generate_random_message() noexcept {
        double roll = rng_.rand_double();
        if (roll < 0.45)       return generate_add_order();
        else if (roll < 0.70)  return generate_execute();
        else if (roll < 0.85)  return generate_trade();
        else                   return generate_cancel();
    }
};


// PipelineStats — the results of a simulation run.
struct PipelineStats {
    int     messages_generated;
    int     messages_parsed;
    int     orders_submitted;
    int     orders_filled;
    int     orders_rejected;         // OMS rejected (position / value limit in the OMS)
    int     orders_risk_rejected;    // RiskManager rejected BEFORE the OMS (pre-trade)
    bool    risk_kill_tripped;       // the kill switch latched during the session
    int     orders_book_partial;     // how many FullOrderBook fills were partial
    double  book_slippage_usd;       // total slippage cost from the real match
    // MarketMaker mode (--mm): the MM quotes, market flow crosses it, fills go
    // through Risk→OMS. Stats aggregated across all symbols.
    uint64_t mm_quotes_placed;       // total quote churn (cancel+replace)
    uint64_t mm_fills;               // how many times the MM was crossed (fill)
    double   mm_pnl;                 // the MM's mark-to-market P&L at the last mid
    int      mm_abs_inventory;       // Σ |net inventory| across symbols at the end
    int     max_in_flight_orders;   // peak submittted-but-unfilled
    int     fill_latency_iters;     // iterations of simulated fill-ack latency (0 = zero-latency)
    double  gen_ms;
    double  parse_ms;
    double  oms_ms;
    double  total_ms;
    double  total_pnl;

    // Message-type breakdown
    int add_orders;
    int executes;
    int trades;
    int cancels;
    int system_events;
};


// A pending fill tied to a submitted order — drained when current_iter reaches due_iter.
struct PendingFill {
    uint64_t order_id;
    int32_t  shares;
    double   price;
    int      due_iter;
};


// ============================================================================
// BookMatchEngine — an adapter wiring OMS orders to the real FullOrderBook
// matching engine (orderbook_pro). It replaces the synthetic "fill at the submit
// price" with a real match: FIFO price-time, walking multiple levels, partials,
// a real VWAP and slippage.
//
// Why a normalized tick grid, not absolute prices?
//   FullOrderBook<LEVELS=16384> @ $0.01/tick covers $0..$163.83 — not enough for
//   NVDA ($880). Instead we work in a grid around MID_TICK: the dollar price
//   = ref_price + (tick - MID_TICK) * TICK_USD. It works for ANY symbol price,
//   and the matching engine operates purely on ticks anyway.
//
// Liquidity model (per match): a ladder of counter-orders placed from the touch
// down into the book (LEVEL_LIQ shares per level). Our order enters as IOC and
// walks as many levels as needed — the larger it is, the deeper the walk = more
// slippage. After the match we cancel the ladder remnants by id (cheaper than clear() O(LEVELS)).
//
// A full, faithful book reconstruction from the ITCH stream is a separate step (#82) —
// here the point is for OUR orders to actually match in the gem of an engine.
class BookMatchEngine {
    // Types from orderbook_pro — pulled into the class scope (without polluting the global).
    using FullOrderBook = orderbook_pro::FullOrderBook<>;
    using BookEvent     = orderbook_pro::BookEvent;
    using EventType     = orderbook_pro::EventType;
    using OrderType     = orderbook_pro::OrderType;
    using TimeInForce   = orderbook_pro::TimeInForce;

    static constexpr std::int32_t MID_TICK   = 8192;   // middle of the 16384 grid
    static constexpr std::int32_t MAX_LADDER = 250;    // depth cap (< LEVELS/2)
    static constexpr double       TICK_USD   = 0.01;

    FullOrderBook book_;
    std::int32_t    level_liq_;        // shares per ladder level
    std::uint64_t   next_id_;          // shared id counter (ladder + taker)

    // Fill-capture state for the current taker (set per match).
    struct Capture {
        std::uint64_t taker_id;
        std::int64_t  qty;             // Σ exec_qty on the taker side
        std::int64_t  notional_ticks;  // Σ price_ticks * exec_qty
    } cap_{};

    // A FILL is emitted for both sides (taker and maker). We count only the taker
    // (e.order_id == taker_id) — no double counting.
    static void on_event(const BookEvent& e, void* ctx) noexcept {
        auto* c = static_cast<Capture*>(ctx);
        if (e.type == EventType::FILL && e.order_id == c->taker_id) {
            c->qty            += e.qty;
            c->notional_ticks += static_cast<std::int64_t>(e.price_ticks) * e.qty;
        }
    }

public:
    explicit BookMatchEngine(std::int32_t level_liq = 100) noexcept
        : level_liq_(level_liq > 0 ? level_liq : 100), next_id_(1) {
        book_.set_event_callback(&on_event, &cap_);
    }

    // match: match an order (side, qty) at the reference price ref_price.
    // Returns the actually filled quantity; out_vwap_usd = the average execution price.
    // When liquidity < qty → a partial fill (out_filled < qty).
    std::int32_t match(Side side, double ref_price, std::int32_t qty,
                       double& out_vwap_usd) noexcept {
        out_vwap_usd = ref_price;
        if (qty <= 0 || ref_price <= 0.0) return 0;

        // Ladder depth: cover qty + a cushion, within MAX_LADDER.
        const std::int32_t levels =
            std::min<std::int32_t>(MAX_LADDER, qty / level_liq_ + 2);
        const bool is_buy = (side == Side::BUY);

        // Place counter-liquidity: BUY hits the ASK (ascending from the touch), SELL the BID
        // (descending). We remember the ids to cancel the remnants.
        std::uint64_t ladder_ids[MAX_LADDER];
        const Side contra = is_buy ? Side::SELL : Side::BUY;
        for (std::int32_t k = 0; k < levels; ++k) {
            const std::int32_t px = is_buy ? (MID_TICK + k) : (MID_TICK - k);
            const std::uint64_t id = next_id_++;
            ladder_ids[k] = id;
            book_.submit(contra, px, level_liq_, OrderType::LIMIT,
                         TimeInForce::DAY, id, /*client_id=*/0);
        }

        // Taker as IOC: the limit is set so it can walk the whole ladder.
        cap_ = Capture{ next_id_++, 0, 0 };
        const std::int32_t taker_px = is_buy ? (MID_TICK + levels) : (MID_TICK - levels);
        book_.submit(side, taker_px, qty, OrderType::IOC,
                     TimeInForce::IOC, cap_.taker_id, /*client_id=*/0);

        // Cancel the ladder remnants (consumed ids → no-op).
        for (std::int32_t k = 0; k < levels; ++k) book_.cancel(ladder_ids[k]);

        if (cap_.qty <= 0) return 0;
        const double vwap_ticks = static_cast<double>(cap_.notional_ticks) / cap_.qty;
        out_vwap_usd = ref_price + (vwap_ticks - MID_TICK) * TICK_USD;
        return static_cast<std::int32_t>(cap_.qty);
    }
};


// run_pipeline — a full end-to-end simulation, everything on one thread.
inline PipelineStats run_pipeline(int num_messages = 1000,
                                   bool use_strategy = false,
                                   bool use_router = false,
                                   uint64_t seed = 42,
                                   const HFTConfig* cfg = nullptr,
                                   int fill_latency_iters = 0,
                                   bool use_risk = false,
                                   bool use_book = false,
                                   bool use_mm = false) {
    PipelineStats stats = {};
    stats.fill_latency_iters = fill_latency_iters;

    // Parameter resolution: the config file wins over the hardcoded defaults.
    int    strat_window    = cfg ? cfg->strategy.window        : 20;
    double strat_threshold = cfg ? cfg->strategy.threshold_pct : 0.1;
    int    strat_size      = cfg ? cfg->strategy.order_size     : 100;
    int    oms_max_pos     = cfg ? cfg->oms.max_position        : 1000;
    double oms_max_val     = cfg ? cfg->oms.max_order_value     : 100000.0;

    // Symbol universe — config (if non-empty) overrides the default STOCKS.
    std::vector<StockInfo> sim_stocks;
    if (cfg && !cfg->simulator.stocks.empty()) {
        sim_stocks.reserve(cfg->simulator.stocks.size());
        for (const auto& sc : cfg->simulator.stocks) {
            StockInfo si{};
            std::strncpy(si.symbol, sc.symbol, 8);
            si.symbol[8]    = '\0';
            si.base_price   = sc.base_price;
            sim_stocks.push_back(si);
        }
    }
    const StockInfo* active_stocks = sim_stocks.empty() ? STOCKS              : sim_stocks.data();
    const int        n_active      = sim_stocks.empty() ? NUM_STOCKS          : static_cast<int>(sim_stocks.size());

    // Component initialization
    MarketDataGenerator generator(seed);
    generator.set_stocks(active_stocks, n_active);
    ITCHParser parser;
    OMS oms(oms_max_pos, oms_max_val);
    MeanReversionStrategy strategy(strat_window, strat_threshold, strat_size);

    // Router strategy from the config
    RoutingStrategy rr = RoutingStrategy::BEST_PRICE;
    int split_thr = 500;
    if (cfg) {
        if (std::strcmp(cfg->router.default_strategy, "LOWEST_LATENCY") == 0)
            rr = RoutingStrategy::LOWEST_LATENCY;
        else if (std::strcmp(cfg->router.default_strategy, "SPLIT") == 0)
            rr = RoutingStrategy::SPLIT;
        split_thr = cfg->router.split_threshold;
    }
    SmartOrderRouter router(rr, split_thr);

    if (use_router) {
        // Prefer venues from the config, fall back to the hardcoded defaults.
        if (cfg && !cfg->router.venues.empty()) {
            for (const auto& vc : cfg->router.venues) {
                Venue v;
                std::strncpy(v.name, vc.name, 15);
                v.latency_ns    = vc.latency_ns;
                v.fee_per_share = vc.fee_per_share;
                router.add_venue(v);
            }
        } else {
            Venue nyse, nasdaq, bats;
            std::strncpy(nyse.name,    "NYSE",   15); nyse.latency_ns = 500;   nyse.fee_per_share = 0.003;
            std::strncpy(nasdaq.name,  "NASDAQ", 15); nasdaq.latency_ns = 350; nasdaq.fee_per_share = 0.002;
            std::strncpy(bats.name,    "BATS",   15); bats.latency_ns = 250;   bats.fee_per_share = 0.001;
            router.add_venue(nyse);
            router.add_venue(nasdaq);
            router.add_venue(bats);
        }
    }

    // RiskManager — the pre-trade gatekeeper between Strategy/Router and the OMS.
    // Limits from the config (cfg->risk), fall back to RiskLimits() defaults.
    // SEC 15c3-5: nothing reaches the OMS without a positive check_order().
    RiskLimits rlimits;
    if (cfg) {
        rlimits.max_position_per_symbol = cfg->risk.max_position_per_symbol;
        rlimits.max_portfolio_exposure  = cfg->risk.max_portfolio_exposure;
        rlimits.max_daily_loss          = static_cast<int64_t>(cfg->risk.max_daily_loss);
        rlimits.max_orders_per_second   = cfg->risk.max_orders_per_second;
        rlimits.max_order_value         = static_cast<int64_t>(cfg->risk.max_order_value);
        rlimits.max_drawdown_pct        = cfg->risk.max_drawdown_pct;
        rlimits.max_price_band_pct      = cfg->risk.max_price_band_pct;
        rlimits.max_shares_per_order    = cfg->risk.max_shares_per_order;
        rlimits.max_short_per_symbol    = cfg->risk.max_short_per_symbol;
        rlimits.max_consecutive_losses  = cfg->risk.max_consecutive_losses;
        rlimits.max_daily_traded_notional = cfg->risk.max_daily_traded_notional;
    }
    // The rate limiter measures orders/sec by the wall clock (mono_ns). The simulator
    // compresses time — all orders fly in a fraction of a second, so a real
    // N/s threshold would reject them en masse as a benchmark artifact, not a risk one. For
    // in-process simulation we relax ONLY this one check; the other 6
    // (kill switch, value, per-symbol position, portfolio exposure, circuit
    // breaker, drawdown) work fully and are the interesting ones here.
    rlimits.max_orders_per_second = (num_messages > 0) ? (num_messages + 16) : 1024;
    RiskManager risk(rlimits);

    // Matching engine — real matching of our orders in FullOrderBook
    // (opt-in --book). 200 shares/ladder level = moderate slippage.
    // On the heap: FullOrderBook<16384,65536> is an MB-sized object — on the stack
    // it would overflow. Created only when use_book.
    std::unique_ptr<BookMatchEngine> book_engine;
    if (use_book) book_engine = std::make_unique<BookMatchEngine>(/*level_liq=*/200);

    // MarketMaker mode (--mm): one MM per symbol. MarketMaker is non-movable
    // (deleted copy/move), so we keep it in a std::deque (emplace_back doesn't move
    // existing elements) + a sym_key→index map + a parallel last_mid.
    mm::MMConfig mm_cfg;
    mm_cfg.quote_size          = 10;
    mm_cfg.half_spread_ticks   = 2;
    mm_cfg.max_inventory       = 200;
    mm_cfg.risk_aversion_ticks = 0.05;
    std::deque<mm::MarketMaker>          makers;
    std::vector<int32_t>                 maker_last_mid;
    std::unordered_map<uint64_t, size_t> maker_idx;

    // [1/4] Market data generation
    auto gen_start = std::chrono::high_resolution_clock::now();

    // Pre-allocate the message buffer
    int total_msgs = num_messages + 6;  // +6 for system events (3 start + 3 end)
    std::vector<GeneratedMessage> messages(total_msgs);
    int msg_idx = 0;

    // Start-of-day system events
    messages[msg_idx++] = generator.generate_system_event('O');
    messages[msg_idx++] = generator.generate_system_event('S');
    messages[msg_idx++] = generator.generate_system_event('Q');

    for (int i = 0; i < num_messages; ++i) {
        messages[msg_idx++] = generator.generate_random_message();
    }

    // End-of-day system events
    messages[msg_idx++] = generator.generate_system_event('M');
    messages[msg_idx++] = generator.generate_system_event('E');
    messages[msg_idx++] = generator.generate_system_event('C');

    auto gen_end = std::chrono::high_resolution_clock::now();
    stats.gen_ms = std::chrono::duration<double, std::milli>(gen_end - gen_start).count();
    stats.messages_generated = msg_idx;

    // [2/4] Parse all messages
    auto parse_start = std::chrono::high_resolution_clock::now();

    std::vector<ParsedMessage> parsed(msg_idx);
    for (int i = 0; i < msg_idx; ++i) {
        parsed[i] = parser.parse(messages[i].data, messages[i].length);
        switch (parsed[i].type) {
            case MsgType::ADD_ORDER:
            case MsgType::ADD_ORDER_MPID: stats.add_orders++; break;
            case MsgType::ORDER_EXECUTED:  stats.executes++; break;
            case MsgType::TRADE:           stats.trades++; break;
            case MsgType::ORDER_CANCELLED: stats.cancels++; break;
            case MsgType::SYSTEM_EVENT:    stats.system_events++; break;
            default: break;
        }
    }

    auto parse_end = std::chrono::high_resolution_clock::now();
    stats.parse_ms = std::chrono::duration<double, std::milli>(parse_end - parse_start).count();
    stats.messages_parsed = msg_idx;

    // [3/4] Routing through the OMS
    auto oms_start = std::chrono::high_resolution_clock::now();

    // FIFO queue of in-flight fills; size() = number of pending orders in the OMS.
    std::vector<PendingFill> pending_fills;

    // do_fill — one fill-settlement point (3 places call the same thing):
    // the OMS books the position/P&L, and when use_risk also the RiskManager (the
    // pending→realized flow + feeding realized P&L to the circuit breaker/drawdown).
    // We compute the P&L delta from the OMS Position before/after the fill (realized_pnl is
    // fixed-point int64, so via to_float to dollars for risk.update_pnl).
    auto do_fill = [&](uint64_t oid, int32_t sh, double px) {
        char    sym[9]   = {0};
        Side    fside    = Side::BUY;
        int64_t pnl_pre  = 0;
        const Order* o = oms.get_order(oid);
        if (o) {
            std::memcpy(sym, o->symbol, 9);
            fside = o->side;
            const Position* p = oms.get_position(sym);
            pnl_pre = p ? p->realized_pnl : 0;
        }
        const uint32_t applied = oms.fill_order(oid, sh, px);
        stats.orders_filled++;
        if (use_risk && o && applied > 0) {
            risk.update_position(sym, fside, static_cast<int32_t>(applied));
            const Position* p2 = oms.get_position(sym);
            const int64_t pnl_post = p2 ? p2->realized_pnl : 0;
            if (pnl_post != pnl_pre) risk.update_pnl(to_float(pnl_post - pnl_pre));
            if (risk.is_kill_switch_active()) stats.risk_kill_tripped = true;
        }
    };

    for (int i = 0; i < msg_idx; ++i) {
        // Drain fills whose ack deadline has passed.
        size_t drained = 0;
        for (; drained < pending_fills.size() && pending_fills[drained].due_iter <= i; ++drained) {
            const PendingFill& pf = pending_fills[drained];
            do_fill(pf.order_id, pf.shares, pf.price);
        }
        if (drained > 0) pending_fills.erase(pending_fills.begin(), pending_fills.begin() + drained);

        const ParsedMessage& pm = parsed[i];
        const char* stock = nullptr;
        double price = 0.0;
        const char* side_str = nullptr;
        int32_t shares = 0;

        if (pm.type == MsgType::ADD_ORDER) {
            stock = pm.data.add_order.stock;
            price = pm.data.add_order.price;
            side_str = pm.data.add_order.side == 'B' ? "BUY" : "SELL";
            shares = pm.data.add_order.shares;
        } else if (pm.type == MsgType::TRADE) {
            stock = pm.data.trade.stock;
            price = pm.data.trade.price;
            side_str = pm.data.trade.side == 'B' ? "BUY" : "SELL";
            shares = pm.data.trade.shares;
        } else {
            continue;  // skip non-tradable messages
        }

        if (!stock || price <= 0.0) continue;

        // Feed the Risk reference price from market data (price-band / fat-finger).
        if (use_risk) risk.update_reference_price(stock, price);

        // === MarketMaker mode (--mm) ===
        // The MM is a maker: it quotes both sides around the mid, and incoming market
        // flow (this message = the aggressor) crosses its quote. The MM fill goes
        // through Risk→OMS, so the OMS tracks the position and realized P&L, and Risk gates.
        if (use_mm) {
            const uint64_t sym_key = sym_to_key(stock);
            auto it = maker_idx.find(sym_key);
            if (it == maker_idx.end()) {
                it = maker_idx.emplace(sym_key, makers.size()).first;
                makers.emplace_back(mm_cfg, stock);
                maker_last_mid.push_back(0);
            }
            mm::MarketMaker& maker = makers[it->second];

            // Mid in ticks ($0.01); a reference spread of 1 tick around the mid.
            const int32_t mid_ticks = static_cast<int32_t>(price * 100.0 + 0.5);
            maker_last_mid[it->second] = mid_ticks;
            const mm::Quote q = maker.quote(mid_ticks - 1, mid_ticks + 1);

            // Aggressor: a BUY market order lifts our ASK (the MM sold),
            // a SELL hits our BID (the MM bought). side_str from the message = the aggressor's side.
            const bool aggressor_buy = (side_str[0] == 'B');
            Side    mm_side = Side::BUY;
            int32_t mm_qty  = 0;
            int32_t mm_px_ticks = 0;
            if (aggressor_buy && q.ask_size > 0) {
                mm_side = Side::SELL; mm_qty = q.ask_size; mm_px_ticks = q.ask_price;
            } else if (!aggressor_buy && q.bid_size > 0) {
                mm_side = Side::BUY;  mm_qty = q.bid_size; mm_px_ticks = q.bid_price;
            }

            if (mm_qty > 0 && mm_px_ticks > 0) {
                const double mm_px = mm_px_ticks / 100.0;
                bool allowed = true;
                if (use_risk) {
                    const RiskCheckResult rc = risk.check_order(stock, mm_side, mm_px, mm_qty);
                    allowed = (rc.action == RiskAction::ALLOW);
                    if (!allowed) {
                        stats.orders_risk_rejected++;
                        if (risk.is_kill_switch_active()) stats.risk_kill_tripped = true;
                    }
                }
                if (allowed) {
                    Order* order = oms.submit_order(stock, mm_side, mm_px, mm_qty);
                    if (order) {
                        stats.orders_submitted++;
                        if (use_risk) risk.on_order_sent(stock, mm_side, mm_qty);
                        maker.apply_fill(mm_side, mm_qty, mm_px_ticks);
                        stats.mm_fills++;
                        do_fill(order->order_id, mm_qty, mm_px);
                    } else {
                        stats.orders_rejected++;
                    }
                }
            }
            continue;  // the MM is the strategy — skip the mean-reversion/router path
        }

        // Update the router quotes
        if (use_router) {
            double spread = price * 0.0002;
            router.update_quote("NYSE",   price - spread, price + spread, 300, 300);
            router.update_quote("NASDAQ", price - spread, price + spread, 300, 300);
            router.update_quote("BATS",   price - spread, price + spread, 300, 300);
        }

        double fill_price = price;
        Side   oms_side   = side_from_str(side_str);

        if (use_strategy) {
            // Strategy mode: feed the price, trade only on signals.
            // Signal::side is a Side enum — there's no more dangling-pointer aliasing
            // risk (it used to be a char[5] inside a local Signal).
            Signal signal = strategy.on_market_data(stock, price, 0);
            if (!signal.valid) continue;
            oms_side   = signal.side;
            shares     = signal.quantity;
            fill_price = signal.price;

            if (use_router) {
                // ::side_str disambiguates the global function from the local
                // `const char* side_str` declared above in the loop.
                RouteDecision route = router.route_order(::side_str(signal.side), shares);
                if (route.valid) fill_price = route.price;
            }
        } else if (use_router) {
            RouteDecision route = router.route_order(side_str, shares);
            if (route.valid) fill_price = route.price;
        }

        // Pre-trade risk: the gatekeeper BEFORE the OMS. REJECT/KILL → the order does not go.
        if (use_risk) {
            const RiskCheckResult rc = risk.check_order(stock, oms_side, fill_price, shares);
            if (rc.action != RiskAction::ALLOW) {
                stats.orders_risk_rejected++;
                if (risk.is_kill_switch_active()) stats.risk_kill_tripped = true;
                continue;
            }
        }

        Order* order = oms.submit_order(stock, oms_side, fill_price, shares);
        if (order) {
            stats.orders_submitted++;
            if (use_risk) risk.on_order_sent(stock, oms_side, shares);  // reserve pending

            // Real match in FullOrderBook (opt-in) — determines the actual quantity
            // and VWAP. Liquidity IS assessed now; settlement in the OMS may be
            // deferred (fill latency). Without --book: a legacy fill at the submit price.
            int32_t fill_sh = shares;
            double  fill_px = fill_price;
            if (use_book) {
                double vwap = fill_price;
                const int32_t matched = book_engine->match(oms_side, fill_price, shares, vwap);
                fill_sh = matched;
                fill_px = vwap;
                if (matched < shares) stats.orders_book_partial++;
                if (matched > 0) {
                    stats.book_slippage_usd +=
                        (oms_side == Side::BUY ? (vwap - fill_price)
                                               : (fill_price - vwap)) * matched;
                }
            }

            if (fill_sh <= 0) {
                // No liquidity — the order stays pending in OMS/Risk (realistic).
            } else if (fill_latency_iters <= 0) {
                do_fill(order->order_id, fill_sh, fill_px);
            } else {
                pending_fills.push_back({order->order_id, fill_sh, fill_px, i + fill_latency_iters});
                int in_flight = static_cast<int>(pending_fills.size());
                if (in_flight > stats.max_in_flight_orders) stats.max_in_flight_orders = in_flight;
            }
        } else {
            stats.orders_rejected++;
        }
    }

    // Final drain — settle orders still in-flight at the end of the stream.
    for (const auto& pf : pending_fills) {
        do_fill(pf.order_id, pf.shares, pf.price);
    }
    pending_fills.clear();

    auto oms_end = std::chrono::high_resolution_clock::now();
    stats.oms_ms = std::chrono::duration<double, std::milli>(oms_end - oms_start).count();
    stats.total_ms = stats.gen_ms + stats.parse_ms + stats.oms_ms;

    // Total P&L — we go over the positions of all symbols.
    // realized_pnl is fixed-point int64 (× PRICE_SCALE) in the OMS.
    stats.total_pnl = 0.0;
    for (int i = 0; i < n_active; ++i) {
        const Position* pos = oms.get_position(active_stocks[i].symbol);
        if (pos) {
            stats.total_pnl += to_float(pos->realized_pnl);
        }
    }

    // MM aggregation across all symbols (mark-to-market at the last mid).
    if (use_mm) {
        for (size_t m = 0; m < makers.size(); ++m) {
            stats.mm_quotes_placed += makers[m].quotes_placed();
            stats.mm_pnl           += makers[m].pnl(maker_last_mid[m]);
            stats.mm_abs_inventory += std::abs(makers[m].position());
        }
    }

    return stats;
}


// run_pipeline_threaded — the generator on the producer thread, the parser on the
// consumer thread, hand-off via lockfree::SPSCQueue.
//
// This is the simplest realistic use of our lock-free primitive: the feed handler
// and parser run on different cores, the queue is the only thing connecting them.
// Zero mutex, zero allocation on the hot path, the parser thread doesn't even need
// to know what protocol the producer generates — it just drains bytes.

inline PipelineStats run_pipeline_threaded(int num_messages = 1000,
                                            uint64_t seed = 42) {
    PipelineStats stats = {};

    // SIZE = 1024: each GeneratedMessage is ~68 bytes, ~70 KB total —
    // small enough and allocated once per call (unique_ptr → heap, alignas preserved).
    auto queue = std::make_unique<lockfree::SPSCQueue<GeneratedMessage, 1024>>();
    std::atomic<bool> producer_done{false};

    auto t0 = std::chrono::high_resolution_clock::now();

    std::thread producer([&]() {
        MarketDataGenerator gen(seed);
        for (int i = 0; i < num_messages; ++i) {
            GeneratedMessage msg = gen.generate_random_message();
            while (!queue->push(msg)) { /* busy-spin when the consumer falls behind */ }
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::thread consumer([&]() {
        ITCHParser parser;
        GeneratedMessage msg{};
        while (!producer_done.load(std::memory_order_acquire) || !queue->empty()) {
            if (queue->pop(msg)) {
                const ParsedMessage pm = parser.parse(msg.data, msg.length);
                stats.messages_generated++;
                stats.messages_parsed++;
                switch (pm.type) {
                    case MsgType::ADD_ORDER:
                    case MsgType::ADD_ORDER_MPID: stats.add_orders++; break;
                    case MsgType::ORDER_EXECUTED:  stats.executes++; break;
                    case MsgType::TRADE:           stats.trades++; break;
                    case MsgType::ORDER_CANCELLED: stats.cancels++; break;
                    case MsgType::SYSTEM_EVENT:    stats.system_events++; break;
                    default: break;
                }
            }
        }
    });

    producer.join();
    consumer.join();

    auto t1 = std::chrono::high_resolution_clock::now();
    stats.total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    return stats;
}


// print_pipeline_stats — displays the simulation results.
inline void print_pipeline_stats(const PipelineStats& s, bool use_strategy, bool use_router,
                                 bool use_risk = false, bool use_book = false,
                                 bool use_mm = false) {
    printf("\n=== HFT Market Data Simulator (C++) ===\n");
    printf("Pipeline: ITCH Generator -> Parser");
    if (use_mm) printf(" -> MarketMaker");
    if (use_strategy && !use_mm) printf(" -> Strategy");
    if (use_router && !use_mm) printf(" -> Router");
    if (use_risk) printf(" -> Risk");
    printf(" -> OMS");
    if (use_book) printf(" -> FullOrderBook(match)");
    printf(" -> P&L\n\n");

    printf("[1/4] Generation\n");
    printf("  Messages: %d\n", s.messages_generated);
    printf("  Time:     %.1f ms\n", s.gen_ms);
    printf("  Speed:    %.0f msg/sec\n\n", s.messages_generated / (s.gen_ms / 1000.0));

    printf("[2/4] Parsing\n");
    printf("  Parsed: %d messages in %.1f ms\n", s.messages_parsed, s.parse_ms);
    printf("  Throughput: %.0f msg/sec\n", s.messages_parsed / (s.parse_ms / 1000.0));
    printf("  Breakdown:\n");
    printf("    ADD_ORDER:      %d\n", s.add_orders);
    printf("    ORDER_EXECUTED: %d\n", s.executes);
    printf("    TRADE:          %d\n", s.trades);
    printf("    ORDER_CANCEL:   %d\n", s.cancels);
    printf("    SYSTEM_EVENT:   %d\n\n", s.system_events);

    printf("[3/4] OMS Processing\n");
    printf("  Submitted: %d\n", s.orders_submitted);
    printf("  Filled:    %d\n", s.orders_filled);
    printf("  Rejected (OMS):  %d\n", s.orders_rejected);
    if (use_risk) {
        printf("  Rejected (Risk pre-trade): %d\n", s.orders_risk_rejected);
        printf("  Kill switch: %s\n", s.risk_kill_tripped ? "TRIPPED" : "inactive");
    }
    if (use_book) {
        printf("  Book partial fills: %d\n", s.orders_book_partial);
        printf("  Book slippage cost: $%.2f\n", s.book_slippage_usd);
    }
    if (use_mm) {
        printf("  MM quotes placed:  %lu\n", (unsigned long)s.mm_quotes_placed);
        printf("  MM fills:          %lu\n", (unsigned long)s.mm_fills);
        printf("  MM abs inventory:  %d shares\n", s.mm_abs_inventory);
        printf("  MM P&L (mark):     $%.2f\n", s.mm_pnl);
    }
    if (s.fill_latency_iters > 0) {
        printf("  Max in-flight: %d  (fill latency: %d iters)\n",
               s.max_in_flight_orders, s.fill_latency_iters);
    }
    if (s.oms_ms > 0)
        printf("  OMS throughput: %.0f orders/sec\n\n", s.orders_submitted / (s.oms_ms / 1000.0));

    printf("[4/4] Pipeline Summary\n");
    printf("  Total time:     %.1f ms\n", s.total_ms);
    printf("  E2E throughput: %.0f msg/sec\n", s.messages_generated / (s.total_ms / 1000.0));
    printf("  Total P&L:      $%.2f\n", s.total_pnl);
}
