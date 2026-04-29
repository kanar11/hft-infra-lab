/*
 * Ping-Pong Latency Benchmark — Thread-to-Thread Communication
 * Benchmark opóźnienia Ping-Pong — komunikacja wątek-do-wątku
 *
 * WHAT THIS MEASURES:
 * How fast can two threads exchange messages? This simulates the critical
 * path in HFT: one thread receives market data, another thread processes it.
 * The latency between them determines how fast you can react to price changes.
 *
 * CO TO MIERZY:
 * Jak szybko dwa wątki mogą wymieniać wiadomości? Symuluje to krytyczną
 * ścieżkę w HFT: jeden wątek odbiera dane rynkowe, drugi je przetwarza.
 * Opóźnienie między nimi determinuje jak szybko reagujesz na zmiany cen.
 *
 * HOW IT WORKS:
 * Thread A sets a flag and waits. Thread B sees the flag, responds.
 * We measure the round-trip time (ping → pong) in nanoseconds.
 * This is called "ping-pong" because the signal bounces back and forth.
 *
 * JAK TO DZIAŁA:
 * Wątek A ustawia flagę i czeka. Wątek B widzi flagę, odpowiada.
 * Mierzymy czas okrążenia (ping → pong) w nanosekundach.
 * Nazywa się "ping-pong" bo sygnał odbija się tam i z powrotem.
 *
 * REAL-WORLD CONTEXT:
 * In production HFT systems, this kind of inter-thread latency is typically:
 *   - Shared memory + atomic:  50-200 ns  (what we test here)
 *   - Lock-free queue (SPSC):  100-300 ns
 *   - Mutex + condition var:   1,000-5,000 ns  (too slow for HFT)
 *   - TCP loopback:            5,000-20,000 ns (way too slow)
 *
 * Compile / Kompilacja:
 *   g++ -O2 -std=c++17 -Wall -Wextra -pthread -o latency_benchmark latency_benchmark.cpp
 *
 * Run / Uruchomienie:
 *   ./latency_benchmark [rounds]    # default: 1,000,000 rounds
 *
 * Output: CSV results to stdout, summary to stderr
 *         CSV na stdout, podsumowanie na stderr
 */

#include <atomic>
// atomic: variables that can be safely read/written by multiple threads simultaneously
// Like a shared whiteboard with a rule: only one person can write at a time
// atomic: zmienne które mogą być bezpiecznie czytane/zapisywane przez wiele wątków jednocześnie
// Jak wspólna tablica z zasadą: tylko jedna osoba może pisać naraz

#include <thread>
// thread: allows running functions in parallel (like & in bash: command &)
// thread: pozwala uruchamiać funkcje równolegle (jak & w bashu: command &)

#include <chrono>
// chrono: high-resolution time measurement (like 'time' command in bash but nanosecond precision)
// chrono: pomiar czasu o wysokiej rozdzielczości (jak 'time' w bashu ale z nanosekundową precyzją)

#include <vector>
#include <algorithm>
#include <numeric>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

// alignas(64): tells compiler to place this variable on a 64-byte boundary (cache line)
// WHY: if two atomics share a cache line, they "false share" — each write by one core
// invalidates the other core's cache. Alignment prevents this performance killer.
// alignas(64): mówi kompilatorowi aby umieścił zmienną na granicy 64 bajtów (linia cache)
// DLACZEGO: jeśli dwa atomiki dzielą linię cache, "fałszywie współdzielą" — każdy zapis
// przez jeden rdzeń unieważnia cache drugiego rdzenia. Wyrównanie zapobiega temu.

// Ping-pong state: 0 = idle, 1 = ping sent, 2 = pong sent
// Stan ping-pong: 0 = bezczynny, 1 = ping wysłany, 2 = pong wysłany
alignas(64) std::atomic<int> flag{0};

// Padding to ensure flag and any other data are on separate cache lines
// Wypełnienie aby zapewnić że flaga i inne dane są na osobnych liniach cache
alignas(64) std::atomic<bool> ready{false};


// pong_thread: the "responder" — waits for ping, sends pong
// wątek pong: "odpowiadający" — czeka na ping, wysyła pong
void pong_thread(int rounds) {
    // Signal that this thread is ready
    // Sygnalizuj że ten wątek jest gotowy
    ready.store(true, std::memory_order_release);

    for (int i = 0; i < rounds; ++i) {
        // Spin-wait for ping (flag == 1)
        // memory_order_acquire: ensures we see all writes that happened before the store
        // Aktywne czekanie na ping (flag == 1)
        // memory_order_acquire: zapewnia że widzimy wszystkie zapisy sprzed ustawienia
        while (flag.load(std::memory_order_acquire) != 1) {
            // __builtin_ia32_pause(): x86 PAUSE instruction
            // Tells CPU "I'm in a spin loop" — saves power, reduces pipeline stalls
            // __builtin_ia32_pause(): instrukcja x86 PAUSE
            // Mówi CPU "jestem w pętli oczekiwania" — oszczędza energię, zmniejsza blokady pipeline
            #if defined(__x86_64__) || defined(__i386__)
            __builtin_ia32_pause();
            #endif
        }

        // Respond with pong (flag = 2)
        // memory_order_release: ensures our response is visible to the other thread
        // Odpowiedz pongiem (flag = 2)
        // memory_order_release: zapewnia że nasza odpowiedź jest widoczna dla drugiego wątku
        flag.store(2, std::memory_order_release);
    }
}


