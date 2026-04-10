# HFT Linux Tuning Lab

Baseline Linux kernel tuning for low-latency trading infrastructure.

## Environment
- OS: Ubuntu 24.04 LTS
- Kernel: 6.6.87.2-microsoft-standard-WSL2
- CPU: AMD Ryzen 7 5700X (16 logical cores)
- RAM: 16GB

## Optimizations Applied

| # | Optimization | Kernel Parameter |
|---|---|---|
| 1 | Disable swap | vm.swappiness=0 |
| 2 | Network buffers | net.core.rmem_max=16777216 |
| 3 | Disable auto-upgrades | systemctl stop unattended-upgrades |
| 4 | Hugepages (1GB) | vm.nr_hugepages=512 |
| 5 | TCP low latency | tcp_nodelay=1, tcp_timestamps=0 |
| 6 | IRQ affinity | eth0 IRQs pinned to CPU0 |
| 7 | FIFO scheduler | chrt -f -p 80 |

## Benchmark Results (cyclictest)

| Metric | Before | After |
|---|---|---|
| Min latency | 4 µs | 3 µs |
| Avg latency | 12 µs | 9 µs |

## Usage
```bash
sudo ./hft_tuning.sh
sudo cyclictest -l 10000 -t 1 -p 80 -i 200
```

## Why This Matters
In HFT, microseconds determine whether an order is filled before competitors.
- **Hugepages** eliminate TLB misses on critical memory paths
- **IRQ affinity** prevents network interrupts from disturbing the trading thread
- **FIFO scheduler** gives the trading process guaranteed CPU time
- **TCP tuning** reduces kernel network stack latency
