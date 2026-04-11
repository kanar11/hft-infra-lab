# Infrastructure Monitor

Real-time system monitoring for HFT infrastructure.

## Metrics Tracked
- Memory usage and hugepages status
- Context switches per second
- Network throughput (bytes/sec, packets)
- IRQ count on isolated CPUs
- NUMA node status

## Alerts
- Memory usage > 85%
- Interrupts on isolated CPU > threshold

## Output
- Console: real-time metrics every 5 seconds
- JSON log file for analysis

## Run
```bash
python3 infra_monitor.py
```
