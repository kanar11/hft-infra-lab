# Infrastructure Monitor

Real-time system monitoring for HFT infrastructure — parses the /proc filesystem for CPU, memory, and network stats.

## Metrics Tracked
- CPU usage per core (user, system, idle, iowait)
- Context switches per second
- Memory usage (total, free, available, hugepages)
- Network throughput (bytes/sec, packets per interface)
- IRQ count on isolated CPUs
- NUMA node status

## Alerts
- Memory usage > configurable threshold
- Context switches > threshold
- Network errors > threshold

## Files
| File | Description |
|------|-------------|
| `infra_monitor.hpp` | C++ header-only — /proc parser, AlertThresholds, 8.6M parse/sec |
| `monitor_demo.cpp` | 26 unit tests (mock /proc data) + throughput benchmark |

## Performance

| Metric | Result |
|--------|--------|
| /proc/stat parse | **8.6M/sec** (90ns p50) |

## Run
```bash
make build
./monitoring/monitor_demo 50000    # benchmark
./monitoring/monitor_demo 0        # tests only
```
