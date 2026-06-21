# NASDAQ OUCH 4.2 Protocol

Binary order entry protocol for sending orders TO the exchange.

## Performance

| Metric | C++ |
|--------|-----|
| **Throughput** | **19.9M msg/sec** |
| **Latency (p50)** | **30 ns** |
| **Latency (p99)** | **40 ns** |

## Message Types

- O = Enter Order (buy/sell with price, qty, TIF)
- X = Cancel Order
- U = Replace Order (modify price/qty)

## Response Types

- A = Accepted
- C = Cancelled
- E = Executed

## ITCH vs OUCH vs FIX

- ITCH: binary, receive market data FROM the exchange
- OUCH: binary, send orders TO the exchange
- FIX: text-based, both directions (slower)

## Files

| File | Description |
|------|-------------|
| `ouch_protocol.hpp` | C++ header-only implementation |
| `ouch_demo.cpp` | C++ demo with 33 unit tests + throughput benchmark |

## Run

```bash
# C++ (build + run)
make build
./ouch-protocol/ouch_demo              # tests + benchmark (1M encodes)
./ouch-protocol/ouch_demo 5000000      # 5M encodes
```
