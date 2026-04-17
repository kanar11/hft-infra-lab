# NASDAQ OUCH 4.2 Protocol

Binary order entry protocol for sending orders TO the exchange.
*Binarny protokół wprowadzania zleceń do wysyłania zleceń NA giełdę.*

## Performance Comparison / Porównanie wydajności

| Metric | Python | C++ |
|--------|--------|-----|
| **Throughput** | ~1.7M msg/sec | **19.9M msg/sec** (12x faster) |
| **Latency (p50)** | ~575 ns | **30 ns** |
| **Latency (p99)** | ~1,000 ns | **40 ns** |

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
| `ouch_sender.py` | Python implementation (reference, with beginner comments) |
| `ouch_protocol.hpp` | C++ header-only implementation |
| `ouch_demo.cpp` | C++ demo with 33 unit tests + throughput benchmark |

## Run / Uruchomienie

```bash
# Python
python3 ouch-protocol/ouch_sender.py
python3 tests/test_ouch.py

# C++ (build + run)
make build
./ouch-protocol/ouch_demo              # tests + benchmark (1M encodes)
./ouch-protocol/ouch_demo 5000000      # 5M encodes
```
