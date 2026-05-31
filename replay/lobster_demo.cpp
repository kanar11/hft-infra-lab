/*
 * lobster_demo — replay a LOBSTER message CSV through the lab pipeline.
 *
 * For each row in the CSV the demo:
 *   - SUBMIT (event 1)            → OMS::submit_order + TradeLogger event
 *   - EXECUTE_VISIBLE (event 4)   → OMS::fill_order   + TradeLogger event
 *   - CANCEL_PARTIAL / DELETE     → OMS::cancel_order + TradeLogger event
 *   - everything else             → counted, ignored
 *
 * The symbol is taken from the filename (LOBSTER convention:
 * "<TICKER>_<DATE>_messages.csv"); falls back to "UNKNOWN" if it can't be
 * parsed. A LOBSTER→OMS order_id map handles the cancel/fill lookup —
 * LOBSTER IDs are 64-bit exchange IDs, OMS issues its own sequential IDs.
 *
 * Run:
 *   ./replay/lobster_demo replay/sample_aapl.csv          # uses bundled fixture
 *   ./replay/lobster_demo /path/to/AAPL_2012-06-21_messages.csv
 */
#include "lobster_reader.hpp"

#include "../common/types.hpp"
#include "../oms/oms.hpp"
#include "../logger/trade_logger.hpp"

#include <cstdio>
#include <cstring>
#include <unordered_map>


// Pull the ticker out of a LOBSTER filename like
// "AAPL_2012-06-21_34200000_57600000_message_5.csv".
static void extract_ticker(const char* path, char* out_sym, std::size_t cap) noexcept {
    // basename
    const char* base = path;
    for (const char* p = path; *p; ++p) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    std::size_t i = 0;
    while (i + 1 < cap && base[i] && base[i] != '_' && base[i] != '.') {
        out_sym[i] = base[i];
        ++i;
    }
    if (i == 0) {
        // No ticker prefix — use a safe default.
        std::strncpy(out_sym, "UNKNOWN", cap - 1);
        out_sym[cap - 1] = '\0';
        return;
    }
    out_sym[i] = '\0';
}


struct ReplayStats {
    std::uint64_t rows           = 0;
    std::uint64_t submits        = 0;
    std::uint64_t executes       = 0;
    std::uint64_t cancels        = 0;
    std::uint64_t halts          = 0;
    std::uint64_t skipped        = 0;
    std::uint64_t oms_rejects    = 0;
    std::uint64_t orphan_fills   = 0;  // execute for an order_id we never saw
    double        first_ts_s     = 0.0;
    double        last_ts_s      = 0.0;
};


// Inline testy malformed CSV — sprawdzają że reader bezpiecznie pomija
// uszkodzone wiersze i kontynuuje. Wywoływane na początku main().
static int run_malformed_tests() {
    const char* path = "/tmp/hft_lobster_test.csv";
    FILE* f = std::fopen(path, "w");
    if (!f) { std::fprintf(stderr, "TEST SKIP: cannot write %s\n", path); return 0; }
    // 3 dobre wiersze, między nimi: pusty, za mało pól, niedopuszczalne znaki, komentarz.
    std::fprintf(f, "34200.1,1,11885113,100,2238100,1\n");
    std::fprintf(f, "\n");                                       // pusty
    std::fprintf(f, "# comment\n");                              // komentarz
    std::fprintf(f, "34200.2,1,11885114,200\n");                 // za mało pól
    std::fprintf(f, "garbage,not,a,csv,row\n");                  // nieparsowalne
    std::fprintf(f, "34200.3,4,11885113,50,2238100,1\n");
    std::fprintf(f, "34200.4,3,11885114,200,2238500,-1\n");
    std::fclose(f);

    lobster::LobsterReader r(path);
    if (!r.is_open()) { std::fprintf(stderr, "TEST FAIL: cannot reopen %s\n", path); return 1; }
    lobster::LobsterMessage m{};
    int good = 0;
    while (r.next(m)) ++good;
    std::remove(path);

    int rc = 0;
    if (good != 3) {
        std::fprintf(stderr, "TEST FAIL malformed: expected 3 good rows, got %d\n", good);
        rc = 1;
    }
    if (r.rows_bad() < 2) {  // co najmniej "za mało pól" + "garbage"
        std::fprintf(stderr, "TEST FAIL malformed: rows_bad()=%lu < 2\n",
                     static_cast<unsigned long>(r.rows_bad()));
        rc = 1;
    }
    if (rc == 0) std::printf("[ok] lobster malformed CSV test: 3 valid, %lu bad\n",
                              static_cast<unsigned long>(r.rows_bad()));
    return rc;
}


