# Cache Latency Benchmark / Benchmark opóźnienia pamięci podręcznej

Pointer-chasing memory latency benchmark measuring access times across L1, L2, L3 cache and main RAM.
*Benchmark opóźnienia pamięci z śledzeniem wskaźników mierzący czasy dostępu w pamięci L1, L2, L3 i pamięci RAM.*

## Why This Matters / Dlaczego to ma znaczenie
In HFT, every memory access matters. A TLB miss or cache miss can add 100ns+ to a hot path. This benchmark measures the real cost of memory hierarchy on your hardware.

*W HFT każdy dostęp do pamięci ma znaczenie. Brakujący wpis TLB lub cache miss może dodać 100ns+ do gorącej ścieżki. Ten benchmark mierzy rzeczywisty koszt hierarchii pamięci na Twoim sprzęcie.*

## Method / Metoda
- Creates a linked list of 64-byte nodes (one cache line each)
- Shuffles pointers randomly to defeat hardware prefetcher
- Chases pointers and measures average access latency per level

*- Tworzy połączoną listę węzłów 64-bajtowych (jedna linia pamięci podręcznej każda)*
*- Losowo tasuje wskaźniki, aby pokonać prefetura sprzętu*
*- Śledzi wskaźniki i mierzy średnie opóźnienie dostępu na każdym poziomie*

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

## Key Takeaway / Kluczowa lekcja
L3 dropped from 154ns to 38ns after CPU isolation — proving that `isolcpus=1` eliminates cache pollution from other processes.

*L3 spadł z 154ns na 38ns po izolacji CPU — dowodząc, że `isolcpus=1` eliminuje zanieczyszczenie pamięci podręcznej z innych procesów.*
