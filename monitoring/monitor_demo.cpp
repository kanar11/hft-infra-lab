/*
 * Infrastructure Monitor Demo & Benchmark — C++
 *
 * Compile: g++ -O2 -std=c++17 -Wall -Wextra -o monitor_demo monitor_demo.cpp
 * Run:     ./monitor_demo [num_parse_iterations]
 *
 * Tests /proc parser functions with mock data + benchmarks real /proc reads.
 */

#include "infra_monitor.hpp"
#include <vector>
#include <algorithm>
#include <cstdlib>
#include <cmath>

static int tests_passed = 0;
static int tests_total = 0;

#define ASSERT(cond, msg) do { \
    tests_total++; \
    if (!(cond)) { \
        printf("  FAIL: %s (%s)\n", msg, #cond); \
    } else { \
        printf("  PASS: %s\n", msg); \
        tests_passed++; \
    } \
} while(0)


// === Mock /proc data for unit testing ===
// Like creating test fixtures — predictable data to verify parser logic
// Jak tworzenie danych testowych — przewidywalne dane do weryfikacji logiki parsera

static const char* MOCK_PROC_STAT =
    "cpu  10000 200 3000 50000 100 0 50 0 0 0\n"
    "cpu0 5000 100 1500 25000 50 0 25 0 0 0\n"
    "cpu1 5000 100 1500 25000 50 0 25 0 0 0\n"
    "intr 1234567\n"
    "ctxt 9876543\n"
    "processes 12345\n";

static const char* MOCK_MEMINFO =
    "MemTotal:        4028440 kB\n"
    "MemFree:          500000 kB\n"
    "MemAvailable:    2014220 kB\n"
    "Buffers:          100000 kB\n"
    "Cached:           800000 kB\n"
    "HugePages_Total:     128\n"
    "HugePages_Free:       64\n"
    "HugePages_Rsvd:        0\n"
    "Hugepagesize:       2048 kB\n";

static const char* MOCK_NET_DEV =
    "Inter-|   Receive                                                |  Transmit\n"
    " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed\n"
    "    lo: 1234567   10000    0    0    0     0          0         0  1234567   10000    0    0    0     0       0          0\n"
    "  eth0: 9876543   50000    0    0    0     0          0         0  5432100   30000    0    0    0     0       0          0\n";


void test_parse_cpu_stats() {
    auto stats = proc_parser::parse_cpu_stats(MOCK_PROC_STAT);
    // total = 10000+200+3000+50000+100+0+50+0+0+0 = 63350
    // idle = 50000 (4th field)
    ASSERT(stats.idle == 50000, "test_cpu_idle");
    ASSERT(stats.total == 63350, "test_cpu_total");
}

void test_parse_context_switches() {
    int64_t ctx = proc_parser::parse_context_switches(MOCK_PROC_STAT);
    ASSERT(ctx == 9876543, "test_context_switches");
}

void test_parse_meminfo() {
    auto mem = proc_parser::parse_meminfo(MOCK_MEMINFO);
    ASSERT(mem.total_kb == 4028440, "test_mem_total");
    ASSERT(mem.available_kb == 2014220, "test_mem_available");
    ASSERT(mem.hugepages_total == 128, "test_hugepages_total");
    ASSERT(mem.hugepages_free == 64, "test_hugepages_free");
    ASSERT(mem.hugepages_used() == 64, "test_hugepages_used");
    // used_percent = (1 - 2014220/4028440) * 100 ≈ 50%
    ASSERT(std::fabs(mem.used_percent - 50.0) < 1.0, "test_mem_percent");
    ASSERT(mem.total_mb() == 3934, "test_mem_mb");
}