int main(int argc, char* argv[]) {
    if (int rc = run_malformed_tests(); rc != 0) return rc;

    const char* path = (argc > 1) ? argv[1] : "replay/sample_aapl.csv";
    char sym[16];
    extract_ticker(path, sym, sizeof(sym));

    lobster::LobsterReader reader(path);
    if (!reader.is_open()) {
        std::fprintf(stderr, "lobster_demo: cannot open %s\n", path);
        return 1;
    }

    // Generous limits — LOBSTER day files can have millions of events.
    OMS         oms(/*max_position=*/2'000'000, /*max_order_value=*/100'000'000.0);
    TradeLogger logger;

    // LOBSTER order_id → OMS internal order_id
    std::unordered_map<std::int64_t, std::uint64_t> id_map;
    id_map.reserve(16'384);

    ReplayStats stats;
    lobster::LobsterMessage msg{};

    std::printf("Replaying %s (ticker=%s)...\n", path, sym);

    while (reader.next(msg)) {
        if (stats.rows == 0) stats.first_ts_s = msg.timestamp_s;
        stats.last_ts_s = msg.timestamp_s;
        stats.rows++;

        const Side side  = (msg.direction > 0) ? Side::BUY : Side::SELL;
        const double px  = msg.price / 10000.0;  // LOBSTER price → dollars

        switch (msg.event_type) {
        case lobster::EventType::SUBMIT: {
            Order* o = oms.submit_order(sym, side, px, static_cast<std::uint32_t>(msg.size));
            if (o) {
                id_map[msg.order_id] = o->order_id;
                logger.log(EventType::ORDER_SUBMIT, o->order_id,
                           sym, side_str(side), msg.size, px);
                stats.submits++;
            } else {
                stats.oms_rejects++;
            }
            break;
        }
        case lobster::EventType::EXECUTE_VISIBLE: {
            auto it = id_map.find(msg.order_id);
            if (it == id_map.end()) {
                stats.orphan_fills++;
                break;
            }
            oms.fill_order(it->second, static_cast<std::uint32_t>(msg.size), px);
            logger.log(EventType::ORDER_FILL, it->second,
                       sym, side_str(side), msg.size, px);
            stats.executes++;
            break;
        }
        case lobster::EventType::CANCEL_PARTIAL:
        case lobster::EventType::DELETE: {
            auto it = id_map.find(msg.order_id);
            if (it == id_map.end()) {
                stats.skipped++;
                break;
            }
            oms.cancel_order(it->second);
            logger.log(EventType::ORDER_CANCEL, it->second,
                       sym, side_str(side), msg.size, px);
            stats.cancels++;
            break;
        }
        case lobster::EventType::HALT:
            stats.halts++;
            break;
        default:
            stats.skipped++;
            break;
        }
    }

    const double span_s = stats.last_ts_s - stats.first_ts_s;

    std::printf("\n=== LOBSTER replay summary ===\n");
    std::printf("  file               : %s\n", path);
    std::printf("  ticker             : %s\n", sym);
    std::printf("  rows read          : %lu\n",       (unsigned long)reader.rows_read());
    std::printf("  rows malformed     : %lu\n",       (unsigned long)reader.rows_bad());
    std::printf("  time span (s)      : %.6f\n",      span_s);
    std::printf("  submits accepted   : %lu\n",       (unsigned long)stats.submits);
    std::printf("  OMS rejects        : %lu\n",       (unsigned long)stats.oms_rejects);
    std::printf("  executions         : %lu\n",       (unsigned long)stats.executes);
    std::printf("  cancels / deletes  : %lu\n",       (unsigned long)stats.cancels);
    std::printf("  halts              : %lu\n",       (unsigned long)stats.halts);
    std::printf("  skipped events     : %lu\n",       (unsigned long)stats.skipped);
    std::printf("  orphan fills       : %lu\n",       (unsigned long)stats.orphan_fills);
    std::printf("  total logged       : %lu events\n",(unsigned long)logger.total_logged());
    std::printf("  open OMS orders    : %lu\n",       (unsigned long)oms.order_count());
    return 0;
}