int main(int argc, char* argv[]) {
    int rounds = 1'000'000;
    if (argc > 1) {
        rounds = std::atoi(argv[1]);
        if (rounds <= 0) rounds = 1'000'000;
    }

    fprintf(stderr, "=== Ping-Pong Latency Benchmark ===\n");
    fprintf(stderr, "Rounds: %d\n\n", rounds);

    // Total rounds = warmup + measured
    // Całkowite rundy = rozgrzewka + mierzone
    int warmup = std::min(1000, rounds / 10);
    int total_rounds = warmup + rounds;

    // Store individual round-trip times for percentile calculation
    // Przechowuj indywidualne czasy okrążeń do obliczenia percentyli
    std::vector<int64_t> latencies;
    latencies.reserve(rounds);

    // Start the pong thread (handles all rounds: warmup + measured)
    // Uruchom wątek pong (obsługuje wszystkie rundy: rozgrzewka + mierzone)
    ready.store(false, std::memory_order_release);
    flag.store(0, std::memory_order_release);
    std::thread responder(pong_thread, total_rounds);

    // Wait for pong thread to be ready
    // Czekaj aż wątek pong będzie gotowy
    while (!ready.load(std::memory_order_acquire)) {
        #if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
        #endif
    }

    // Warm up: a few rounds to fill caches and train branch predictor
    // Rozgrzewka: kilka rund żeby wypełnić cache i wytrenować predyktor skoków
    for (int i = 0; i < warmup; ++i) {
        flag.store(1, std::memory_order_release);
        while (flag.load(std::memory_order_acquire) != 2) {
            #if defined(__x86_64__) || defined(__i386__)
            __builtin_ia32_pause();
            #endif
        }
        flag.store(0, std::memory_order_release);
    }

    // === Main benchmark loop ===
    // === Główna pętla benchmarku ===
    auto total_start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < rounds; ++i) {
        auto start = std::chrono::high_resolution_clock::now();

        // Send ping (flag = 1)
        flag.store(1, std::memory_order_release);

        // Wait for pong (flag == 2)
        while (flag.load(std::memory_order_acquire) != 2) {
            #if defined(__x86_64__) || defined(__i386__)
            __builtin_ia32_pause();
            #endif
        }

        auto end = std::chrono::high_resolution_clock::now();

        // Reset for next round
        // Reset na następną rundę
        flag.store(0, std::memory_order_release);

        int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
        latencies.push_back(ns);
    }

    auto total_end = std::chrono::high_resolution_clock::now();
    responder.join();

    // === Calculate statistics ===
    // === Oblicz statystyki ===
    std::sort(latencies.begin(), latencies.end());

    int64_t total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(total_end - total_start).count();
    double avg_ns = static_cast<double>(total_ns) / rounds;

    // Percentile calculation: sort all latencies, pick the value at X% position
    // Like: "99% of round-trips were faster than this value"
    // Obliczanie percentyli: posortuj wszystkie opóźnienia, wybierz wartość na pozycji X%
    // Jak: "99% okrążeń było szybszych niż ta wartość"
    int64_t p50  = latencies[static_cast<size_t>(rounds) / 2];
    int64_t p90  = latencies[static_cast<size_t>(rounds * 0.90)];
    int64_t p99  = latencies[static_cast<size_t>(rounds * 0.99)];
    int64_t p999 = latencies[static_cast<size_t>(rounds * 0.999)];
    int64_t min_lat = latencies.front();
    int64_t max_lat = latencies.back();

    // Print summary to stderr (human-readable)
    fprintf(stderr, "--- Results ---\n");
    fprintf(stderr, "  Total time:  %.2f ms\n", total_ns / 1e6);
    fprintf(stderr, "  Avg RTT:     %.0f ns\n", avg_ns);
    fprintf(stderr, "  Min:         %ld ns\n", min_lat);
    fprintf(stderr, "  p50:         %ld ns\n", p50);
    fprintf(stderr, "  p90:         %ld ns\n", p90);
    fprintf(stderr, "  p99:         %ld ns\n", p99);
    fprintf(stderr, "  p99.9:       %ld ns\n", p999);
    fprintf(stderr, "  Max:         %ld ns\n", max_lat);
    fprintf(stderr, "  Throughput:  %.1f M round-trips/sec\n",
            rounds / (total_ns / 1e9) / 1e6);

    // Print CSV header + data to stdout (machine-readable)
    // Output CSV to stdout for saving to file
    printf("round,latency_ns\n");
    // Print every 100th sample to keep CSV manageable
    for (int i = 0; i < rounds; i += std::max(1, rounds / 10000)) {
        printf("%d,%ld\n", i, latencies[static_cast<size_t>(i)]);
    }

    return 0;
}
