/*
 * DPDK Kernel Bypass Simulator — C++ Implementation
 * Symulator omijania jądra DPDK — implementacja C++
 *
 * Simulates kernel bypass concepts: poll-mode driver vs interrupt-driven I/O.
 * In real HFT, DPDK bypasses the Linux kernel network stack entirely —
 * packets go directly from NIC to userspace via DMA (Direct Memory Access).
 * Symuluje koncepcje omijania jądra: sterownik trybu sondowania vs I/O z przerwaniami.
 * W prawdziwym HFT, DPDK omija cały stos sieciowy Linuxa — pakiety idą
 * bezpośrednio z karty sieciowej do przestrzeni użytkownika przez DMA.
 *
 * Why this matters / Dlaczego to ważne:
 *   Interrupt mode: NIC → kernel IRQ → context switch → copy → userspace
 *   Poll mode:      NIC → DMA → userspace ring buffer (zero copy!)
 *
 *   Interrupt: ~10-50μs per packet (context switches are expensive)
 *   Poll mode: ~1-5μs per packet (busy-wait, no context switch)
 *
 * This simulator measures the overhead difference using simulated packets
 * in memory — no actual network required.
 * Ten symulator mierzy różnicę narzutów używając symulowanych pakietów
 * w pamięci — nie wymaga rzeczywistej sieci.
 *
 * Pipeline: NIC → **DPDK** → ITCH Parser → Strategy → OMS
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <chrono>
#include <thread>
#include <atomic>
#include <algorithm>
#include <vector>

// Simulated packet size (typical market data message)
// Rozmiar symulowanego pakietu (typowa wiadomość danych rynkowych)
static constexpr int PACKET_SIZE = 64;

// Ring buffer size for poll-mode (power of 2 for fast modulo)
// Rozmiar bufora pierścieniowego dla trybu sondowania (potęga 2 dla szybkiego modulo)
static constexpr int RING_SIZE = 4096;
static constexpr int RING_MASK = RING_SIZE - 1;


// === Simulated packet ===
// Like an Ethernet frame but simplified — timestamp + payload
// Jak ramka Ethernet ale uproszczona — znacznik czasu + ładunek

struct alignas(64) SimPacket {
    int64_t  send_ts_ns;            // sender timestamp / znacznik czasu nadawcy
    uint32_t seq_num;               // sequence number / numer sekwencji
    uint8_t  payload[PACKET_SIZE];  // market data / dane rynkowe

    SimPacket() noexcept : send_ts_ns(0), seq_num(0) {
        std::memset(payload, 0, PACKET_SIZE);
    }
};


// === Ring buffer — shared between producer and consumer ===
// Like DPDK's rte_ring — lock-free SPSC (single producer, single consumer)
// Jak rte_ring z DPDK — bezblokadowy SPSC (jeden producent, jeden konsument)

struct alignas(64) PacketRing {
    SimPacket packets[RING_SIZE];
    alignas(64) std::atomic<uint32_t> head{0};  // write position / pozycja zapisu
    alignas(64) std::atomic<uint32_t> tail{0};  // read position / pozycja odczytu

    bool push(const SimPacket& pkt) noexcept {
        uint32_t h = head.load(std::memory_order_relaxed);
        uint32_t next = (h + 1) & RING_MASK;
        if (next == tail.load(std::memory_order_acquire)) return false;  // full
        packets[h] = pkt;
        head.store(next, std::memory_order_release);
        return true;
    }

    bool pop(SimPacket& pkt) noexcept {
        uint32_t t = tail.load(std::memory_order_relaxed);
        if (t == head.load(std::memory_order_acquire)) return false;  // empty
        pkt = packets[t];
        tail.store((t + 1) & RING_MASK, std::memory_order_release);
        return true;
    }
};


// === Benchmark results ===

struct BypassStats {
    int64_t  total_packets;
    double   avg_latency_ns;
    int64_t  p50_ns;
    int64_t  p99_ns;
    int64_t  min_ns;
    int64_t  max_ns;
    double   throughput_mpps;       // million packets per second
    const char* mode;               // "POLL" or "INTERRUPT"

    void print() const {
        printf("  Mode:        %s\n", mode);
        printf("  Packets:     %ld\n", total_packets);
        printf("  Avg latency: %.0f ns\n", avg_latency_ns);
        printf("  p50:         %ld ns\n", p50_ns);
        printf("  p99:         %ld ns\n", p99_ns);
        printf("  Min:         %ld ns\n", min_ns);
        printf("  Max:         %ld ns\n", max_ns);
        printf("  Throughput:  %.1f M pkt/sec\n", throughput_mpps);
    }
};


// === KernelBypassSimulator ===

class KernelBypassSimulator {

    static int64_t now_ns() noexcept {
        auto now = std::chrono::high_resolution_clock::now();
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()).count();
    }

    // simulate_interrupt_overhead: add artificial delay representing kernel overhead
    // In real systems: IRQ handler → softirq → socket buffer copy → wake process
    // Like 'sleep 0.00001' but at nanosecond granularity — simulates context switch cost
    // Symuluje narzut przerwania: handler IRQ → softirq → kopiowanie bufora → budzenie procesu
    static void simulate_interrupt_overhead() noexcept {
        // Burn ~500ns of CPU cycles to simulate interrupt + context switch overhead
        // Spal ~500ns cykli CPU żeby symulować narzut przerwania + przełączenia kontekstu
        volatile int x = 0;
        for (int i = 0; i < 150; ++i) {
            x += i;
        }
    }

public:
    // benchmark_poll_mode: measure latency of DPDK-style poll mode
    // Packets go directly to ring buffer — consumer polls continuously
    // Pakiety idą bezpośrednio do bufora pierścieniowego — konsument sonduje ciągle
    static BypassStats benchmark_poll_mode(int num_packets) {
        PacketRing ring;
        std::vector<int64_t> latencies;
        latencies.reserve(num_packets);
        std::atomic<bool> done{false};

        // Producer thread: simulate NIC DMA writing to ring buffer
        // Wątek producenta: symuluje DMA karty sieciowej zapisujące do bufora
        std::thread producer([&]() {
            for (int i = 0; i < num_packets; ++i) {
                SimPacket pkt;
                pkt.seq_num = i;
                pkt.send_ts_ns = now_ns();
                while (!ring.push(pkt)) {}  // spin if full / kręć się jeśli pełny
            }
            done.store(true, std::memory_order_release);
        });

        // Consumer: poll mode — busy-wait for packets (no sleep, no yield)
        // Konsument: tryb sondowania — aktywne czekanie na pakiety (bez sleep, bez yield)
        int received = 0;
        auto total_start = now_ns();

        while (received < num_packets) {
            SimPacket pkt;
            if (ring.pop(pkt)) {
                int64_t recv_ts = now_ns();
                latencies.push_back(recv_ts - pkt.send_ts_ns);
                received++;
            }
            // No sleep — this IS the poll mode advantage
            // Bez sleep — to JEST przewaga trybu sondowania
        }

        auto total_end = now_ns();
        producer.join();

        // Calculate stats
        std::sort(latencies.begin(), latencies.end());
        int n = latencies.size();
        int64_t total_ns = total_end - total_start;

        BypassStats stats;
        stats.mode = "POLL";
        stats.total_packets = n;
        stats.avg_latency_ns = 0;
        for (auto l : latencies) stats.avg_latency_ns += l;
        stats.avg_latency_ns /= n;
        stats.p50_ns = latencies[n / 2];
        stats.p99_ns = latencies[(int)(n * 0.99)];
        stats.min_ns = latencies.front();
        stats.max_ns = latencies.back();
        stats.throughput_mpps = n / (total_ns / 1e9) / 1e6;
        return stats;
    }

    // benchmark_interrupt_mode: measure latency with simulated kernel overhead
    // Each packet incurs interrupt + context switch cost before delivery
    // Każdy pakiet ponosi koszt przerwania + przełączenia kontekstu przed dostarczeniem
    static BypassStats benchmark_interrupt_mode(int num_packets) {
        PacketRing ring;
        std::vector<int64_t> latencies;
        latencies.reserve(num_packets);
        std::atomic<bool> done{false};

        // Producer: simulate NIC with interrupt overhead
        std::thread producer([&]() {
            for (int i = 0; i < num_packets; ++i) {
                SimPacket pkt;
                pkt.seq_num = i;
                pkt.send_ts_ns = now_ns();
                // Simulate kernel overhead: IRQ → softirq → buffer copy
                simulate_interrupt_overhead();
                while (!ring.push(pkt)) {}
            }
            done.store(true, std::memory_order_release);
        });

        // Consumer: also pays interrupt overhead (context switch to wake)
        int received = 0;
        auto total_start = now_ns();

        while (received < num_packets) {
            SimPacket pkt;
            if (ring.pop(pkt)) {
                // Simulate kernel-to-userspace copy overhead
                simulate_interrupt_overhead();
                int64_t recv_ts = now_ns();
                latencies.push_back(recv_ts - pkt.send_ts_ns);
                received++;
            }
        }

        auto total_end = now_ns();
        producer.join();

        std::sort(latencies.begin(), latencies.end());
        int n = latencies.size();
        int64_t total_ns = total_end - total_start;

        BypassStats stats;
        stats.mode = "INTERRUPT";
        stats.total_packets = n;
        stats.avg_latency_ns = 0;
        for (auto l : latencies) stats.avg_latency_ns += l;
        stats.avg_latency_ns /= n;
        stats.p50_ns = latencies[n / 2];
        stats.p99_ns = latencies[(int)(n * 0.99)];
        stats.min_ns = latencies.front();
        stats.max_ns = latencies.back();
        stats.throughput_mpps = n / (total_ns / 1e9) / 1e6;
        return stats;
    }
};
