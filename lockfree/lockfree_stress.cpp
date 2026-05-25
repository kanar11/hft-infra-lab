/*
 * lockfree_stress — testy stresowe wszystkich prymitywów lock-free
 *                   POD THREAD SANITIZER.
 *
 * Po co? TSAN to narzędzie kompilatora które przy uruchomieniu wyłapuje
 * **wyścigi danych** (dwa wątki piszą do tej samej pamięci bez synchronizacji).
 * Bugi tego typu są wyjątkowo paskudne — działają w 99% przypadków, padają
 * raz na milion przy konkretnym timing'u. Statyczne narzędzia (cppcheck,
 * clang-tidy) ich NIE łapią — trzeba zaobserwować runtime żeby wiedzieć.
 *
 * Co testujemy:
 *   - SPSCQueue          (1 prod, 1 kons)   ←już w spsc_queue.cpp, dla kompletności
 *   - MPSCQueue          (N prod, 1 kons)
 *   - MPMCQueue          (N prod, N kons)
 *   - WaitableMPSCQueue  (N prod, 1 kons z blokującym pop_wait)
 *   - Sequencer          (1 prod, 1 kons, LMAX-style zero-copy)
 *   - VarlenRingBuffer   (1 prod, 1 kons, ramki o zmiennej długości)
 *
 * Każdy test:
 *   1. Spawn'uje wątki producenta/konsumenta.
 *   2. Pompuje N wiadomości (każda z unikalnym monotonicznym sequence).
 *   3. Konsument sprawdza że seq monotonicznie rośnie i że nie ma luk.
 *   4. Asercja: producenci wysłali == konsumenci odebrali.
 *
 * Build dla TSAN (patrz .github/workflows/tests.yml):
 *   g++ -O1 -g -fsanitize=thread -fno-omit-frame-pointer -std=c++17 \
 *       -Wall -Wextra -pthread -o lockfree/lockfree_stress lockfree/lockfree_stress.cpp
 *
 * Uruchomienie:
 *   TSAN_OPTIONS=halt_on_error=1 ./lockfree/lockfree_stress
 */
#include "spsc_queue.hpp"
#include "mpsc_queue.hpp"
#include "mpmc_queue.hpp"
#include "waitable_mpsc.hpp"
#include "sequencer.hpp"
#include "varlen_ring.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>


