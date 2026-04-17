# Smart Order Router (SOR) / Inteligentny Router Zleceń

Routes orders to the best execution venue based on price, latency, and liquidity.
*Kieruje zlecenia do najlepszej giełdy wykonania w oparciu o cenę, opóźnienie i płynność.*

## Performance Comparison / Porównanie wydajności

| Metric | Python | C++ |
|--------|--------|-----|
| **Throughput** | ~200K routes/sec | **9.7M routes/sec** (48x faster) |
| **Latency (p50)** | ~4,000 ns | **70 ns** |
| **Latency (p99)** | ~8,000 ns | **150 ns** |

## Routing Strategies / Strategie Routingu

- **BEST_PRICE**: Route to venue with best bid/ask (ties broken by fee, then latency)
*Kieruje do giełdy z najlepszą ceną kupna/sprzedaży (remisy rozstrzygane przez opłatę, następnie opóźnienie).*

- **LOWEST_LATENCY**: Route to fastest venue (for latency-sensitive strategies)
*Kieruje do najszybszej giełdy (dla strategii wrażliwych na opóźnienia).*

- **SPLIT**: Split large orders across venues proportional to available liquidity
*Dzieli duże zlecenia między giełdy proporcjonalnie do dostępnej płynności.*

## Venues

Each venue has: name, round-trip latency (ns), fee per share, live bid/ask quotes.
Negative fees represent maker rebates (common on electronic exchanges).

## Pipeline / Potok

```
Strategy → **Router** → Risk → OMS → Exchange
```

## Files / Pliki

| File | Description |
|------|-------------|
| `smart_router.py` | Python implementation (reference, with beginner comments) |
| `smart_router.hpp` | C++ header-only implementation |
| `router_demo.cpp` | C++ demo with 12 unit tests + throughput benchmark |

## Run / Uruchomienie

```bash
# Python
python3 router/smart_router.py
python3 tests/test_router.py

# C++ (build + run)
make build
./router/router_demo              # tests + benchmark (1M routes)
./router/router_demo 5000000      # 5M routes
```
