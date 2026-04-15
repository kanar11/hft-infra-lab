# Infrastructure Monitor / Monitor infrastruktury

Real-time system monitoring for HFT infrastructure.
*Monitorowanie systemu w czasie rzeczywistym dla infrastruktury HFT.*

## Metrics Tracked / Śledzone metryki
- Memory usage and hugepages status
- Context switches per second
- Network throughput (bytes/sec, packets)
- IRQ count on isolated CPUs
- NUMA node status

## Alerts / Alerty
- Memory usage > 85%
- Interrupts on isolated CPU > threshold

## Output / Wynik
- Console: real-time metrics every 5 seconds
- JSON log file for analysis

## Run / Uruchomienie
```bash
python3 infra_monitor.py
```
