/*
 * FIX 4.2 Protocol Parser Demo & Benchmark — C++
 *
 * Compile: g++ -O2 -std=c++17 -Wall -Wextra -o fix_demo fix_demo.cpp
 * Run:     ./fix_demo [number_of_parses]
 */

#include "fix_parser.hpp"
#include "fix_session.hpp"
#include <vector>
#include <algorithm>
#include <cstdlib>

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


void test_parse_new_order() {
    FIXMessage msg;
    msg.parse("8=FIX.4.2|35=D|49=TRADER1|56=EXCHANGE|55=AAPL|54=1|44=150.25|38=100");
    ASSERT(std::strcmp(msg.get_msg_type(), "D") == 0, "test_new_order_type");
    ASSERT(std::strcmp(msg.get_symbol(), "AAPL") == 0, "test_new_order_symbol");
    ASSERT(std::strcmp(msg.get_side(), "BUY") == 0, "test_new_order_side");
    ASSERT(msg.get_price() == 150.25, "test_new_order_price");
    ASSERT(msg.get_quantity() == 100, "test_new_order_qty");
}

void test_parse_sell() {
    FIXMessage msg;
    msg.parse("8=FIX.4.2|35=D|55=MSFT|54=2|44=380.50|38=50");
    ASSERT(std::strcmp(msg.get_side(), "SELL") == 0, "test_sell_side");
    ASSERT(std::strcmp(msg.get_symbol(), "MSFT") == 0, "test_sell_symbol");
    ASSERT(msg.get_price() == 380.50, "test_sell_price");
    ASSERT(msg.get_quantity() == 50, "test_sell_qty");
}

void test_parse_cancel() {
    FIXMessage msg;
    msg.parse("8=FIX.4.2|35=F|49=TRADER1|56=EXCHANGE|55=AAPL|54=1|44=150.25|38=100");
    ASSERT(std::strcmp(msg.get_msg_type(), "F") == 0, "test_cancel_type");
}

void test_parse_execution() {
    FIXMessage msg;
    msg.parse("8=FIX.4.2|35=8|49=EXCHANGE|56=TRADER1|55=AAPL|54=1|44=150.25|38=100");
    ASSERT(std::strcmp(msg.get_msg_type(), "8") == 0, "test_execution_type");
    ASSERT(std::strcmp(msg.get_symbol(), "AAPL") == 0, "test_execution_symbol");
}

void test_malformed_tags() {
    // Malformed tags should not crash — parser skips invalid entries
    FIXMessage msg;
    msg.parse("abc=xyz|35=D|=empty|55=AAPL|bad|44=100.0");
    ASSERT(std::strcmp(msg.get_msg_type(), "D") == 0, "test_malformed_type");
    ASSERT(std::strcmp(msg.get_symbol(), "AAPL") == 0, "test_malformed_symbol");
    ASSERT(msg.get_price() == 100.0, "test_malformed_price");
}

void test_empty_message() {
    FIXMessage msg;
    msg.parse("");
    ASSERT(std::strcmp(msg.get_msg_type(), "UNKNOWN") == 0, "test_empty_type");
    ASSERT(msg.field_count() == 0, "test_empty_fields");
}

void test_null_message() {
    FIXMessage msg;
    msg.parse(nullptr);
    ASSERT(std::strcmp(msg.get_msg_type(), "UNKNOWN") == 0, "test_null_type");
}

void test_field_count() {
    FIXMessage msg;
    msg.parse("8=FIX.4.2|35=D|49=TRADER1|56=EXCHANGE|55=AAPL|54=1|44=150.25|38=100");
    ASSERT(msg.field_count() == 8, "test_field_count");
}

void test_generic_field_lookup() {
    FIXMessage msg;
    msg.parse("8=FIX.4.2|35=D|49=TRADER1|56=EXCHANGE|55=AAPL");
    // Tag 49 = SenderCompID
    const char* sender = msg.get_field(49);
    ASSERT(sender != nullptr, "test_generic_field_exists");
    if (!sender) return;
    ASSERT(std::strcmp(sender, "TRADER1") == 0, "test_generic_field_value");
    // Tag 999 doesn't exist
    ASSERT(msg.get_field(999) == nullptr, "test_generic_field_missing");
}

void test_multiple_messages() {
    // Parse two different messages into separate objects
    FIXMessage buy, sell;
    buy.parse("8=FIX.4.2|35=D|55=AAPL|54=1|44=150.25|38=100");
    sell.parse("8=FIX.4.2|35=D|55=TSLA|54=2|44=250.00|38=200");

    ASSERT(std::strcmp(buy.get_symbol(), "AAPL") == 0, "test_multi_buy_symbol");
    ASSERT(std::strcmp(sell.get_symbol(), "TSLA") == 0, "test_multi_sell_symbol");
    ASSERT(std::strcmp(buy.get_side(), "BUY") == 0, "test_multi_buy_side");
    ASSERT(std::strcmp(sell.get_side(), "SELL") == 0, "test_multi_sell_side");
}

