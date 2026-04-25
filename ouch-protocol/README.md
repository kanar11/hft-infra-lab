# NASDAQ OUCH 4.2 Protocol

Binary order entry protocol for sending orders TO the exchange.
*Binarny protokół wprowadzania zleceń do wysyłania zleceń NA giełdę.*

## Performance / Wydajność

| Metric | C++ |
|--------|-----|
| **Throughput** | **19.9M msg/sec** |
| **Latency (p50)** | **30 ns** |
| **Latency (p99)** | **40 ns** |

## Message Types / Typy wiadomości

- O = Enter Order (buy/sell with price, qty, TIF)
- X = Cancel Order
- U = Replace Order (modify price/qty)

## Response Types / Typy odpowiedzi

- A = Accepted
- C = Cancelled
- E = Executed

## ITCH vs OUCH vs FIX

- ITCH: binary, receive market data FROM exchange
- OUCH: binary, send orders TO exchange
- FIX: text-based, both directions (slower)

## Files / Pliki

| File | Description |
|------|-------------|
| `ouch_protocol.hpp` | C++ header-only implementation |
| `ouch_demo.cpp` | C++ demo with 33 unit tests + throughput benchmark |

## Run / Uruchomienie

```bash
# C++ (build + run)
make build
./ouch-protocol/ouch_demo              # tests + benchmark (1M encodes)
./ouch-protocol/ouch_demo 5000000      # 5M encodes
```
