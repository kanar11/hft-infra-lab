# NASDAQ ITCH 5.0 Binary Protocol Parser

Parses binary market data feed messages from NASDAQ.

## Performance

| Metric | C++ (`itch_parser.hpp`) |
|--------|------------------------|
| Throughput | **60M msg/sec** |
| Latency | **16 ns/msg** (p99=50ns) |

## Supported Messages (9 types)

| Code | Type | Size | Description |
|------|------|------|---|
| A | ADD_ORDER | 34 B | New limit order |
| F | ADD_ORDER_MPID | 38 B | New order with Market Participant ID |
| D | DELETE_ORDER | 17 B | Cancel order |
| U | REPLACE_ORDER | 33 B | Modify order (new price/size) |
| E | ORDER_EXECUTED | 29 B | Order filled |
| C | ORDER_CANCELLED | 21 B | Partial cancel |
| P | TRADE | 42 B | Matched trade |
| S | SYSTEM_EVENT | 10 B | Market open/close/halt |
| R | STOCK_DIRECTORY | 18 B | Stock metadata |

## ITCH vs OUCH vs FIX

- **ITCH**: binary, receive market data FROM the exchange (read-only)
- **OUCH**: binary, send orders TO the exchange (write-only)
- **FIX**: text-based, bidirectional communication

## Files

| File | Description |
|------|---|
| `itch_parser.hpp` | C++ header-only parser — zero-alloc, inline byte-swap |
| `benchmark_itch.cpp` | C++ benchmark — 10M messages, throughput + latency percentiles |

## Run

```bash
# C++ benchmark
make build
./itch-parser/benchmark_itch
```
