# Cache Latency Benchmark

Pointer-chasing memory latency benchmark measuring access times across L1, L2, L3 cache and main RAM.

## Why This Matters
In HFT, every memory access matters. A TLB miss or cache miss can add 100ns+ to a hot path. This benchmark measures the real cost of memory hierarchy on your hardware.

## Method
- Creates a linked list of 64-byte nodes (one cache line each)
- Shuffles pointers randomly to defeat hardware prefetcher
- Chases pointers and measures average access latency per level

## Results (Red Hat EL10, VirtualBox 2-core VM, tuned)

| Target | Latency |
|--------|---------|
| L1 (32 KB) | 0.97 ns |
| L2 (256 KB) | 3.38 ns |
| L3 (8 MB) | 38.26 ns |
| RAM (64 MB) | 124.14 ns |
| RAM (256 MB) | 108.51 ns |

## Build & Run
```bash
g++ -O2 -std=c++17 -o cache_latency cache_latency.cpp
./cache_latency
```

## Key Takeaway
L3 dropped from 154ns to 38ns after CPU isolation — proving that `isolcpus=1` eliminates cache pollution from other processes.
