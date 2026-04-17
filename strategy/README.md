# Mean Reversion Strategy / Strategia Powrotu do Średniej

Simple mean reversion strategy for the HFT pipeline demo.
*Prosta strategia powrotu do średniej dla demonstracji potoku HFT.*

## Performance Comparison / Porównanie wydajności

| Metric | Python | C++ |
|--------|--------|-----|
| **Throughput** | ~430K ticks/sec | **8.0M ticks/sec** (19x faster) |
| **Latency (p50)** | ~2,300 ns | **100 ns** |
| **Latency (p99)** | ~5,000 ns | **121 ns** |

## Logic / Logika

- Tracks a rolling Simple Moving Average (SMA) per stock
- When price deviates from SMA by more than a threshold:
  - Price > SMA + 0.1% → SELL (overpriced, expect drop)
  - Price < SMA - 0.1% → BUY (underpriced, expect rise)
  - Otherwise → HOLD (no signal)

*Śledzi toczącą się prostą średnią ruchomą (SMA) na akcję. Gdy cena odbiega od SMA o więcej niż próg: Cena > SMA + 0,1% → SPRZEDAJ. Cena < SMA - 0,1% → KUP. W przeciwnym razie → CZEKAJ.*

## Parameters / Parametry

- Window: 20 ticks (SMA lookback)
- Threshold: 0.1% deviation from SMA
- Order size: 100 shares per signal

## Files / Pliki

| File | Description |
|------|-------------|
| `mean_reversion.py` | Python implementation (reference, with beginner comments) |
| `mean_reversion.hpp` | C++ header-only implementation |
| `strategy_demo.cpp` | C++ demo with 44 unit tests + throughput benchmark |

## Run / Uruchomienie

```bash
# Python
python3 strategy/mean_reversion.py
python3 simulator/market_sim.py 10000 --strategy

# C++ (build + run)
make build
./strategy/strategy_demo              # tests + benchmark (1M ticks)
./strategy/strategy_demo 5000000      # 5M ticks
```
