# FIX 4.2 Protocol Parser

Parses Financial Information eXchange messages for order routing.
*Analizuje komunikaty Financial Information eXchange do routingu zamówień.*

## Performance 

| Metric | C++ |
|--------|-----|
| **Throughput** | **5.5M msg/sec** |
| **Latency (p50)** | **150 ns** |
| **Latency (p99)** | **250 ns** |

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
| `fix_parser.hpp` | C++ header-only implementation |
| `fix_demo.cpp` | C++ demo with 27 unit tests + throughput benchmark |

## Run 

```bash
# C++ (build + run)
make build
./fix-protocol/fix_demo              # tests + benchmark (1M parses)
./fix-protocol/fix_demo 5000000      # 5M parses
```
