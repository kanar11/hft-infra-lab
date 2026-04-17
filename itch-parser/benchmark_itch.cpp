/*
 * ITCH Parser Benchmark — C++ vs Python comparison
 * Benchmark parsera ITCH — porównanie C++ i Python
 *
 * Generates N synthetic ITCH messages, parses them all, measures throughput.
 * Generuje N syntetycznych wiadomości ITCH, parsuje je wszystkie, mierzy przepustowość.
 *
 * Build / Kompilacja:
 *   g++ -O2 -std=c++17 -o itch_parser/benchmark_itch itch_parser/benchmark_itch.cpp
 *
 * Run / Uruchomienie:
 *   ./itch_parser/benchmark_itch
 */

#include "itch_parser.hpp"

#include <iostream>   // std::cout — console output / wyjście na konsolę
#include <vector>     // std::vector — dynamic array / tablica dynamiczna
#include <chrono>     // high-resolution timer / zegar wysokiej rozdzielczości
#include <random>     // random number generation / generowanie liczb losowych
#include <cstring>    // memcpy, memset
#include <iomanip>    // std::setw, std::fixed — output formatting / formatowanie wyjścia
#include <algorithm>  // std::sort — for latency histogram / do histogramu opóźnień
#include <numeric>    // std::accumulate — sum / suma

// ─────────────────────────────────────────────
// MESSAGE GENERATORS / GENERATORY WIADOMOŚCI
// ─────────────────────────────────────────────
//
// These functions build raw bytes for each ITCH message type.
// Te funkcje budują surowe bajty dla każdego typu wiadomości ITCH.
// In production, the exchange sends these bytes over UDP multicast.
// W produkcji giełda wysyła te bajty przez UDP multicast.

// Write a big-endian 64-bit integer into a byte buffer
// Zapisz 64-bitową liczbę całkowitą big-endian do bufora bajtów
static inline void write_be64(uint8_t* p, int64_t v) {
    uint64_t u = htobe64((uint64_t)v); // host to big-endian / konwersja z formatu hosta do big-endian
    memcpy(p, &u, 8);
}

// Write a big-endian 32-bit unsigned integer
// Zapisz 32-bitową nieznakową liczbę całkowitą big-endian
static inline void write_be32(uint8_t* p, uint32_t v) {
    uint32_t u = htobe32(v);
    memcpy(p, &u, 4);
}

// Build an ADD_ORDER message (34 bytes)
// Zbuduj wiadomość ADD_ORDER (34 bajty)
static void make_add_order(uint8_t* buf, int64_t ts, int64_t ref,
                           char side, uint32_t shares,
                           const char* stock, uint32_t price_raw) {
    buf[0] = 'A';
    write_be64(buf + 1,  ts);
    write_be64(buf + 9,  ref);
    buf[17] = (uint8_t)side;
    write_be32(buf + 18, shares);
    memcpy(buf + 22, stock, 8);    // stock symbol, 8 bytes padded with spaces
                                   // symbol akcji, 8 bajtów wypełnionych spacjami
    write_be32(buf + 30, price_raw); // price * 10000, e.g. 150.25 → 1502500
                                     // cena * 10000, np. 150,25 → 1502500
}

// Build a DELETE_ORDER message (17 bytes)
// Zbuduj wiadomość DELETE_ORDER (17 bajtów)
static void make_delete_order(uint8_t* buf, int64_t ts, int64_t ref) {
    buf[0] = 'D';
    write_be64(buf + 1, ts);
    write_be64(buf + 9, ref);
}

// Build a TRADE message (42 bytes)
// Zbuduj wiadomość TRADE (42 bajty)
static void make_trade(uint8_t* buf, int64_t ts, int64_t ref,
                       char side, uint32_t shares,
                       const char* stock, uint32_t price_raw, int64_t match) {
    buf[0] = 'P';
    write_be64(buf + 1,  ts);
    write_be64(buf + 9,  ref);
    buf[17] = (uint8_t)side;
    write_be32(buf + 18, shares);
    memcpy(buf + 22, stock, 8);
    write_be32(buf + 30, price_raw);
    write_be64(buf + 34, match);
}

// ─────────────────────────────────────────────
// MAIN / FUNKCJA GŁÓWNA
// ─────────────────────────────────────────────

