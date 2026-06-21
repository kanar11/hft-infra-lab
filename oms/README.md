# Order Management System

Full order lifecycle management with pre-trade risk checks.

## Performance

| Metric | C++ |
|--------|-----|
| **Throughput** | **11.6M orders/sec** |
| **Latency (p50)** | **60 ns** |
| **Latency (p99)** | **121 ns** |
| **Price handling** | Fixed-point int64 (no FP on hot path) |
| **Order lookup** | unordered_map (hash, O(1)) |

## Features

- Order lifecycle: NEW → SENT → FILLED / PARTIAL / CANCELLED / REJECTED
- Pre-trade risk checks: order value limit + position limit
- Position tracking with average cost basis
- Realized P&L calculation
- Fixed-point pricing (C++): `$150.25 → 1502500` — avoids floating-point errors

## Why C++ Here

The OMS is on the critical path — it processes every order between the strategy
and the exchange. In production HFT, the OMS must:
- Validate orders in < 100ns
- Track positions without locking
- Never allocate memory on the hot path

## Files

| File | Description |
|------|-------------|
| `oms.hpp` | C++ header-only implementation (production-style) |
| `oms_demo.cpp` | C++ demo with 17 unit tests + throughput benchmark |

## Run

```bash
# C++ (build + run)
make build
./oms/oms_demo            # tests + benchmark (default: 1M orders)
./oms/oms_demo 5000000    # benchmark with 5M orders
```

## Risk Checks

Pre-trade checks enforce:
- **Max order value**: rejects if `price × quantity > limit`
- **Max position size**: rejects if projected position exceeds limit

```
Strategy signal → Risk Check → [ACCEPT/REJECT] → OMS → Exchange
```
