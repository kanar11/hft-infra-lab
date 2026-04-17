# Infrastructure Monitor / Monitor infrastruktury

Real-time system monitoring for HFT infrastructure — parses /proc filesystem for CPU, memory, network stats.
*Monitorowanie systemu w czasie rzeczywistym dla infrastruktury HFT — parsuje /proc dla statystyk CPU, pamięci, sieci.*

## Metrics Tracked / Śledzone metryki
- CPU usage per core (user, system, idle, iowait)
- Context switches per second
- Memory usage (total, free, available, hugepages)
- Network throughput (bytes/sec, packets per interface)
- IRQ count on isolated CPUs
- NUMA node status

## Alerts / Alerty
- Memory usage > configurable threshold
- Context switches > threshold
- Network errors > threshold

## Files / Pliki
| File | Description |
|------|-------------|
| `infra_monitor.py` | Python monitor — real-time console output, JSON logging |
| `infra_monitor.hpp` | C++ header-only — /proc parser, AlertThresholds, 8.6M parse/sec |
| `monitor_demo.cpp` | 26 unit tests (mock /proc data) + throughput benchmark |

## Performance / Wydajność

| Metric | Python | C++ |
|--------|--------|-----|
| /proc/stat parse | ~100K/sec | **8.6M/sec** (90ns p50) |

## Run / Uruchomienie
```bash
# C++ (from project root)
make build
./monitoring/monitor_demo 50000

# Python (real-time monitoring)
python3 infra_monitor.py
```
