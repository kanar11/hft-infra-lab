# Python bindings (`pyhft`)

`pybind11` wrappers exposing the lab's C++ core to Python — the typical
"hot path in C++, research/backtesting in Python" pattern used by every
real HFT shop.

## What's wrapped

| C++                    | Python                          |
|------------------------|---------------------------------|
| `Side` enum            | `pyhft.Side.BUY` / `.SELL`      |
| `OrderStatus` enum     | `pyhft.OrderStatus.FILLED` etc. |
| `Order` struct         | `pyhft.Order` (read-only)       |
| `Position` struct      | `pyhft.Position` (read-only)    |
| `OMS`                  | `pyhft.OMS(...)`                |
| `RiskLimits`           | `pyhft.RiskLimits()`            |
| `RiskCheckResult`      | `pyhft.RiskCheckResult`         |
| `RiskManager`          | `pyhft.RiskManager(...)`        |
| `FlatOrderBook<16384>` | `pyhft.FlatOrderBook()`         |

## Build

```bash
pip install pybind11 setuptools
python3 bindings/setup.py build_ext --inplace
```

That produces `pyhft.cpython-*.so` (Linux) or `.pyd` (Windows) at the
repo root. After that:

```bash
python3 bindings/demo.py
```

## Usage example

```python
import sys; sys.path.insert(0, '.')
import pyhft

oms = pyhft.OMS(max_position=1000, max_order_value=100_000.0)
order = oms.submit_order("AAPL", pyhft.Side.BUY, 150.25, 100)
oms.fill_order(order.order_id, 100, 150.25)

position = oms.get_position("AAPL")
print(position.net_qty, position.avg_price)  # 100, 1502500 (fixed-point)
```

## CI

A separate `python-bindings` job in `.github/workflows/tests.yml`
installs `python3-dev` + `pybind11` + `setuptools`, builds the
extension, then runs `bindings/demo.py` end-to-end.

## What's NOT wrapped (yet)

- `TradeLogger` / `LockfreeTradeLogger` / `MmapTradeLogger` — file I/O
  side-effects, less useful from a notebook than from a service.
- `MarketMaker`, `MeanReversionStrategy` — straightforward to add if
  you want to backtest strategies in Python while keeping the OMS in C++.
- `ITCHParser` / `FIXMessage` / `OUCH` — protocol parsers; useful for
  ingesting real captures from Python, but not in this initial cut.

Open an issue / send a PR if you need any of the above bound.
