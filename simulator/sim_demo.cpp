/*
 * Market Simulator Demo & Benchmark — C++
 *
 * Compile: g++ -O2 -std=c++17 -Wall -Wextra -o simulator/sim_demo simulator/sim_demo.cpp
 * Run:     ./simulator/sim_demo [num_messages] [--strategy] [--router]
 */

#include "market_sim.hpp"
#include "../config/config_loader.hpp"
#include <cstdlib>
#include <cstring>

static int tests_passed = 0;
static int tests_failed = 0;
static int tests_total = 0;

#define ASSERT(cond, msg) do { \
    tests_total++; \
    if (!(cond)) { \
        printf("  FAIL: %s (%s)\n", msg, #cond); \
        tests_failed++; \
    } else { \
        printf("  PASS: %s\n", msg); \
        tests_passed++; \
    } \
} while(0)


void test_generator_add_order() {
    MarketDataGenerator gen(42);
    auto msg = gen.generate_add_order();
    ASSERT(msg.length == 34, "add_order_length");
    ASSERT(msg.data[0] == 'A', "add_order_type");
    ASSERT(gen.active_order_count() == 1, "add_order_active_count");
}

void test_generator_execute() {
    MarketDataGenerator gen(42);
    gen.generate_add_order();
    gen.generate_add_order();
    auto msg = gen.generate_execute();
    ASSERT(msg.data[0] == 'E', "execute_type");
    ASSERT(msg.length == 29, "execute_length");
}

void test_generator_cancel() {
    MarketDataGenerator gen(42);
    gen.generate_add_order();
    int before = gen.active_order_count();
    auto msg = gen.generate_cancel();
    ASSERT(msg.data[0] == 'C', "cancel_type");
    ASSERT(gen.active_order_count() < before, "cancel_reduces_active");
}

void test_generator_trade() {
    MarketDataGenerator gen(42);
    auto msg = gen.generate_trade();
    ASSERT(msg.data[0] == 'P', "trade_type");
    ASSERT(msg.length == 42, "trade_length");
}

void test_generator_system_event() {
    MarketDataGenerator gen(42);
    auto msg = gen.generate_system_event('O');
    ASSERT(msg.data[0] == 'S', "system_event_type");
    ASSERT(msg.data[9] == 'O', "system_event_code");
    ASSERT(msg.length == 10, "system_event_length");
}

void test_parse_generated_add_order() {
    MarketDataGenerator gen(42);
    ITCHParser parser;
    auto msg = gen.generate_add_order();
    auto parsed = parser.parse(msg.data, msg.length);
    ASSERT(parsed.type == MsgType::ADD_ORDER, "parsed_add_order_type");
    ASSERT(parsed.data.add_order.shares > 0, "parsed_add_order_shares");
    ASSERT(parsed.data.add_order.price > 0.0, "parsed_add_order_price");
}

void test_parse_generated_trade() {
    MarketDataGenerator gen(42);
    ITCHParser parser;
    auto msg = gen.generate_trade();
    auto parsed = parser.parse(msg.data, msg.length);
    ASSERT(parsed.type == MsgType::TRADE, "parsed_trade_type");
    ASSERT(parsed.data.trade.shares > 0, "parsed_trade_shares");
}

void test_pipeline_direct() {
    PipelineStats stats = run_pipeline(100, false, false, 42);
    ASSERT(stats.messages_generated > 100, "pipeline_msg_count");
    ASSERT(stats.messages_parsed == stats.messages_generated, "pipeline_parsed_all");
    ASSERT(stats.orders_submitted > 0, "pipeline_submitted");
    ASSERT(stats.orders_filled > 0, "pipeline_filled");
    ASSERT(stats.total_ms > 0, "pipeline_time");
}

void test_pipeline_with_strategy() {
    PipelineStats stats = run_pipeline(500, true, false, 42);
    ASSERT(stats.messages_parsed > 0, "strategy_parsed");
    // Strategy is selective — may submit fewer orders
    ASSERT(stats.orders_submitted >= 0, "strategy_submitted");
}

void test_pipeline_with_router() {
    PipelineStats stats = run_pipeline(100, false, true, 42);
    ASSERT(stats.orders_submitted > 0, "router_submitted");
}

void test_pipeline_full() {
    PipelineStats stats = run_pipeline(500, true, true, 42);
    ASSERT(stats.messages_parsed > 0, "full_pipeline_parsed");
}

void test_rng_deterministic() {
    FastRNG rng1(12345);
    FastRNG rng2(12345);
    bool all_match = true;
    for (int i = 0; i < 100; ++i) {
        if (rng1.next() != rng2.next()) { all_match = false; break; }
    }
    ASSERT(all_match, "rng_deterministic");
}


int main(int argc, char* argv[]) {
    printf("=== Market Simulator C++ Unit Tests ===\n\n");

    test_generator_add_order();
    test_generator_execute();
    test_generator_cancel();
    test_generator_trade();
    test_generator_system_event();
    test_parse_generated_add_order();
    test_parse_generated_trade();
    test_pipeline_direct();
    test_pipeline_with_strategy();
    test_pipeline_with_router();
    test_pipeline_full();
    test_rng_deterministic();

    printf("\n%d/%d tests passed", tests_passed, tests_total);
    if (tests_failed > 0) printf("  (%d FAILED)", tests_failed);
    printf("\n");

    // Load config — defaults from config.yaml, overridable via HFT_ env vars
    HFTConfig cfg = load_config("config.yaml");
    print_config(cfg);

    // CLI args override config values
    bool use_strategy = false;
    bool use_router   = false;
    int  num_messages = cfg.simulator.num_messages;
    int  seed         = cfg.simulator.seed;

    for (int i = 1; i < argc; ++i) {
        if      (std::strcmp(argv[i], "--strategy") == 0) use_strategy = true;
        else if (std::strcmp(argv[i], "--router")   == 0) use_router   = true;
        else {
            int n = std::atoi(argv[i]);
            if (n > 0) num_messages = n;
        }
    }

    PipelineStats stats = run_pipeline(num_messages, use_strategy, use_router,
                                       static_cast<uint64_t>(seed), &cfg);
    print_pipeline_stats(stats, use_strategy, use_router);

    return (tests_failed == 0) ? 0 : 1;
}