int main() {
    // Number of messages to generate and parse
    // Liczba wiadomości do wygenerowania i sparsowania
    constexpr int N = 10'000'000;  // 10 million / 10 milionów
                                   // constexpr: value computed at compile time, not runtime
                                   // wartość obliczana w czasie kompilacji, nie w czasie uruchomienia

    std::cout << "=== ITCH C++ Parser Benchmark ===" << std::endl;
    std::cout << "=== Benchmark parsera ITCH w C++ ===" << std::endl;
    std::cout << "\nGenerating " << N << " synthetic ITCH messages..." << std::endl;
    std::cout << "Generowanie " << N << " syntetycznych wiadomości ITCH..." << std::endl;

    // ── Build message dataset ──────────────────────────────────────────────
    // Pre-allocate all messages before timing starts (don't measure allocation)
    // Wstępnie alokuj wszystkie wiadomości przed pomiarem (nie mierz alokacji)

    // 'auto': compiler deduces type (std::mt19937 — Mersenne Twister RNG)
    // 'auto': kompilator sam określa typ
    auto rng = std::mt19937{42};  // seed=42 for reproducibility / ziarno=42 dla powtarzalności

    // Uniform distribution: all values equally likely (like dice / jak kość do gry)
    // Rozkład jednostajny: wszystkie wartości jednakowo prawdopodobne
    std::uniform_int_distribution<int>     type_dist(0, 2);   // 0=add, 1=del, 2=trade
    std::uniform_int_distribution<int64_t> ref_dist(1, 100000);
    std::uniform_int_distribution<uint32_t> shares_dist(100, 10000);
    std::uniform_int_distribution<uint32_t> price_dist(1000000, 5000000); // $100-$500

    // Stock symbols (padded to 8 bytes with spaces, as ITCH requires)
    // Symbole akcji (wypełnione do 8 bajtów spacjami, jak wymaga ITCH)
    const char* stocks[] = {
        "AAPL    ", "MSFT    ", "GOOGL   ", "TSLA    ", "AMZN    "
    };
    std::uniform_int_distribution<int> stock_dist(0, 4);

    // Each message is at most 42 bytes. We store type and size alongside data.
    // Każda wiadomość ma co najwyżej 42 bajty. Przechowujemy typ i rozmiar razem z danymi.
    struct RawMsg {
        uint8_t data[42]; // max ITCH message size / maksymalny rozmiar wiadomości ITCH
        size_t  len;
    };

    // std::vector: a resizable array (like a dynamic list)
    // std::vector: tablica dynamiczna (jak lista)
    // Reserve N slots up front so no reallocation happens during fill
    // Zarezerwuj N slotów z góry żeby nie było realokacji podczas wypełniania
    std::vector<RawMsg> messages;
    messages.reserve(N);

    int64_t ts = 34200'000'000'000LL; // 9:30 AM in nanoseconds / 9:30 rano w nanosekundach

    for (int i = 0; i < N; ++i) {
        RawMsg m{};
        memset(m.data, 0, sizeof(m.data));

        int64_t ref    = ref_dist(rng);
        uint32_t shares = shares_dist(rng);
        uint32_t price  = price_dist(rng);
        const char* stk = stocks[stock_dist(rng)];
        char side = (rng() % 2) ? 'B' : 'S';

        // Rotate through 3 message types to simulate a real feed
        // Rotuj przez 3 typy wiadomości żeby symulować prawdziwy feed
        switch (type_dist(rng)) {
            case 0:
                make_add_order(m.data, ts, ref, side, shares, stk, price);
                m.len = 34;
                break;
            case 1:
                make_delete_order(m.data, ts, ref);
                m.len = 17;
                break;
            default:
                make_trade(m.data, ts, ref, side, shares, stk, price, ref * 100);
                m.len = 42;
        }

        ts += 1000; // advance timestamp by 1 microsecond / przesuń czas o 1 mikrosekundę
        messages.push_back(m);
    }

    std::cout << "Done. Starting parse benchmark..." << std::endl;
    std::cout << "Gotowe. Uruchamianie benchmarku parsowania..." << std::endl;

    // ── Throughput benchmark ───────────────────────────────────────────────
    // Parse all N messages and measure total time
    // Sparsuj wszystkie N wiadomości i zmierz całkowity czas

    ITCHParser parser;

    // volatile: prevents compiler from optimizing away the result we "don't use"
    // volatile: zapobiega optymalizacji przez kompilator wyniku który "nie jest używany"
    volatile uint64_t checksum = 0; // used to prevent dead-code elimination / zapobiega eliminacji martwego kodu

    // std::chrono: C++ high-resolution clock (like clock_gettime(CLOCK_MONOTONIC))
    // std::chrono: zegar wysokiej rozdzielczości C++ (jak clock_gettime(CLOCK_MONOTONIC))
    auto t_start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < N; ++i) {
        auto result = parser.parse(messages[i].data, messages[i].len);
        // Use the result so compiler doesn't eliminate the parse call
        // Użyj wyniku żeby kompilator nie wyeliminował wywołania parse
        checksum += (uint64_t)result.type;
    }

    auto t_end = std::chrono::high_resolution_clock::now();

    // Calculate elapsed time in nanoseconds
    // Oblicz czas w nanosekundach
    auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        t_end - t_start).count();

    double elapsed_s     = elapsed_ns / 1e9;
    double msgs_per_sec  = N / elapsed_s;
    double ns_per_msg    = (double)elapsed_ns / N;

    // ── Latency benchmark (per-message timing) ─────────────────────────────
    // Measure latency of parsing individual messages
    // Zmierz opóźnienie parsowania pojedynczych wiadomości

    constexpr int LATENCY_SAMPLES = 10000;
    std::vector<int64_t> latencies;
    latencies.reserve(LATENCY_SAMPLES);

    for (int i = 0; i < LATENCY_SAMPLES; ++i) {
        auto t0 = std::chrono::high_resolution_clock::now();
        auto r = parser.parse(messages[i % N].data, messages[i % N].len);
        auto t1 = std::chrono::high_resolution_clock::now();
        (void)r; // suppress unused variable warning / wycisz ostrzeżenie o nieużywanej zmiennej
        latencies.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    }

    // Sort latencies to compute percentiles
    // Posortuj opóźnienia żeby obliczyć percentyle
    std::sort(latencies.begin(), latencies.end());

    auto p50  = latencies[LATENCY_SAMPLES * 50  / 100];
    auto p95  = latencies[LATENCY_SAMPLES * 95  / 100];
    auto p99  = latencies[LATENCY_SAMPLES * 99  / 100];
    auto p999 = latencies[LATENCY_SAMPLES * 999 / 1000];

    const auto& s = parser.stats();

    // ── Print results ──────────────────────────────────────────────────────

    std::cout << "\n=== Results / Wyniki ===" << std::endl;
    std::cout << std::fixed << std::setprecision(2);

    std::cout << "\n  Messages parsed:   " << N << std::endl;
    std::cout << "  Elapsed time:      " << elapsed_s * 1000.0 << " ms" << std::endl;
    std::cout << "\n  Throughput:        " << msgs_per_sec / 1e6 << " million msg/sec" << std::endl;
    std::cout << "  Przepustowość:     " << msgs_per_sec / 1e6 << " milionów wiad./sek" << std::endl;
    std::cout << "  Avg per message:   " << ns_per_msg << " ns" << std::endl;
    std::cout << "  Śr. na wiadomość:  " << ns_per_msg << " ns" << std::endl;

    std::cout << "\n  Latency percentiles (single message) / Percentyle opóźnień (pojedyncza wiadomość):" << std::endl;
    std::cout << "    p50:    " << p50  << " ns" << std::endl;
    std::cout << "    p95:    " << p95  << " ns" << std::endl;
    std::cout << "    p99:    " << p99  << " ns" << std::endl;
    std::cout << "    p99.9:  " << p999 << " ns" << std::endl;

    std::cout << "\n  Message type breakdown / Podział typów wiadomości:" << std::endl;
    std::cout << "    ADD_ORDER:       " << s.add_orders     << std::endl;
    std::cout << "    DELETE_ORDER:    " << s.delete_orders  << std::endl;
    std::cout << "    REPLACE_ORDER:   " << s.replace_orders << std::endl;
    std::cout << "    ORDER_EXECUTED:  " << s.executions     << std::endl;
    std::cout << "    ORDER_CANCELLED: " << s.cancels        << std::endl;
    std::cout << "    TRADE:           " << s.trades         << std::endl;
    std::cout << "    SYSTEM_EVENT:    " << s.system_events  << std::endl;
    std::cout << "    UNKNOWN:         " << s.unknowns       << std::endl;

    std::cout << "\n  Python equivalent: ~1-2 million msg/sec" << std::endl;
    std::cout << "  Odpowiednik Python: ~1-2 miliony wiad./sek" << std::endl;
    std::cout << "  Speedup: ~" << (int)(msgs_per_sec / 1.5e6) << "x faster than Python" << std::endl;
    std::cout << "  Przyspieszenie: ~" << (int)(msgs_per_sec / 1.5e6) << "x szybciej niż Python" << std::endl;

    std::cout << "\n  (checksum=" << checksum << " — prevents dead-code optimization)" << std::endl;

    return 0;
}
