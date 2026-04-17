# Benchmarks / Wzorce Wydajności

Dedicated micro-benchmarks measuring core HFT operation latencies.
*Dedykowane mikro-benchmarki mierzące opóźnienia podstawowych operacji HFT.*

## Results Summary / Podsumowanie wyników

### Ping-Pong Thread-to-Thread Latency

Measures atomic flag round-trip between two threads (simulates market data → strategy path).
*Mierzy czas okrążenia flagi atomowej między dwoma wątkami (symuluje ścieżkę dane rynkowe → strategia).*

| Metric | Value |
|--------|-------|
| **Avg RTT** | **121 ns** |
| Min | 50 ns |
| p50 | 81 ns |
| p90 | 110 ns |
| p99 | 120 ns |
| p99.9 | 3,196 ns |
| Throughput | 8.3 M round-trips/sec |

### Orderbook Operations

Per-operation latencies for the matching engine's three core operations.
*Opóźnienia na operację dla trzech podstawowych operacji silnika dopasowującego.*

| Operation | Avg | p50 | p90 | p99 | Throughput |
|-----------|-----|-----|-----|-----|------------|
| **ADD** (insert order) | **85 ns** | 40 ns | 50 ns | 1,413 ns | 11.8 M ops/sec |
| **CANCEL** (remove order) | **296 ns** | 280 ns | 451 ns | 561 ns | 3.4 M ops/sec |
| **MATCH** (fill crossing) | **114 ns** | 90 ns | 241 ns | 281 ns | 8.7 M ops/sec |

> Note: Results measured on sandbox Linux (2 vCPU). Your Red Hat VM results may differ.
> Uwaga: Wyniki zmierzone na sandboxowym Linuxie. Twoje wyniki na Red Hat VM mogą się różnić.

## How to Run / Jak uruchomić

```bash
# Build all benchmarks (from project root)
make build

# Run ping-pong latency test (default: 100K rounds)
./benchmarks/latency_benchmark 100000

# Run orderbook micro-benchmark (default: 100K ops)
./benchmarks/orderbook_benchmark 100000

# Save results to CSV
./benchmarks/latency_benchmark 100000 > benchmarks/results/latency_raw.csv
./benchmarks/orderbook_benchmark 100000 > benchmarks/results/orderbook_raw.csv

# Generate charts (requires gnuplot)
cd benchmarks/results && gnuplot plot_latency.gnuplot
cd benchmarks/results && gnuplot plot_orderbook.gnuplot
```

## What Each Benchmark Tests / Co testuje każdy benchmark

### latency_benchmark.cpp — Ping-Pong

Two threads communicate via `std::atomic<int>` with cache-line alignment (`alignas(64)`).
Thread A sets flag to 1 (ping), Thread B responds with 2 (pong). We measure the round-trip.

*Dwa wątki komunikują się przez `std::atomic<int>` z wyrównaniem do linii cache (`alignas(64)`).
Wątek A ustawia flagę na 1 (ping), Wątek B odpowiada 2 (pong). Mierzymy czas okrążenia.*

Key techniques / Kluczowe techniki:
- `alignas(64)` — prevents false sharing between cache lines
- `memory_order_acquire/release` — minimal memory ordering (faster than `seq_cst`)
- `__builtin_ia32_pause()` — x86 PAUSE instruction for efficient spin-waiting
- Warmup phase — fills caches and trains branch predictor before measurement

### orderbook_benchmark.cpp — Add/Cancel/Match

Benchmarks a simplified orderbook with `std::map` (sorted tree) + `std::unordered_map` (hash index).

*Benchmarkuje uproszczony orderbook z `std::map` (posortowane drzewo) + `std::unordered_map` (indeks hash).*

- **ADD**: Insert order into price-level tree + hash index → O(log N)
- **CANCEL**: Hash lookup O(1) + tree removal O(log N)
- **MATCH**: Walk the tree from best price, fill against resting orders

## Files / Pliki

| File | Description |
|------|-------------|
| `latency_benchmark.cpp` | Ping-pong thread-to-thread latency test |
| `orderbook_benchmark.cpp` | Orderbook add/cancel/match micro-benchmark |
| `results/latency_raw.csv` | Raw latency data (round, ns) |
| `results/latency_summary.txt` | Human-readable latency summary |
| `results/orderbook_raw.csv` | Raw orderbook data (operation, round, ns) |
| `results/orderbook_summary.txt` | Human-readable orderbook summary |
| `results/plot_latency.gnuplot` | Gnuplot script for latency histogram |
| `results/plot_orderbook.gnuplot` | Gnuplot script for orderbook chart |

## Real-World Comparison / Porównanie z produkcją

| Metric | This Lab | Production HFT |
|--------|----------|-----------------|
| Thread-to-thread RTT | ~120 ns | 50-80 ns (kernel bypass + CPU pinning) |
| Orderbook ADD | ~85 ns | 20-50 ns (custom allocator, flat arrays) |
| Orderbook MATCH | ~114 ns | 30-80 ns (FPGA-assisted) |
| Orderbook CANCEL | ~296 ns | 50-100 ns (lock-free, pre-allocated) |
