/*
 * HFT Market Data Simulator — C++ Implementation
 * Symulator Danych Rynkowych HFT — implementacja C++
 *
 * Generates synthetic ITCH market data, parses through ITCH parser,
 * routes orders through OMS with risk checks, and tracks P&L.
 *
 * Pipeline: ITCH Generator → ITCH Parser → Strategy → Router → OMS → P&L
 * Potok: Generator ITCH → Parser ITCH → Strategia → Router → OMS → P&L
 *
 * Performance / Wydajność:
 *   Python: ~50-100K msg/sec end-to-end
 *   C++:    target 1-5M msg/sec end-to-end
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <chrono>
#include <algorithm>
#include <vector>

// Include all pipeline modules
#include "../itch-parser/itch_parser.hpp"
#include "../oms/oms.hpp"
#include "../config/config_loader.hpp"
#include "../strategy/mean_reversion.hpp"
#include "../router/smart_router.hpp"


// ============================================================
// MarketDataGenerator — generates synthetic ITCH binary messages
// Generator danych rynkowych — generuje syntetyczne wiadomości binarne ITCH
// ============================================================

// Simple LCG random number generator (deterministic, no heap allocation)
// Prosty generator liczb losowych LCG (deterministyczny, bez alokacji na stercie)
struct FastRNG {
    uint64_t state;

    explicit FastRNG(uint64_t seed = 42) noexcept : state(seed) {}

    uint64_t next() noexcept {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return state >> 16;
    }

    // Random int in [0, max)
    int rand_int(int max) noexcept {
        return static_cast<int>(next() % max);
    }

    // Random double in [0.0, 1.0)
    double rand_double() noexcept {
        return static_cast<double>(next() & 0xFFFFFFFF) / 4294967296.0;
    }

    // Random double in [lo, hi)
    double rand_range(double lo, double hi) noexcept {
        return lo + rand_double() * (hi - lo);
    }
};


// Stock info for the generator
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


// Active order tracking (fixed-size, no heap on hot path)
struct ActiveOrder {
    int64_t order_ref;
    char    stock[9];
    char    side;       // 'B' or 'S'
    double  price;
    int32_t shares;
    bool    active;
};

static constexpr int MAX_ACTIVE_ORDERS = 8192;


// Pre-built raw ITCH message buffer (max message size)
static constexpr int MAX_MSG_SIZE = 64;

// A generated message with its raw bytes and length
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

    // Pack big-endian helpers (ITCH uses network byte order)
    // Pomocniki pakowania big-endian (ITCH używa sieciowej kolejności bajtów)
    static void pack_be64(uint8_t* buf, int64_t val) noexcept {
        for (int i = 7; i >= 0; --i) { buf[i] = val & 0xFF; val >>= 8; }
    }
    static void pack_be32(uint8_t* buf, int32_t val) noexcept {
        for (int i = 3; i >= 0; --i) { buf[i] = val & 0xFF; val >>= 8; }
    }

    int64_t next_ts() noexcept {
        seq_++;
        return 34200000000000LL + seq_ * 1000000LL;  // 9:30 AM + seq ms
    }

    // Find a random active order, return index or -1
    int random_active_order() noexcept {
        if (active_count_ == 0) return -1;
        // Collect active indices
        int indices[MAX_ACTIVE_ORDERS];
        int n = 0;
        for (int i = 0; i < MAX_ACTIVE_ORDERS && n < active_count_; ++i) {
            if (active_orders_[i].active) indices[n++] = i;
        }
        if (n == 0) return -1;
        return indices[rng_.rand_int(n)];
    }

public:
    explicit MarketDataGenerator(uint64_t seed = 42) noexcept
        : rng_(seed), seq_(0), order_ref_(1000), active_count_(0) {
        for (int i = 0; i < MAX_ACTIVE_ORDERS; ++i)
            active_orders_[i].active = false;
    }

    int active_order_count() const noexcept { return active_count_; }

    // Generate Add Order (A) message
    GeneratedMessage generate_add_order() noexcept {
        GeneratedMessage msg = {};
        const StockInfo& stock = STOCKS[rng_.rand_int(NUM_STOCKS)];
        double price = stock.base_price + rng_.rand_range(-2.0, 2.0);
        price = std::round(price * 100.0) / 100.0;
        char side = rng_.rand_int(2) ? 'S' : 'B';
        int shares = SHARE_SIZES[rng_.rand_int(NUM_SHARE_SIZES)];
        order_ref_++;

        // Store active order
        for (int i = 0; i < MAX_ACTIVE_ORDERS; ++i) {
            if (!active_orders_[i].active) {
                active_orders_[i].order_ref = order_ref_;
                std::strncpy(active_orders_[i].stock, stock.symbol, 8);
                active_orders_[i].stock[8] = '\0';
                active_orders_[i].side = side;
                active_orders_[i].price = price;
                active_orders_[i].shares = shares;
                active_orders_[i].active = true;
                active_count_++;
                break;
            }
        }

        // Pack: 'A' + timestamp(8) + order_ref(8) + side(1) + shares(4) + stock(8) + price(4)
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

    // Generate Order Executed (E) message
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

    // Generate Order Cancelled (C) message
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

    // Generate Trade (P) message
    GeneratedMessage generate_trade() noexcept {
        const StockInfo& stock = STOCKS[rng_.rand_int(NUM_STOCKS)];
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

    // Generate a mixed message based on probability distribution
    GeneratedMessage generate_random_message() noexcept {
        double roll = rng_.rand_double();
        if (roll < 0.45)       return generate_add_order();
        else if (roll < 0.70)  return generate_execute();
        else if (roll < 0.85)  return generate_trade();
        else                   return generate_cancel();
    }
};


// ============================================================
// PipelineStats — results from a simulation run
// Statystyki potoku — wyniki z uruchomienia symulacji
// ============================================================

struct PipelineStats {
    int     messages_generated;
    int     messages_parsed;
    int     orders_submitted;
    int     orders_filled;
    int     orders_rejected;
    double  gen_ms;
    double  parse_ms;
    double  oms_ms;
    double  total_ms;
    double  total_pnl;

    // Message type breakdown
    int add_orders;
    int executes;
    int trades;
    int cancels;
    int system_events;
};


// ============================================================
// run_pipeline — full end-to-end simulation
// Pełna symulacja od końca do końca
// ============================================================

inline PipelineStats run_pipeline(int num_messages = 1000,
                                   bool use_strategy = false,
                                   bool use_router = false,
                                   uint64_t seed = 42,
                                   const HFTConfig* cfg = nullptr) {
    PipelineStats stats = {};

    // Resolve parameters: config file wins over hardcoded defaults
    int    strat_window    = cfg ? cfg->strategy.window        : 20;
    double strat_threshold = cfg ? cfg->strategy.threshold_pct : 0.1;
    int    strat_size      = cfg ? cfg->strategy.order_size     : 100;
    int    oms_max_pos     = cfg ? cfg->oms.max_position        : 1000;
    double oms_max_val     = cfg ? cfg->oms.max_order_value     : 100000.0;

    // Initialize components
    MarketDataGenerator generator(seed);
    ITCHParser parser;
    OMS oms(oms_max_pos, oms_max_val);
    MeanReversionStrategy strategy(strat_window, strat_threshold, strat_size);

    // Router strategy from config
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
        // Prefer venues from config; fall back to hardcoded defaults
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

    // [1/4] Generate market data
    auto gen_start = std::chrono::high_resolution_clock::now();

    // Pre-allocate message buffer
    int total_msgs = num_messages + 6;  // +6 for system events
    std::vector<GeneratedMessage> messages(total_msgs);
    int msg_idx = 0;

    // Start of day system events
    messages[msg_idx++] = generator.generate_system_event('O');
    messages[msg_idx++] = generator.generate_system_event('S');
    messages[msg_idx++] = generator.generate_system_event('Q');

    for (int i = 0; i < num_messages; ++i) {
        messages[msg_idx++] = generator.generate_random_message();
    }

    // End of day system events
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

    // [3/4] Route through OMS
    auto oms_start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < msg_idx; ++i) {
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
            continue;  // skip non-tradeable messages
        }

        if (!stock || price <= 0.0) continue;

        // Update router quotes
        if (use_router) {
            double spread = price * 0.0002;
            router.update_quote("NYSE",   price - spread, price + spread, 300, 300);
            router.update_quote("NASDAQ", price - spread, price + spread, 300, 300);
            router.update_quote("BATS",   price - spread, price + spread, 300, 300);
        }

        double fill_price = price;

        if (use_strategy) {
            // Strategy mode: feed price, trade only on signals
            Signal signal = strategy.on_market_data(stock, price, 0);
            if (!signal.valid) continue;
            side_str = signal.side;
            shares = signal.quantity;
            fill_price = signal.price;

            if (use_router) {
                RouteDecision route = router.route_order(side_str, shares);
                if (route.valid) fill_price = route.price;
            }
        } else {
            // Direct mode
            if (use_router) {
                RouteDecision route = router.route_order(side_str, shares);
                if (route.valid) fill_price = route.price;
            }
        }

        Side oms_side = (std::strcmp(side_str, "BUY") == 0) ? Side::BUY : Side::SELL;
        Order* order = oms.submit_order(stock, oms_side, fill_price, shares);
        if (order) {
            stats.orders_submitted++;
            oms.fill_order(order->order_id, shares, fill_price);
            stats.orders_filled++;
        } else {
            stats.orders_rejected++;
        }
    }

    auto oms_end = std::chrono::high_resolution_clock::now();
    stats.oms_ms = std::chrono::duration<double, std::milli>(oms_end - oms_start).count();
    stats.total_ms = stats.gen_ms + stats.parse_ms + stats.oms_ms;

    // Calculate total P&L by checking positions for all traded stocks
    // realized_pnl is stored as fixed-point int64 (× PRICE_SCALE) in OMS
    stats.total_pnl = 0.0;
    for (int i = 0; i < NUM_STOCKS; ++i) {
        const Position* pos = oms.get_position(STOCKS[i].symbol);
        if (pos) {
            stats.total_pnl += to_float(pos->realized_pnl);
        }
    }

    return stats;
}


// ============================================================
// print_pipeline_stats — display simulation results
// Wyświetl wyniki symulacji
// ============================================================

inline void print_pipeline_stats(const PipelineStats& s, bool use_strategy, bool use_router) {
    printf("\n=== HFT Market Data Simulator (C++) ===\n");
    printf("Pipeline: ITCH Generator -> Parser");
    if (use_strategy) printf(" -> Strategy");
    if (use_router) printf(" -> Router");
    printf(" -> OMS -> P&L\n\n");

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
    printf("  Rejected:  %d\n", s.orders_rejected);
    if (s.oms_ms > 0)
        printf("  OMS throughput: %.0f orders/sec\n\n", s.orders_submitted / (s.oms_ms / 1000.0));

    printf("[4/4] Pipeline Summary\n");
    printf("  Total time:     %.1f ms\n", s.total_ms);
    printf("  E2E throughput: %.0f msg/sec\n", s.messages_generated / (s.total_ms / 1000.0));
    printf("  Total P&L:      $%.2f\n", s.total_pnl);
}
