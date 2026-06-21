# Risk Manager

Standalone pre-trade risk engine with circuit breaker and kill switch.

## Performance

| Metric | C++ |
|--------|-----|
| **Throughput** | **7.9M checks/sec** |
| **Latency (p50)** | **91 ns** |
| **Latency (p99)** | **140 ns** |

## Features

- **Per-symbol position limit** — max shares in one instrument
- **Portfolio exposure limit** — max total absolute exposure
- **Daily P&L loss limit** — circuit breaker triggers kill switch
- **Drawdown limit** — max % drop from peak P&L
- **Order rate limiting** — max orders per second
- **Order value limit** — max notional value per order
- **Kill switch** — halts all trading instantly (manual or automatic)

## Pipeline

```
Strategy → Router → **Risk Manager** → OMS → Exchange
```

Every order MUST pass all 7 risk checks before reaching the OMS.

## Files

| File | Description |
|------|-------------|
| `risk_manager.hpp` | C++ header-only implementation |
| `risk_demo.cpp` | C++ demo with 14 unit tests + throughput benchmark |

## Run

```bash
# C++ (build + run)
make build
./risk/risk_demo              # tests + benchmark (1M checks)
./risk/risk_demo 5000000      # 5M checks
```
