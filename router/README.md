# Smart Order Router (SOR)

Routes orders to the best execution venue based on price, latency, and liquidity.

## Performance

| Metric | C++ |
|--------|-----|
| **Throughput** | **9.7M routes/sec** |
| **Latency (p50)** | **70 ns** |
| **Latency (p99)** | **150 ns** |

## Routing Strategies

- **BEST_PRICE**: Route to venue with best bid/ask (ties broken by fee, then latency)
- **LOWEST_LATENCY**: Route to fastest venue (for latency-sensitive strategies)
- **SPLIT**: Split large orders across venues proportional to available liquidity

## Venues

Each venue has: name, round-trip latency (ns), fee per share, live bid/ask quotes.
Negative fees represent maker rebates (common on electronic exchanges).

## Pipeline

```
Strategy → **Router** → Risk → OMS → Exchange
```

## Files

| File | Description |
|------|-------------|
| `smart_router.hpp` | C++ header-only implementation |
| `router_demo.cpp` | C++ demo with 12 unit tests + throughput benchmark |

## Run

```bash
# C++ (build + run)
make build
./router/router_demo              # tests + benchmark (1M routes)
./router/router_demo 5000000      # 5M routes
```
