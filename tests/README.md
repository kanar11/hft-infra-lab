# Tests & Benchmarks

All tests are C++ with built-in assertions. Each module demo includes unit tests that run before benchmarks.

## Test Suite (200+ assertions)

| Binary | Module | Tests |
|--------|--------|-------|
| `oms/oms_demo` | Order Management System | submit, risk checks, fill, cancel, positions, P&L |
| `risk/risk_demo` | Risk Manager | limits, circuit breaker, kill switch, drawdown, rate limit |
| `router/router_demo` | Smart Order Router | best price, latency, split, fees, inactive venues |
| `logger/logger_demo` | Trade Logger | log, filter, trail, summary, ring buffer, latency stats |
| `strategy/strategy_demo` | Mean Reversion Strategy | signals, SMA, threshold, hold detection |
| `fix-protocol/fix_demo` | FIX 4.2 Parser | order types, malformed tags, encoding |
| `ouch-protocol/ouch_demo` | OUCH 4.2 Protocol | encoding, parsing, truncation, precision |
| `dpdk-bypass/dpdk_demo` | DPDK Bypass | poll vs interrupt, packet processing |
| `monitoring/monitor_demo` | Infrastructure Monitor | /proc parsing, alerts, thresholds |
| `multicast/multicast_demo` | Multicast | serialize, deserialize, market data |
| `simulator/sim_demo` | Market Simulator | generator, parser integration, full pipeline |
| `tests/test_all` | Integration | cross-module pipeline, end-to-end validation |

## Run

```bash
make build      # compile everything
make test       # run all unit tests
make benchmark  # run throughput benchmarks
make simulate   # run full market simulation (4 modes)

# Individual module tests
./oms/oms_demo 0
./logger/logger_demo 0
./simulator/sim_demo 0
```

## CI

Tests run automatically on every push via GitHub Actions (`.github/workflows/tests.yml`).
All tests are pure C++ — no Python required.
