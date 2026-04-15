# Tests & Benchmarks

Unit tests and performance benchmarks for all HFT modules.

## Test Suite (26 tests)

| File | Module | Tests |
|------|--------|-------|
| `test_oms.py` | Order Management System | 7 (submit, risk checks, fill, cancel, positions) |
| `test_itch.py` | ITCH 5.0 Parser | 5 (add order, sell, delete, trade, speed) |
| `test_ouch.py` | OUCH 4.2 Protocol | 7 (encoding, parsing, truncation, precision) |
| `test_fix.py` | FIX 4.2 Parser | 7 (order types, malformed tags, speed) |

## Run Tests
```bash
# All tests (from project root)
make test

# Individual test files
python3 tests/test_oms.py
python3 tests/test_itch.py
python3 tests/test_ouch.py
python3 tests/test_fix.py
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
