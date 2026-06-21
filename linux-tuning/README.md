# HFT Linux Tuning Lab

Baseline Linux kernel tuning for low-latency trading infrastructure.

## Environment
- OS: Red Hat Enterprise Linux 10.1 (Coughlan)
- Kernel: 6.12.0-124.8.1.el10_1.x86_64
- VM: VirtualBox (2 vCPUs, 4GB RAM)
- Boot params: isolcpus=1 nohz_full=1 rcu_nocbs=1

## Optimizations Applied

| # | Optimization | Kernel Parameter |
|---|---|---|
| 1 | Disable swap | vm.swappiness=0 |
| 2 | Network buffers | net.core.rmem_max=16777216 |
| 3 | Disable background services | systemctl stop dnf-makecache.timer |
| 4 | Hugepages (1GB) | vm.nr_hugepages=512 |
| 5 | TCP low latency | tcp_nodelay=1, tcp_timestamps=0 |
| 6 | IRQ affinity | enp0s3 IRQs pinned to CPU0 |
| 7 | FIFO scheduler | chrt -f -p 80 |

## Benchmark Results (cyclictest on tuned VM)

| Metric | Result |
|---|---|
| Min latency | 21 µs |
| Avg latency | 3878 µs |
| Max latency | 2,464,055 µs |

Note: High max/avg caused by VirtualBox hypervisor scheduling. Production HFT uses bare metal with PREEMPT_RT for sub-10µs p99.

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
