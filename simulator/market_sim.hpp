/*
 * MarketSim — symulator danych rynkowych end-to-end pipeline'u HFT.
 *
 * Generuje syntetyczne wiadomości ITCH, parsuje przez ITCH parser, routuje
 * zlecenia przez OMS z risk checkami, śledzi P&L. To "klejenie" wszystkich
 * komponentów labu w jeden binarny demo — pokazuje że całość działa.
 *
 * Pipeline: ITCH Generator → ITCH Parser → Strategy → Router → Risk → OMS → P&L
 *
 * Dwa warianty:
 *   - run_pipeline()           — wszystko w jednym wątku, sekwencyjnie.
 *   - run_pipeline_threaded()  — gen / parse-strategy / oms na osobnych
 *                                wątkach przez SPSC queue (lockfree).
 *
 * Wydajność (lab): ~573K msg/sec end-to-end (full pipeline).
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

// Wszystkie moduły pipeline'u
#include "../itch-parser/itch_parser.hpp"
#include "../oms/oms.hpp"
#include "../config/config_loader.hpp"
#include "../strategy/mean_reversion.hpp"
#include "../router/smart_router.hpp"
#include "../lockfree/spsc_queue.hpp"


// MarketDataGenerator — generuje syntetyczne binarne wiadomości ITCH.

// Prosty LCG (deterministyczny, zero heap allocation — istotne w hot path).
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


// Informacje o symbolu dla generatora.
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


// Śledzenie aktywnych zleceń (stała tablica, zero heap na hot path).
struct ActiveOrder {
    int64_t order_ref;
    char    stock[9];
    char    side;       // 'B' lub 'S'
    double  price;
    int32_t shares;
    bool    active;
};

static constexpr int MAX_ACTIVE_ORDERS = 8192;

// Pre-built bufor surowej wiadomości ITCH (max rozmiar to ramka 'P' z match_num).
static constexpr int MAX_MSG_SIZE = 64;

// Wygenerowana wiadomość — surowe bajty + długość.
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
    // Scratch buffer dla random_active_order — pole klasy żeby każde wywołanie
    // nie wrzucało 32 KB na stos. Nadpisywany przy każdym użyciu, nigdy nie
    // czytany przed wypełnieniem, więc init zbędny.
    int active_indices_[MAX_ACTIVE_ORDERS];
    // Symbol universe — wskazuje na pre-built StockInfo. Domyślnie hardcoded
    // STOCKS[] fallback gdy config nie został dostarczony.
    const StockInfo* stocks_;
    int              n_stocks_;

    // ITCH używa network byte order (big-endian) — pakery 32/64 bit.
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

    // Wylosuj indeks aktywnego zlecenia, -1 gdy brak.
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

    // set_stocks: nadpisz symbol universe ticker'ami z configa.
    // Ownership wskaźnika zostaje po stronie wywołującego — zwykle wskazuje
    // na długożyjący StockInfo vector zbudowany z HFTConfig::simulator::stocks.
    void set_stocks(const StockInfo* arr, int n) noexcept {
        if (arr && n > 0) { stocks_ = arr; n_stocks_ = n; }
    }

    int active_order_count() const noexcept { return active_count_; }

    // Generuj wiadomość Add Order ('A').
    GeneratedMessage generate_add_order() noexcept {
        GeneratedMessage msg = {};
        const StockInfo& stock = stocks_[rng_.rand_int(n_stocks_)];
        double price = stock.base_price + rng_.rand_range(-2.0, 2.0);
        price = std::round(price * 100.0) / 100.0;
        char side = rng_.rand_int(2) ? 'S' : 'B';
        int shares = SHARE_SIZES[rng_.rand_int(NUM_SHARE_SIZES)];
        order_ref_++;

        // Zapisz aktywne zlecenie.
        for (int i = 0; i < MAX_ACTIVE_ORDERS; ++i) {
            if (!active_orders_[i].active) {
                active_orders_[i].order_ref = order_ref_;
                // memcpy zamiast strncpy — g++ -Wstringop-truncation flagują
                // strncpy(dst, src, 8) jako "może nie być null-terminated", mimo
                // że explicit stock[8] = '\0' linijkę niżej. memcpy daje identyczne
                // bajty i nie wywala warninga.
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

        // Pakowanie ramki: 'A' + timestamp(8) + order_ref(8) + side(1) + shares(4) + stock(8) + price(4)
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

    // Generuj wiadomość Order Executed ('E').
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

    // Generuj wiadomość Order Cancelled ('C').
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

    // Generuj wiadomość Trade ('P').
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

    // Wymieszane wiadomości wg rozkładu prawdopodobieństwa (45% A, 25% E, 15% P, 15% C).
    GeneratedMessage generate_random_message() noexcept {
        double roll = rng_.rand_double();
        if (roll < 0.45)       return generate_add_order();
        else if (roll < 0.70)  return generate_execute();
        else if (roll < 0.85)  return generate_trade();
        else                   return generate_cancel();
    }
};


// PipelineStats — wyniki uruchomienia symulacji.
struct PipelineStats {
    int     messages_generated;
    int     messages_parsed;
    int     orders_submitted;
    int     orders_filled;
    int     orders_rejected;
    int     max_in_flight_orders;   // peak submittted-but-unfilled
    int     fill_latency_iters;     // iteracje symulowanego opóźnienia fill-ack (0 = zero-latency)
    double  gen_ms;
    double  parse_ms;
    double  oms_ms;
    double  total_ms;
    double  total_pnl;

    // Rozbicie typów wiadomości
    int add_orders;
    int executes;
    int trades;
    int cancels;
    int system_events;
};


// Pending fill powiązany z wysłanym zleceniem — drainowany gdy current_iter osiągnie due_iter.
struct PendingFill {
    uint64_t order_id;
    int32_t  shares;
    double   price;
    int      due_iter;
};


// run_pipeline — pełna symulacja end-to-end, wszystko w jednym wątku.
inline PipelineStats run_pipeline(int num_messages = 1000,
                                   bool use_strategy = false,
                                   bool use_router = false,
                                   uint64_t seed = 42,
                                   const HFTConfig* cfg = nullptr,
                                   int fill_latency_iters = 0) {
    PipelineStats stats = {};
    stats.fill_latency_iters = fill_latency_iters;

    // Rozwiązywanie parametrów: config file wygrywa nad hardcoded defaults.
    int    strat_window    = cfg ? cfg->strategy.window        : 20;
    double strat_threshold = cfg ? cfg->strategy.threshold_pct : 0.1;
    int    strat_size      = cfg ? cfg->strategy.order_size     : 100;
    int    oms_max_pos     = cfg ? cfg->oms.max_position        : 1000;
    double oms_max_val     = cfg ? cfg->oms.max_order_value     : 100000.0;

    // Symbol universe — config (jeśli niepusty) override'uje domyślne STOCKS.
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

    // Inicjalizacja komponentów
    MarketDataGenerator generator(seed);
    generator.set_stocks(active_stocks, n_active);
    ITCHParser parser;
    OMS oms(oms_max_pos, oms_max_val);
    MeanReversionStrategy strategy(strat_window, strat_threshold, strat_size);

    // Strategia routera z configa
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
        // Preferuj giełdy z configa, fallback do hardcoded defaults.
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

    // [1/4] Generowanie danych rynkowych
    auto gen_start = std::chrono::high_resolution_clock::now();

    // Pre-alokacja bufora wiadomości
    int total_msgs = num_messages + 6;  // +6 na system events (3 start + 3 end)
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

    // [2/4] Parsowanie wszystkich wiadomości
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

    // [3/4] Routing przez OMS
    auto oms_start = std::chrono::high_resolution_clock::now();

    // Kolejka FIFO fillów w locie; size() = liczba pending zleceń w OMS.
    std::vector<PendingFill> pending_fills;

    for (int i = 0; i < msg_idx; ++i) {
        // Drain fillów których ack deadline już minął.
        size_t drained = 0;
        for (; drained < pending_fills.size() && pending_fills[drained].due_iter <= i; ++drained) {
            const PendingFill& pf = pending_fills[drained];
            oms.fill_order(pf.order_id, pf.shares, pf.price);
            stats.orders_filled++;
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
            continue;  // pomiń wiadomości nietradowalne
        }

        if (!stock || price <= 0.0) continue;

        // Aktualizuj quoty routera
        if (use_router) {
            double spread = price * 0.0002;
            router.update_quote("NYSE",   price - spread, price + spread, 300, 300);
            router.update_quote("NASDAQ", price - spread, price + spread, 300, 300);
            router.update_quote("BATS",   price - spread, price + spread, 300, 300);
        }

        double fill_price = price;
        Side   oms_side   = side_from_str(side_str);

        if (use_strategy) {
            // Tryb strategy: karm cenę, traduj tylko na sygnałach.
            // Signal::side jest enumem Side — nie ma już dangling-pointer aliasing
            // risk (wcześniej był char[5] wewnątrz lokalnego Signal).
            Signal signal = strategy.on_market_data(stock, price, 0);
            if (!signal.valid) continue;
            oms_side   = signal.side;
            shares     = signal.quantity;
            fill_price = signal.price;

            if (use_router) {
                // ::side_str disambiguates global function od lokalnego
                // `const char* side_str` zadeklarowanego wyżej w pętli.
                RouteDecision route = router.route_order(::side_str(signal.side), shares);
                if (route.valid) fill_price = route.price;
            }
        } else if (use_router) {
            RouteDecision route = router.route_order(side_str, shares);
            if (route.valid) fill_price = route.price;
        }

        Order* order = oms.submit_order(stock, oms_side, fill_price, shares);
        if (order) {
            stats.orders_submitted++;
            if (fill_latency_iters <= 0) {
                oms.fill_order(order->order_id, shares, fill_price);
                stats.orders_filled++;
            } else {
                pending_fills.push_back({order->order_id, shares, fill_price, i + fill_latency_iters});
                int in_flight = static_cast<int>(pending_fills.size());
                if (in_flight > stats.max_in_flight_orders) stats.max_in_flight_orders = in_flight;
            }
        } else {
            stats.orders_rejected++;
        }
    }

    // Final drain — rozlicz zlecenia jeszcze in-flight na końcu strumienia.
    for (const auto& pf : pending_fills) {
        oms.fill_order(pf.order_id, pf.shares, pf.price);
        stats.orders_filled++;
    }
    pending_fills.clear();

    auto oms_end = std::chrono::high_resolution_clock::now();
    stats.oms_ms = std::chrono::duration<double, std::milli>(oms_end - oms_start).count();
    stats.total_ms = stats.gen_ms + stats.parse_ms + stats.oms_ms;

    // Łączny P&L — przeglądamy pozycje wszystkich symboli.
    // realized_pnl jest fixed-point int64 (× PRICE_SCALE) w OMS.
    stats.total_pnl = 0.0;
    for (int i = 0; i < n_active; ++i) {
        const Position* pos = oms.get_position(active_stocks[i].symbol);
        if (pos) {
            stats.total_pnl += to_float(pos->realized_pnl);
        }
    }

    return stats;
}


// run_pipeline_threaded — generator na wątku producenta, parser na wątku
// konsumenta, hand-off przez lockfree::SPSCQueue.
//
// To najprostsze realistyczne użycie naszego prymitywu lock-free: feed-handler
// i parser działają na różnych rdzeniach, queue to jedyna rzecz która je łączy.
// Zero mutexa, zero alokacji na hot path, parser thread nawet nie musi
// wiedzieć jaki protokół generuje producent — po prostu drainuje bajty.

inline PipelineStats run_pipeline_threaded(int num_messages = 1000,
                                            uint64_t seed = 42) {
    PipelineStats stats = {};

    // SIZE = 1024: każda GeneratedMessage to ~68 bajtów, łącznie ~70 KB —
    // wystarczy małe i alokowane raz na call (unique_ptr → heap, alignas zachowany).
    auto queue = std::make_unique<lockfree::SPSCQueue<GeneratedMessage, 1024>>();
    std::atomic<bool> producer_done{false};

    auto t0 = std::chrono::high_resolution_clock::now();

    std::thread producer([&]() {
        MarketDataGenerator gen(seed);
        for (int i = 0; i < num_messages; ++i) {
            GeneratedMessage msg = gen.generate_random_message();
            while (!queue->push(msg)) { /* busy-spin gdy konsument zostaje w tyle */ }
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


// print_pipeline_stats — wyświetla wyniki symulacji.
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
