# Order Book Matching Engine

C++ order book with price-time priority matching.

## Performance
- 23M orders/sec on 2-core VM (throughput benchmark)
- 1M orders processed in 43ms
- Per-order latency: p50 ~50ns, p99 ~130ns (see `latency_histogram.cpp`)
- Compiled with -O2 optimisation

## How it works
- Bids sorted highest-first (std::map with std::greater)
- Asks sorted lowest-first (std::map)
- Matching: when best bid >= best ask, trade executes
- v2 adds cancel/modify with O(log n) lookup via unordered_map<id, Order>

## Build & Run
```bash
# Basic matching engine demo (add/cancel/modify)
g++ -O2 -std=c++17 orderbook_v2.cpp -o orderbook_v2
./orderbook_v2

# Throughput benchmark (1k / 10k / 100k / 1M orders)
g++ -O2 -std=c++17 benchmark_orderbook.cpp -o benchmark_orderbook
./benchmark_orderbook

# Latency histogram (p50 / p95 / p99 / p99.9 / max)
g++ -O2 -std=c++17 latency_histogram.cpp -o latency_histogram
./latency_histogram 1000000
```

## Files
| File | Purpose |
|------|---------|
| `orderbook.cpp` | Minimal v1 matching engine |
| `orderbook_v2.cpp` | v2 with cancel/modify and status tracking |
| `benchmark_orderbook.cpp` | Throughput benchmark across order counts |
| `latency_histogram.cpp` | Per-order latency percentiles (HFT-relevant) |