namespace {

constexpr int N_MESSAGES_PER_TEST = 50'000;   // Wystarczająco żeby TSAN złapał race
constexpr int N_PRODUCERS         = 4;        // Dla MPSC/MPMC
constexpr int N_CONSUMERS         = 4;        // Dla MPMC


// Pojedyncza wiadomość z sekwencją + checksum (do detekcji corruption'u).
struct Msg {
    std::uint64_t seq;
    std::uint64_t producer_id;
    std::uint64_t checksum;   // = seq ^ producer_id ^ 0xDEADBEEF — wykryje torn write
};

inline std::uint64_t msg_checksum(std::uint64_t seq, std::uint64_t pid) noexcept {
    return seq ^ pid ^ 0xDEADBEEFULL;
}


bool test_spsc() {
    std::printf("[SPSC]          ");
    lockfree::SPSCQueue<Msg, 1024> q;
    std::atomic<bool> prod_done{false};
    std::atomic<int>  received{0};
    std::atomic<bool> corrupted{false};

    std::thread cons([&]() {
        Msg m{};
        std::uint64_t expected = 0;
        while (!prod_done.load(std::memory_order_acquire) || !q.empty()) {
            if (q.pop(m)) {
                if (m.checksum != msg_checksum(m.seq, m.producer_id) || m.seq != expected) {
                    corrupted.store(true, std::memory_order_relaxed);
                }
                ++expected;
                received.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    for (int i = 0; i < N_MESSAGES_PER_TEST; ++i) {
        Msg m{static_cast<std::uint64_t>(i), 0, msg_checksum(static_cast<std::uint64_t>(i), 0)};
        while (!q.push(m)) { std::this_thread::yield(); }
    }
    prod_done.store(true, std::memory_order_release);
    cons.join();

    const bool ok = !corrupted.load() && received.load() == N_MESSAGES_PER_TEST;
    std::printf("%s (received=%d, corrupted=%s)\n",
                ok ? "OK" : "FAIL",
                received.load(), corrupted.load() ? "true" : "false");
    return ok;
}


bool test_mpsc() {
    std::printf("[MPSC]          ");
    lockfree::MPSCQueue<Msg, 4096> q;
    std::atomic<int>  producers_done{0};
    std::atomic<int>  received{0};
    std::atomic<bool> corrupted{false};

    std::thread cons([&]() {
        Msg m{};
        while (producers_done.load(std::memory_order_acquire) < N_PRODUCERS || !q.empty()) {
            if (q.pop(m)) {
                if (m.checksum != msg_checksum(m.seq, m.producer_id)) {
                    corrupted.store(true, std::memory_order_relaxed);
                }
                received.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    std::vector<std::thread> producers;
    producers.reserve(N_PRODUCERS);
    const int per_producer = N_MESSAGES_PER_TEST / N_PRODUCERS;
    for (int p = 0; p < N_PRODUCERS; ++p) {
        producers.emplace_back([&, p]() {
            for (int i = 0; i < per_producer; ++i) {
                const auto seq = static_cast<std::uint64_t>(i);
                const auto pid = static_cast<std::uint64_t>(p);
                Msg m{seq, pid, msg_checksum(seq, pid)};
                while (!q.push(m)) { std::this_thread::yield(); }
            }
            producers_done.fetch_add(1, std::memory_order_release);
        });
    }
    for (auto& t : producers) t.join();
    cons.join();

    const int expected = N_PRODUCERS * per_producer;
    const bool ok = !corrupted.load() && received.load() == expected;
    std::printf("%s (received=%d/%d, corrupted=%s)\n",
                ok ? "OK" : "FAIL", received.load(), expected,
                corrupted.load() ? "true" : "false");
    return ok;
}


bool test_mpmc() {
    std::printf("[MPMC]          ");
    lockfree::MPMCQueue<Msg, 4096> q;
    std::atomic<int>  producers_done{0};
    std::atomic<int>  received{0};
    std::atomic<bool> corrupted{false};
    std::atomic<bool> stop_consumers{false};

    std::vector<std::thread> consumers;
    consumers.reserve(N_CONSUMERS);
    for (int c = 0; c < N_CONSUMERS; ++c) {
        consumers.emplace_back([&]() {
            Msg m{};
            while (!stop_consumers.load(std::memory_order_acquire) || !q.empty()) {
                if (q.pop(m)) {
                    if (m.checksum != msg_checksum(m.seq, m.producer_id)) {
                        corrupted.store(true, std::memory_order_relaxed);
                    }
                    received.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    std::vector<std::thread> producers;
    producers.reserve(N_PRODUCERS);
    const int per_producer = N_MESSAGES_PER_TEST / N_PRODUCERS;
    for (int p = 0; p < N_PRODUCERS; ++p) {
        producers.emplace_back([&, p]() {
            for (int i = 0; i < per_producer; ++i) {
                const auto seq = static_cast<std::uint64_t>(i);
                const auto pid = static_cast<std::uint64_t>(p);
                Msg m{seq, pid, msg_checksum(seq, pid)};
                while (!q.push(m)) { std::this_thread::yield(); }
            }
            producers_done.fetch_add(1, std::memory_order_release);
        });
    }
    for (auto& t : producers) t.join();
    // Czekaj aż konsumenci wybiorą wszystko.
    while (received.load() < N_PRODUCERS * per_producer) std::this_thread::yield();
    stop_consumers.store(true, std::memory_order_release);
    for (auto& t : consumers) t.join();

    const int expected = N_PRODUCERS * per_producer;
    const bool ok = !corrupted.load() && received.load() == expected;
    std::printf("%s (received=%d/%d, corrupted=%s)\n",
                ok ? "OK" : "FAIL", received.load(), expected,
                corrupted.load() ? "true" : "false");
    return ok;
}


bool test_waitable_mpsc() {
    std::printf("[WaitableMPSC]  ");
    lockfree::WaitableMPSCQueue<Msg, 4096> q;
    std::atomic<int>  producers_done{0};
    std::atomic<int>  received{0};
    std::atomic<bool> corrupted{false};
    std::atomic<bool> stop_consumer{false};

    std::thread cons([&]() {
        Msg m{};
        while (!stop_consumer.load(std::memory_order_acquire)) {
            if (q.pop_wait(m, std::chrono::milliseconds(10))) {
                if (m.checksum != msg_checksum(m.seq, m.producer_id)) {
                    corrupted.store(true, std::memory_order_relaxed);
                }
                received.fetch_add(1, std::memory_order_relaxed);
            }
        }
        // Drain reszty.
        Msg leftover{};
        while (q.try_pop(leftover)) {
            if (leftover.checksum != msg_checksum(leftover.seq, leftover.producer_id)) {
                corrupted.store(true, std::memory_order_relaxed);
            }
            received.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::vector<std::thread> producers;
    producers.reserve(N_PRODUCERS);
    const int per_producer = N_MESSAGES_PER_TEST / N_PRODUCERS;
    for (int p = 0; p < N_PRODUCERS; ++p) {
        producers.emplace_back([&, p]() {
            for (int i = 0; i < per_producer; ++i) {
                const auto seq = static_cast<std::uint64_t>(i);
                const auto pid = static_cast<std::uint64_t>(p);
                Msg m{seq, pid, msg_checksum(seq, pid)};
                while (!q.push(m)) { std::this_thread::yield(); }
            }
            producers_done.fetch_add(1, std::memory_order_release);
        });
    }
    for (auto& t : producers) t.join();
    // Daj konsumentowi czas na drain.
    while (received.load() < N_PRODUCERS * per_producer) std::this_thread::yield();
    stop_consumer.store(true, std::memory_order_release);
    cons.join();

    const int expected = N_PRODUCERS * per_producer;
    const bool ok = !corrupted.load() && received.load() == expected;
    std::printf("%s (received=%d/%d, corrupted=%s)\n",
                ok ? "OK" : "FAIL", received.load(), expected,
                corrupted.load() ? "true" : "false");
    return ok;
}


bool test_sequencer() {
    std::printf("[Sequencer]     ");
    lockfree::Sequencer<Msg, 1024> seq;
    std::atomic<bool> prod_done{false};
    std::atomic<int>  received{0};
    std::atomic<bool> corrupted{false};

    std::thread cons([&]() {
        std::int64_t next = 0;
        while (!prod_done.load(std::memory_order_acquire) || next <= seq.available()) {
            const std::int64_t hi = seq.available();
            while (next <= hi) {
                const Msg& m = seq.read(next);
                if (m.checksum != msg_checksum(m.seq, m.producer_id) ||
                    m.seq != static_cast<std::uint64_t>(next)) {
                    corrupted.store(true, std::memory_order_relaxed);
                }
                ++next;
                received.fetch_add(1, std::memory_order_relaxed);
            }
            seq.mark_consumed(next - 1);
        }
    });

    for (int i = 0; i < N_MESSAGES_PER_TEST; ++i) {
        std::int64_t s;
        while ((s = seq.try_claim()) < 0) { std::this_thread::yield(); }
        Msg& slot      = seq.slot(s);
        slot.seq       = static_cast<std::uint64_t>(s);
        slot.producer_id = 0;
        slot.checksum  = msg_checksum(slot.seq, 0);
        seq.publish(s);
    }
    prod_done.store(true, std::memory_order_release);
    cons.join();

    const bool ok = !corrupted.load() && received.load() == N_MESSAGES_PER_TEST;
    std::printf("%s (received=%d, corrupted=%s)\n",
                ok ? "OK" : "FAIL", received.load(),
                corrupted.load() ? "true" : "false");
    return ok;
}


bool test_varlen_ring() {
    std::printf("[VarlenRing]    ");
    lockfree::VarlenRingBuffer<8192> ring;
    std::atomic<bool> prod_done{false};
    std::atomic<int>  received{0};
    std::atomic<bool> corrupted{false};

    std::thread cons([&]() {
        std::uint8_t buf[256];
        std::uint64_t expected_seq = 0;
        while (!prod_done.load(std::memory_order_acquire) || !ring.empty()) {
            const std::uint32_t len = ring.read(buf, sizeof(buf));
            if (len > 0) {
                // Wiadomość zaczyna się od 8-bajtowego seq, potem padding.
                std::uint64_t got_seq;
                std::memcpy(&got_seq, buf, sizeof(got_seq));
                if (got_seq != expected_seq) corrupted.store(true, std::memory_order_relaxed);
                ++expected_seq;
                received.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    std::uint8_t payload[64] = {};  // zero-init żeby memcpy nie czytał uninit bajtów
    for (int i = 0; i < N_MESSAGES_PER_TEST; ++i) {
        const std::uint64_t s = static_cast<std::uint64_t>(i);
        std::memcpy(payload, &s, sizeof(s));
        // Zmienna długość — testuje wrap przy różnych offsetach (16..64 B).
        const std::uint32_t len = 16 + static_cast<std::uint32_t>(i % 49);
        while (!ring.write(payload, len)) { std::this_thread::yield(); }
    }
    prod_done.store(true, std::memory_order_release);
    cons.join();

    const bool ok = !corrupted.load() && received.load() == N_MESSAGES_PER_TEST;
    std::printf("%s (received=%d, corrupted=%s)\n",
                ok ? "OK" : "FAIL", received.load(),
                corrupted.load() ? "true" : "false");
    return ok;
}

}  // namespace


int main() {
    std::printf("=== lockfree stress tests (TSAN-enabled when built with -fsanitize=thread) ===\n");
    std::printf("messages per test: %d, producers: %d, consumers (MPMC): %d\n\n",
                N_MESSAGES_PER_TEST, N_PRODUCERS, N_CONSUMERS);

    bool all_ok = true;
    all_ok &= test_spsc();
    all_ok &= test_mpsc();
    all_ok &= test_mpmc();
    all_ok &= test_waitable_mpsc();
    all_ok &= test_sequencer();
    all_ok &= test_varlen_ring();

    std::printf("\n=== %s ===\n", all_ok ? "ALL PASSED" : "FAILED");
    return all_ok ? 0 : 1;
}