void test_parse_net_dev() {
    auto net = proc_parser::parse_net_dev(MOCK_NET_DEV);
    ASSERT(net.valid, "test_net_valid");
    ASSERT(std::strcmp(net.interface, "eth0") == 0, "test_net_iface");
    ASSERT(net.rx_bytes == 9876543, "test_net_rx_bytes");
    ASSERT(net.rx_packets == 50000, "test_net_rx_pkts");
    ASSERT(net.tx_bytes == 5432100, "test_net_tx_bytes");
    ASSERT(net.tx_packets == 30000, "test_net_tx_pkts");
}

void test_parse_net_dev_skips_loopback() {
    // Verify that 'lo' is skipped and eth0 is returned
    auto net = proc_parser::parse_net_dev(MOCK_NET_DEV);
    ASSERT(std::strcmp(net.interface, "lo") != 0, "test_net_skips_lo");
}

void test_parse_empty_content() {
    auto cpu = proc_parser::parse_cpu_stats("");
    ASSERT(cpu.total == 0, "test_empty_cpu");

    int64_t ctx = proc_parser::parse_context_switches("");
    ASSERT(ctx == 0, "test_empty_ctx");

    auto mem = proc_parser::parse_meminfo("");
    ASSERT(mem.total_kb == 0, "test_empty_mem");

    auto net = proc_parser::parse_net_dev("");
    ASSERT(!net.valid, "test_empty_net");
}

void test_alert_thresholds() {
    AlertThresholds t;
    t.mem_percent = 80.0;
    InfraMonitor mon(t);

    // Mock memory with 90% usage — should trigger alert
    MemoryStats mem;
    mem.total_kb = 4000000;
    mem.available_kb = 400000;
    mem.hugepages_total = 0;
    mem.hugepages_free = 0;
    mem.used_percent = 90.0;
    mem.total_kb = 4000000;

    char alerts[4][128];
    int n = mon.check_alerts(mem, alerts, 4);
    ASSERT(n == 1, "test_alert_triggered");

    // Mock memory with 50% usage — no alert
    mem.used_percent = 50.0;
    n = mon.check_alerts(mem, alerts, 4);
    ASSERT(n == 0, "test_alert_not_triggered");
}

void test_live_proc_read() {
    // Test reading actual /proc files (if available)
    // Testuj czytanie prawdziwych plików /proc (jeśli dostępne)
    InfraMonitor mon;
    auto mem = mon.collect_memory();
    // On any Linux system, MemTotal should be > 0
    ASSERT(mem.total_kb > 0, "test_live_proc_meminfo");
}

void test_parse_speed() {
    auto start = std::chrono::high_resolution_clock::now();
    auto mem = proc_parser::parse_meminfo(MOCK_MEMINFO);
    auto end = std::chrono::high_resolution_clock::now();
    int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    ASSERT(ns < 1'000'000, "test_parse_speed");
    ASSERT(mem.total_kb > 0, "test_parse_speed_valid");
    printf("    (meminfo parse: %ld ns)\n", ns);
}


