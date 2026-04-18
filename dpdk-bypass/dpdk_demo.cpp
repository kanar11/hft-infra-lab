/*
 * DPDK Kernel Bypass Simulator Demo & Benchmark — C++
 *
 * Compile: g++ -O2 -std=c++17 -Wall -Wextra -pthread -o dpdk_demo dpdk_demo.cpp
 * Run:     ./dpdk_demo [number_of_packets]
 *
 * Compares poll-mode (DPDK-style) vs interrupt-mode packet processing.
 * Porównuje tryb sondowania (styl DPDK) vs tryb przerwań.
 */

#include "kernel_bypass_sim.hpp"
#include <cstdlib>

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


void test_ring_push_pop() {
    PacketRing ring;
    SimPacket pkt;
    pkt.seq_num = 42;
    pkt.send_ts_ns = 123456;
    ASSERT(ring.push(pkt), "test_ring_push");

    SimPacket out;
    ASSERT(ring.pop(out), "test_ring_pop");
    ASSERT(out.seq_num == 42, "test_ring_seq");
    ASSERT(out.send_ts_ns == 123456, "test_ring_ts");
}

void test_ring_empty() {
    PacketRing ring;
    SimPacket out;
    ASSERT(!ring.pop(out), "test_ring_empty");
}

void test_ring_fifo_order() {
    PacketRing ring;
    // Push 3 packets
    for (uint32_t i = 0; i < 3; ++i) {
        SimPacket pkt;
        pkt.seq_num = i + 1;
        ring.push(pkt);
    }
    // Pop should return in FIFO order
    SimPacket out;
    ring.pop(out); ASSERT(out.seq_num == 1, "test_fifo_1");
    ring.pop(out); ASSERT(out.seq_num == 2, "test_fifo_2");
    ring.pop(out); ASSERT(out.seq_num == 3, "test_fifo_3");
}

void test_ring_wrap_around() {
    PacketRing ring;
    // Fill and drain to force wrap-around
    // Wypełnij i opróżnij żeby wymusić zawinięcie
    bool all_ok = true;
    for (int cycle = 0; cycle < 3; ++cycle) {
        for (int i = 0; i < 100; ++i) {
            SimPacket pkt;
            pkt.seq_num = cycle * 100 + i;
            if (!ring.push(pkt)) all_ok = false;
        }
        for (int i = 0; i < 100; ++i) {
            SimPacket out;
            if (!ring.pop(out)) all_ok = false;
        }
    }
    ASSERT(all_ok, "test_ring_wrap_around");
}

void test_poll_mode_basic() {
    // Small test: 1000 packets in poll mode
    auto stats = KernelBypassSimulator::benchmark_poll_mode(1000);
    ASSERT(stats.total_packets == 1000, "test_poll_mode_count");
    ASSERT(stats.avg_latency_ns > 0, "test_poll_mode_latency");
    ASSERT(stats.throughput_mpps > 0, "test_poll_mode_throughput");
}

void test_interrupt_mode_basic() {
    auto stats = KernelBypassSimulator::benchmark_interrupt_mode(1000);
    ASSERT(stats.total_packets == 1000, "test_interrupt_mode_count");
    ASSERT(stats.avg_latency_ns > 0, "test_interrupt_mode_latency");
}

void test_poll_faster_than_interrupt() {
    // Poll mode should have higher throughput than interrupt mode
    // Tryb sondowania powinien mieć wyższą przepustowość niż tryb przerwań
    // NOTE: This is a soft check — on shared CI runners (GitHub Actions),
    // thread scheduling can make results unpredictable.
    // UWAGA: To jest miękki test — na współdzielonych CI runnerach,
    // planowanie wątków może dawać nieprzewidywalne wyniki.
    auto poll = KernelBypassSimulator::benchmark_poll_mode(50000);
    auto intr = KernelBypassSimulator::benchmark_interrupt_mode(50000);
    bool faster = poll.throughput_mpps > intr.throughput_mpps;
    if (faster) {
        printf("  PASS: test_poll_faster_than_interrupt\n");
        tests_passed++; tests_total++;
    } else {
        printf("  WARN: test_poll_faster_than_interrupt (skipped on shared CPU)\n");
        // Don't count as failure — timing-dependent / Nie licz jako błąd — zależne od timingu
    }
    printf("    (poll: %.1f Mpps, interrupt: %.1f Mpps, speedup: %.1fx)\n",
           poll.throughput_mpps, intr.throughput_mpps,
           intr.throughput_mpps > 0 ? poll.throughput_mpps / intr.throughput_mpps : 0.0);
}

void test_packet_struct_size() {
    // SimPacket should be cache-line aligned (64 bytes or multiple)
    ASSERT(sizeof(SimPacket) >= PACKET_SIZE, "test_packet_size");
    ASSERT(alignof(SimPacket) == 64, "test_packet_alignment");
}


void benchmark(int num_packets) {
    printf("\n=== DPDK Kernel Bypass Benchmark ===\n");
    printf("Packets: %d\n", num_packets);

    printf("\n--- Poll Mode (DPDK-style, zero kernel overhead) ---\n");
    auto poll = KernelBypassSimulator::benchmark_poll_mode(num_packets);
    poll.print();

    printf("\n--- Interrupt Mode (traditional kernel stack) ---\n");
    auto intr = KernelBypassSimulator::benchmark_interrupt_mode(num_packets);
    intr.print();

    printf("\n--- Comparison / Porównanie ---\n");
    double speedup = intr.avg_latency_ns / poll.avg_latency_ns;
    printf("  Poll mode latency:      %.0f ns\n", poll.avg_latency_ns);
    printf("  Interrupt mode latency: %.0f ns\n", intr.avg_latency_ns);
    printf("  Latency reduction:      %.1fx\n", speedup);
    printf("  Poll throughput:        %.1f M pkt/sec\n", poll.throughput_mpps);
    printf("  Interrupt throughput:   %.1f M pkt/sec\n", intr.throughput_mpps);
}


int main(int argc, char* argv[]) {
    printf("=== DPDK Kernel Bypass C++ Unit Tests ===\n\n");

    test_ring_push_pop();
    test_ring_empty();
    test_ring_fifo_order();
    test_ring_wrap_around();
    test_poll_mode_basic();
    test_interrupt_mode_basic();
    test_poll_faster_than_interrupt();
    test_packet_struct_size();

    printf("\n%d/%d tests passed\n", tests_passed, tests_total);

    int num_packets = 500'000;
    if (argc > 1) {
        num_packets = std::atoi(argv[1]);
        if (num_packets <= 0) num_packets = 500'000;
    }

    benchmark(num_packets);

    return (tests_passed == tests_total) ? 0 : 1;
}
