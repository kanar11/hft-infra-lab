# HFT Infrastructure Lab

Complete low-latency infrastructure lab for HFT systems.

## Modules

### kernel-config/
Server hardening: hugepages, CPU isolation, sysctl tuning, IRQ affinity.

### linux-tuning/
Benchmarking and monitoring scripts for baseline vs tuned kernel performance.

### network-latency/
Real-time network latency measurement and jitter analysis.

## Environment
- OS: Ubuntu 24.04 LTS Server
- VM: VirtualBox (2 CPU, 4GB RAM)
- Kernel params: isolcpus=1, nohz_full=1, rcu_nocbs=1
