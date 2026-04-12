// cache_latency.cpp — Pointer-chasing memory latency benchmark
// Measures access latency across L1, L2, L3 cache and main RAM
// Part of hft-infra-lab: github.com/kanar11/hft-infra-lab

#include <iostream>
#include <vector>
#include <numeric>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <random>

struct Node {
    Node* next;
    char pad[56];
};

inline void escape(void* p) {
    asm volatile("" : : "g"(p) : "memory");
}

double measure_latency(size_t array_size_bytes, int iterations) {
    size_t count = array_size_bytes / sizeof(Node);
    if (count < 2) count = 2;
    std::vector<Node> nodes(count);
    std::vector<size_t> indices(count);
    std::iota(indices.begin(), indices.end(), 0);
    std::mt19937 rng(42);
    std::shuffle(indices.begin(), indices.end(), rng);
    for (size_t i = 0; i < count - 1; i++) {
        nodes[indices[i]].next = &nodes[indices[i + 1]];
    }
    nodes[indices[count - 1]].next = &nodes[indices[0]];
    Node* p = &nodes[0];
    for (int i = 0; i < (int)count * 4; i++) {
        p = p->next;
    }
    escape(p);
    int chase_count = iterations;
    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < chase_count; i++) {
        p = p->next;
        escape(p);
    }
    auto end = std::chrono::high_resolution_clock::now();
    double elapsed_ns = std::chrono::duration<double, std::nano>(end - start).count();
    return elapsed_ns / chase_count;
}

int main() {
    std::cout << "=== HFT Infra Lab — Cache Latency Benchmark ===" << std::endl;
    std::cout << "Method: pointer chasing (defeats prefetcher)" << std::endl;
    std::cout << std::endl;
    struct TestCase { const char* label; size_t size; int iters; };
    TestCase tests[] = {
        {"L1  (32 KB)", 32*1024, 5000000},
        {"L2  (256 KB)", 256*1024, 2000000},
        {"L3  (8 MB)", 8*1024*1024, 1000000},
        {"RAM (64 MB)", 64*1024*1024, 500000},
        {"RAM (256 MB)", 256*1024*1024, 200000}
    };
    std::cout << "Target          | Latency (ns)" << std::endl;
    std::cout << "----------------|-------------" << std::endl;
    for (auto& t : tests) {
        double lat = measure_latency(t.size, t.iters);
        printf("%-16s| %.2f ns\n", t.label, lat);
    }
    std::cout << std::endl;
    std::cout << "Expected ranges (typical desktop/server):" << std::endl;
    std::cout << "  L1: ~1-2 ns | L2: ~3-5 ns | L3: ~10-20 ns | RAM: ~60-100 ns" << std::endl;
    return 0;
}
