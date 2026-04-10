# HFT Infrastructure Lab

Complete low-latency infrastructure lab for HFT systems — kernel tuning, networking, order management, and monitoring.

## Modules

### kernel-config/
Server hardening: hugepages (512x2MB), CPU isolation (isolcpus, nohz_full, rcu_nocbs), sysctl tuning, IRQ affinity.

### linux-tuning/
Benchmarking scripts: baseline vs tuned kernel performance comparison.

### network-latency/
Real-time network latency measurement and jitter analysis.

### multicast/
Market data feed simulation — multicast sender/receiver with latency measurement in microseconds.

### orderbook/
C++ matching engine with price-time priority, bid/ask management, and nanosecond latency tracking.

### fix-protocol/
FIX 4.2 protocol parser — parses NewOrder, Cancel, Execution messages with per-message latency.

### dpdk-bypass/
DPDK kernel bypass simulator — poll mode driver vs interrupt-driven I/O, CPU pinning, busy-wait packet processing.

### oms/
Order Management System — order lifecycle, position tracking, realized P&L, pre-trade risk checks (position limits, order value caps).

### monitoring/
Real-time infrastructure monitor — memory, context switches, network throughput, hugepages usage, IRQ alerts on isolated CPUs.

## Environment
- OS: Ubuntu 24.04 LTS Server
- VM: VirtualBox (2 CPU, 4GB RAM, 25GB disk)
- Kernel: 6.8.0-107-generic
- Boot params: isolcpus=1 nohz_full=1 rcu_nocbs=1
- Hugepages: 512 x 2MB (1GB reserved)
- Swappiness: 0
