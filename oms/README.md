# Order Management System / System zarządzania zleceniami

Full order lifecycle management with pre-trade risk checks.

*Pełne zarządzanie cyklem życia zlecenia z kontrolami ryzyka przed transakcją.*

## Performance / Wydajność

| Metric | C++ |
|--------|-----|
| **Throughput** | **11.6M orders/sec** |
| **Latency (p50)** | **60 ns** |
| **Latency (p99)** | **121 ns** |
| **Price handling** | Fixed-point int64 (no FP on hot path) |
| **Order lookup** | unordered_map (hash, O(1)) |

## Features / Funkcje

- Order lifecycle: NEW → SENT → FILLED / PARTIAL / CANCELLED / REJECTED
- Pre-trade risk checks: order value limit + position limit
- Position tracking with average cost basis
- Realized P&L calculation
- Fixed-point pricing (C++): `$150.25 → 1502500` — avoids floating-point errors

## Why C++ Here / Dlaczego C++ tutaj

The OMS is on the critical path — it processes every order between the strategy
and the exchange. In production HFT, the OMS must:
- Validate orders in < 100ns
- Track positions without locking
- Never allocate memory on the hot path

*OMS jest na krytycznej ścieżce — przetwarza każde zlecenie między strategią
a giełdą. W produkcyjnym HFT, OMS musi: walidować zlecenia w < 100ns,
śledzić pozycje bez blokowania, nigdy nie alokować pamięci na gorącej ścieżce.*

## Files / Pliki

| File | Description |
|------|-------------|
| `oms.hpp` | C++ header-only implementation (production-style) |
| `oms_demo.cpp` | C++ demo with 17 unit tests + throughput benchmark |

## Run / Uruchomienie

```bash
# C++ (build + run)
make build
./oms/oms_demo            # tests + benchmark (default: 1M orders)
./oms/oms_demo 5000000    # benchmark with 5M orders
```

## Risk Checks / Kontrole ryzyka

Pre-trade checks enforce:
- **Max order value**: rejects if `price × quantity > limit`
- **Max position size**: rejects if projected position exceeds limit

```
Strategy signal → Risk Check → [ACCEPT/REJECT] → OMS → Exchange
```