// FIX session validation — CheckSum (tag 10) + BodyLength (tag 9) + SOH delimiter.

void test_checksum_valid() {
    char msg[256];
    int n = FIXMessage::build_message(msg, sizeof(msg),
        "35=D\x01" "55=AAPL\x01" "54=1\x01" "44=150.25\x01" "38=100\x01");
    ASSERT(n > 0, "build_message_ok");

    FIXMessage m;
    m.parse(msg);
    ASSERT(m.checksum_valid(),      "checksum_valid_on_built_msg");
    ASSERT(m.body_length_valid(),   "bodylen_valid_on_built_msg");
    ASSERT(m.has_required_header(), "required_header_present");
    ASSERT(m.is_valid(),            "built_msg_is_valid");
    // Fields also parse with the SOH delimiter (not only '|').
    ASSERT(std::strcmp(m.get_symbol(), "AAPL") == 0, "soh_symbol_parsed");
    ASSERT(m.get_quantity() == 100,                  "soh_qty_parsed");
}

void test_checksum_detects_corruption() {
    char msg[256];
    int n = FIXMessage::build_message(msg, sizeof(msg),
        "35=D\x01" "55=AAPL\x01" "38=100\x01");
    ASSERT(n > 0, "build_for_corruption");

    // Corrupt the body (100 → 900) WITHOUT recomputing the checksum → mismatch.
    char* p = std::strstr(msg, "38=100");
    if (p) p[3] = '9';

    FIXMessage m;
    m.parse(msg);
    ASSERT(!m.checksum_valid(), "checksum_detects_corruption");
    ASSERT(!m.is_valid(),       "corrupted_msg_invalid");
}

void test_pipe_message_not_full_session() {
    // Human-readable '|' without tags 9/10 — fields parse, but this is not
    // a complete session message (missing the required header).
    FIXMessage m;
    m.parse("8=FIX.4.2|35=D|55=AAPL|54=1|44=150.25|38=100");
    ASSERT(!m.has_required_header(), "pipe_msg_no_required_header");
    ASSERT(!m.is_valid(),            "pipe_msg_not_valid_session");
    ASSERT(std::strcmp(m.get_symbol(), "AAPL") == 0, "pipe_msg_fields_still_parse");
}

void test_bodylength_validation() {
    char msg[256];
    FIXMessage::build_message(msg, sizeof(msg),
        "35=0\x01" "49=SENDER\x01" "56=TARGET\x01");  // heartbeat
    FIXMessage m;
    m.parse(msg);
    ASSERT(m.body_length_valid(),         "bodylen_valid_heartbeat");
    ASSERT(m.computed_body_length() > 0,  "bodylen_positive");
    ASSERT(std::strcmp(m.get_msg_type(), "0") == 0, "heartbeat_type");
}

// FIX session layer — sequence tracking, heartbeats, state machine.

void test_session_outbound_seq_monotonic() {
    fix::FIXSession s;
    uint32_t a = s.next_outbound_seq();
    uint32_t b = s.next_outbound_seq();
    uint32_t c = s.next_outbound_seq();
    ASSERT(a == 1 && b == 2 && c == 3, "session_out_seq_starts_at_1_and_monotonic");
}

void test_session_inbound_gap() {
    fix::FIXSession s;
    auto g1 = s.observe_inbound(1, /*now*/0);
    ASSERT(!g1.valid, "session_in_seq_1_no_gap");
    auto g2 = s.observe_inbound(5, /*now*/100);   // jump 1→5
    ASSERT(g2.valid,                 "session_gap_detected");
    ASSERT(g2.expected == 2,         "session_gap_expected");
    ASSERT(g2.received == 5,         "session_gap_received");
    ASSERT(s.gaps_detected() == 1,   "session_gap_counter");
    auto g3 = s.observe_inbound(3, /*now*/200);   // duplicate / late
    ASSERT(!g3.valid,                "session_dup_no_new_gap");
}

void test_session_logon_to_logged_in() {
    fix::FIXSession s;
    ASSERT(s.state() == fix::SessionState::DISCONNECTED, "session_initial_disconnected");
    s.mark_logon_sent(0);
    ASSERT(s.state() == fix::SessionState::LOGON_SENT,   "session_after_logon_sent");
    s.mark_logon_received(/*hb*/30, /*now*/100);
    ASSERT(s.state() == fix::SessionState::LOGGED_IN,    "session_logged_in");
    ASSERT(s.heartbeat_interval_sec() == 30,             "session_hb_interval_set");
}

