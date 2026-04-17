# Tests & Benchmarks / Testy i Wzorce Wydajności

Unit tests and performance benchmarks for all HFT modules.
*Testy jednostkowe i wzorce wydajności dla wszystkich modułów HFT.*

## Test Suite (55 tests)

| File | Module | Tests |
|------|--------|-------|
| `test_oms.py` | Order Management System | 10 (submit, risk checks, fill, cancel, positions, P&L accuracy) |
| `test_itch.py` | ITCH 5.0 Parser | 11 (add, sell, delete, trade, replace, executed, cancelled, system event, stock directory, add_mpid, speed) |
| `test_ouch.py` | OUCH 4.2 Protocol | 7 (encoding, parsing, truncation, precision) |
| `test_fix.py` | FIX 4.2 Parser | 7 (order types, malformed tags, speed) |
| `test_router.py` | Smart Order Router | 10 (best price, latency, split, fees, inactive venues, speed) |
| `test_risk.py` | Risk Manager | 10 (limits, circuit breaker, kill switch, drawdown, rate limit, speed) |

## Run Tests
```bash
# All tests (from project root)
make test

# Individual test files
python3 tests/test_oms.py
python3 tests/test_itch.py
python3 tests/test_ouch.py
python3 tests/test_fix.py
python3 tests/test_router.py
python3 tests/test_risk.py
```

## Benchmarks
```bash
# Python benchmarks (ITCH throughput, OMS throughput)
python3 tests/benchmark.py

# C++ benchmarks (run from project root)
make benchmark
```

## CI
Tests run automatically on every push via GitHub Actions. See `.github/workflows/tests.yml`.