void benchmark(int iterations) {
    printf("\n=== /proc Parser Throughput Benchmark ===\n");
    printf("Iterations: %d\n\n", iterations);

    // Benchmark 1: parse /proc/stat (mock data)
    {
        std::vector<int64_t> latencies;
        latencies.reserve(iterations);
        auto total_start = std::chrono::high_resolution_clock::now();

        for (int i = 0; i < iterations; ++i) {
            auto start = std::chrono::high_resolution_clock::now();
            auto stats [[maybe_unused]] = proc_parser::parse_cpu_stats(MOCK_PROC_STAT);
            auto end = std::chrono::high_resolution_clock::now();
            latencies.push_back(
                std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
        }

        auto total_end = std::chrono::high_resolution_clock::now();
        int64_t total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            total_end - total_start).count();
        std::sort(latencies.begin(), latencies.end());
        int n = latencies.size();
        printf("--- /proc/stat parse ---\n");
        printf("  Avg: %.0f ns  p50: %ld ns  p99: %ld ns  Throughput: %.1f M/sec\n",
               (double)total_ns / n, latencies[n/2], latencies[(int)(n*0.99)],
               n / (total_ns / 1e9) / 1e6);
    }

    // Benchmark 2: parse /proc/meminfo (mock data)
    {
        std::vector<int64_t> latencies;
        latencies.reserve(iterations);
        auto total_start = std::chrono::high_resolution_clock::now();

        for (int i = 0; i < iterations; ++i) {
            auto start = std::chrono::high_resolution_clock::now();
            auto mem [[maybe_unused]] = proc_parser::parse_meminfo(MOCK_MEMINFO);
            auto end = std::chrono::high_resolution_clock::now();
            latencies.push_back(
                std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
        }

        auto total_end = std::chrono::high_resolution_clock::now();
        int64_t total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            total_end - total_start).count();
        std::sort(latencies.begin(), latencies.end());
        int n = latencies.size();
        printf("--- /proc/meminfo parse ---\n");
        printf("  Avg: %.0f ns  p50: %ld ns  p99: %ld ns  Throughput: %.1f M/sec\n",
               (double)total_ns / n, latencies[n/2], latencies[(int)(n*0.99)],
               n / (total_ns / 1e9) / 1e6);
    }

    // Benchmark 3: parse /proc/net/dev (mock data)
    {
        std::vector<int64_t> latencies;
        latencies.reserve(iterations);
        auto total_start = std::chrono::high_resolution_clock::now();

        for (int i = 0; i < iterations; ++i) {
            auto start = std::chrono::high_resolution_clock::now();
            auto net [[maybe_unused]] = proc_parser::parse_net_dev(MOCK_NET_DEV);
            auto end = std::chrono::high_resolution_clock::now();
            latencies.push_back(
                std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
        }

        auto total_end = std::chrono::high_resolution_clock::now();
        int64_t total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            total_end - total_start).count();
        std::sort(latencies.begin(), latencies.end());
        int n = latencies.size();
        printf("--- /proc/net/dev parse ---\n");
        printf("  Avg: %.0f ns  p50: %ld ns  p99: %ld ns  Throughput: %.1f M/sec\n",
               (double)total_ns / n, latencies[n/2], latencies[(int)(n*0.99)],
               n / (total_ns / 1e9) / 1e6);
    }

    // Benchmark 4: live /proc read + parse (full snapshot)
    {
        InfraMonitor mon;
        std::vector<int64_t> latencies;
        int live_iters = iterations / 10;  // fewer iterations for I/O
        latencies.reserve(live_iters);

        auto total_start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < live_iters; ++i) {
            auto start = std::chrono::high_resolution_clock::now();
            auto mem [[maybe_unused]] = mon.collect_memory();
            auto end = std::chrono::high_resolution_clock::now();
            latencies.push_back(
                std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
        }
        auto total_end = std::chrono::high_resolution_clock::now();
        int64_t total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            total_end - total_start).count();
        std::sort(latencies.begin(), latencies.end());
        int n = latencies.size();
        printf("--- Live /proc/meminfo read+parse ---\n");
        printf("  Avg: %.0f ns  p50: %ld ns  p99: %ld ns  Throughput: %.1f K/sec\n",
               (double)total_ns / n, latencies[n/2], latencies[(int)(n*0.99)],
               n / (total_ns / 1e9) / 1e3);
    }
}


int main(int argc, char* argv[]) {
    printf("=== Infrastructure Monitor C++ Unit Tests ===\n\n");

    test_parse_cpu_stats();
    test_parse_context_switches();
    test_parse_meminfo();
    test_parse_net_dev();
    test_parse_net_dev_skips_loopback();
    test_parse_empty_content();
    test_alert_thresholds();
    test_live_proc_read();
    test_parse_speed();

    printf("\n%d/%d tests passed\n", tests_passed, tests_total);

    int iterations = 500'000;
    if (argc > 1) {
        iterations = std::atoi(argv[1]);
        if (iterations <= 0) iterations = 500'000;
    }

    benchmark(iterations);

    return (tests_passed == tests_total) ? 0 : 1;
}