void test_session_heartbeat_timer() {
    fix::FIXSession s;
    s.mark_logon_sent(0);
    s.mark_logon_received(30, /*now*/0);
    // Shortly after Logon — no action.
    ASSERT(s.tick(1000) == fix::FIXSession::Action::NONE, "session_tick_quiet");
    // 35 sec after Logon, we were silent — send a heartbeat.
    auto a = s.tick(35'000);
    ASSERT(a == fix::FIXSession::Action::SEND_HEARTBEAT, "session_tick_send_hb");
    ASSERT(s.heartbeats_sent() == 1,                      "session_hb_counter");
}

void test_session_test_request_then_disconnect() {
    fix::FIXSession s;
    s.mark_logon_sent(0);
    s.mark_logon_received(30, /*now*/0);
    s.mark_outbound(/*now*/0);   // so we don't trigger SEND_HEARTBEAT on our side
    // 70 sec with no message from the counterparty (> 2×30) — TestRequest.
    auto a = s.tick(70'000);
    ASSERT(a == fix::FIXSession::Action::SEND_TEST_REQUEST, "session_test_request");
    // More silence after test_request → disconnect.
    s.mark_outbound(70'000);
    auto b = s.tick(140'000);
    ASSERT(b == fix::FIXSession::Action::DISCONNECT, "session_disconnect_after_no_response");
}

void test_parse_speed() {
    FIXMessage msg;
    auto start = std::chrono::high_resolution_clock::now();
    msg.parse("8=FIX.4.2|35=D|49=TRADER1|56=EXCHANGE|55=AAPL|54=1|44=150.25|38=100");
    auto end = std::chrono::high_resolution_clock::now();
    int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    ASSERT(ns < 1'000'000, "test_parse_speed");
    printf("    (latency: %ld ns)\n", ns);
}


void benchmark(int num_parses) {
    printf("\n=== FIX Parser Throughput Benchmark ===\n");
    printf("Parses: %d\n\n", num_parses);

    // 4 sample messages (cycle through them for realistic mix)
    const char* messages[] = {
        "8=FIX.4.2|35=D|49=TRADER1|56=EXCHANGE|55=AAPL|54=1|44=150.25|38=100",
        "8=FIX.4.2|35=D|49=TRADER1|56=EXCHANGE|55=MSFT|54=2|44=380.50|38=50",
        "8=FIX.4.2|35=F|49=TRADER1|56=EXCHANGE|55=AAPL|54=1|44=150.25|38=100",
        "8=FIX.4.2|35=8|49=EXCHANGE|56=TRADER1|55=TSLA|54=1|44=250.00|38=200",
    };

    std::vector<int64_t> latencies;
    latencies.reserve(num_parses);

    auto total_start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_parses; ++i) {
        FIXMessage msg;
        auto start = std::chrono::high_resolution_clock::now();
        msg.parse(messages[i % 4]);
        auto end = std::chrono::high_resolution_clock::now();

        latencies.push_back(
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
    }

    auto total_end = std::chrono::high_resolution_clock::now();
    int64_t total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        total_end - total_start).count();

    std::sort(latencies.begin(), latencies.end());
    int n = static_cast<int>(latencies.size());
    double avg = static_cast<double>(total_ns) / n;
    double throughput = n / (total_ns / 1e9) / 1e6;

    printf("--- Results ---\n");
    printf("  Total time:  %.2f ms\n", total_ns / 1e6);
    printf("  Avg:         %.0f ns/parse\n", avg);
    printf("  p50:         %ld ns\n", latencies[n / 2]);
    printf("  p90:         %ld ns\n", latencies[(int)(n * 0.90)]);
    printf("  p99:         %ld ns\n", latencies[(int)(n * 0.99)]);
    printf("  Max:         %ld ns\n", latencies.back());
    printf("  Throughput:  %.1f M msg/sec\n", throughput);
}


int main(int argc, char* argv[]) {
    printf("=== FIX 4.2 Parser C++ Unit Tests ===\n\n");

    test_parse_new_order();
    test_parse_sell();
    test_parse_cancel();
    test_parse_execution();
    test_malformed_tags();
    test_empty_message();
    test_null_message();
    test_field_count();
    test_generic_field_lookup();
    test_multiple_messages();
    test_checksum_valid();
    test_checksum_detects_corruption();
    test_pipe_message_not_full_session();
    test_bodylength_validation();
    test_session_outbound_seq_monotonic();
    test_session_inbound_gap();
    test_session_logon_to_logged_in();
    test_session_heartbeat_timer();
    test_session_test_request_then_disconnect();
    test_parse_speed();

    printf("\n%d/%d tests passed", tests_passed, tests_total);
    if (tests_failed > 0) printf("  (%d FAILED)", tests_failed);
    printf("\n");

    int num_parses = 1'000'000;
    if (argc > 1) {
        num_parses = std::atoi(argv[1]);
        if (num_parses <= 0) num_parses = 1'000'000;
    }

    benchmark(num_parses);

    return (tests_failed == 0) ? 0 : 1;
}
