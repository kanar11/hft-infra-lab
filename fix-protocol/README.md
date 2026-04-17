# FIX 4.2 Protocol Parser

Parses Financial Information eXchange messages for order routing.
*Analizuje komunikaty Financial Information eXchange do routingu zamówień.*

## Performance Comparison / Porównanie wydajności

| Metric | Python | C++ |
|--------|--------|-----|
| **Throughput** | ~400K msg/sec | **5.5M msg/sec** (14x faster) |
| **Latency (p50)** | ~2,300 ns | **150 ns** |
| **Latency (p99)** | ~5,000 ns | **250 ns** |

## Supported Message Types / Obsługiwane typy wiadomości

- D = New Order
- G = Modify
- F = Cancel
- 8 = Execution Report
- 0 = Heartbeat

## Key Tags / Kluczowe znaczniki

- 35 = MsgType
- 55 = Symbol
- 54 = Side (1=Buy, 2=Sell)
- 44 = Price
- 38 = OrderQty

## Files / Pliki

| File | Description |
|------|-------------|
| `fix_parser.py` | Python implementation (reference, with beginner comments) |
| `fix_parser.hpp` | C++ header-only implementation |
| `fix_demo.cpp` | C++ demo with 27 unit tests + throughput benchmark |

## Run / Uruchomienie

```bash
# Python
python3 fix-protocol/fix_parser.py
python3 tests/test_fix.py

# C++ (build + run)
make build
./fix-protocol/fix_demo              # tests + benchmark (1M parses)
./fix-protocol/fix_demo 5000000      # 5M parses
```
