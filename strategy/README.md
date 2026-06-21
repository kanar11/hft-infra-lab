# Mean Reversion Strategy

Simple mean reversion strategy for the HFT pipeline demo.

## Performance

| Metric | C++ |
|--------|-----|
| **Throughput** | **8.0M ticks/sec** |
| **Latency (p50)** | **100 ns** |
| **Latency (p99)** | **121 ns** |

## Logic

- Tracks a rolling Simple Moving Average (SMA) per stock
- When price deviates from SMA by more than a threshold:
  - Price > SMA + 0.1% → SELL (overpriced, expect drop)
  - Price < SMA - 0.1% → BUY (underpriced, expect rise)
  - Otherwise → HOLD (no signal)

## Parameters

- Window: 20 ticks (SMA lookback)
- Threshold: 0.1% deviation from SMA
- Order size: 100 shares per signal

## Files

| File | Description |
|------|-------------|
| `mean_reversion.hpp` | C++ header-only implementation |
| `strategy_demo.cpp` | C++ demo with 44 unit tests + throughput benchmark |

## Run

```bash
# C++ (build + run)
make build
./strategy/strategy_demo              # tests + benchmark (1M ticks)
./strategy/strategy_demo 5000000      # 5M ticks
```
