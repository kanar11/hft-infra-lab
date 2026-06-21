# Trade Logger / Audit Trail

Thread-safe trade event logger with nanosecond timestamps.

## Performance

| Metric | C++ |
|--------|-----|
| **Throughput** | **14.3M events/sec** |
| **Latency (p50)** | **41 ns** |
| **Latency (p99)** | **60 ns** |

## Why This Exists

Every HFT firm must keep a complete audit trail of all trading activity.
Regulators (SEC, MiFID II) require that every order, fill, cancel, and risk
rejection is logged with precise timestamps and can be replayed later.

## Event Types

| Event | Description |
|-------|-------------|
| `ORDER_SUBMIT` | Strategy sends order |
| `RISK_ACCEPT` | Risk manager approved |
| `RISK_REJECT` | Risk manager blocked |
| `ORDER_FILL` | Exchange filled order |
| `ORDER_PARTIAL` | Partial fill received |
| `ORDER_CANCEL` | Order cancelled |
| `KILL_SWITCH` | Emergency stop |
| `SYSTEM_START` | Session opened |
| `SYSTEM_STOP` | Session closed |

## Files

| File | Description |
|------|-------------|
| `trade_logger.hpp` | C++ header-only implementation |
| `logger_demo.cpp` | C++ demo with 30 unit tests + throughput benchmark |

## Run

```bash
# C++ (build + run)
make build
./logger/logger_demo              # tests + benchmark (500K events)
./logger/logger_demo 2000000      # 2M events
```

## In Real HFT

| This Lab | Production |
|----------|------------|
| std::vector + mutex | Lock-free ring buffer in shared memory |
| `std::chrono::steady_clock` | Hardware TSC (RDTSC instruction) |
| CSV file output | Binary log + async flush to SSD |
| Single process | Separate logger process (no latency on hot path) |
