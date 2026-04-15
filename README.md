# HFT Infrastructure Lab

Complete low-latency infrastructure lab for HFT systems — kernel tuning, networking, order management, and monitoring.

## Performance Highlights
- Order book matching: **23M orders/sec** (C++, 2-core VM)
- ITCH parser: **258K msg/sec** (Python)
- OUCH encoding: **1.7M msg/sec** (Python)
- Lock-free queue: **10M messages** benchmarked
- DPDK poll mode: **5.6x latency reduction** vs interrupts

## Modules

| Module | Description | Language |
|--------|------------|----------|
| kernel-config/ | Hugepages, CPU isolation, sysctl, IRQ affinity | Bash |
| linux-tuning/ | Baseline vs tuned kernel benchmarks | Bash |
| network-latency/ | Network latency and jitter measurement | Bash |
| multicast/ | Market data feed sender/receiver with latency | Python |
| orderbook/ | Matching engine with cancel, modify, benchmarks | C++ |
| fix-protocol/ | FIX 4.2 message parser | Python |
| itch_parser/ | NASDAQ ITCH 5.0 binary protocol parser | Python |
| ouch-protocol/ | NASDAQ OUCH 4.2 order entry protocol | Python |
| dpdk-bypass/ | Kernel bypass simulator with poll mode driver | Python |
| lockfree/ | Lock-free SPSC queue for inter-thread comms | C++ |
| oms/ | Order Management System with risk checks, P&L | Python |
| monitoring/ | Real-time infra monitor with alerts | Python |
| tests/ | Unit tests (26) and benchmarks | Python |
| docs/ | Technical write-up on Linux tuning | Markdown |

## Quick Start
```bash
make build      # compile all C++ modules (orderbook, lockfree, cache_latency)
make test       # run all unit tests (26/26: OMS, ITCH, OUCH, FIX)
make benchmark  # run performance benchmarks (orderbook, ITCH, OMS, latency histogram)
```

## Environment
- OS: Red Hat Enterprise Linux 10.1 (Coughlan) 
- VM: VirtualBox (2 CPU, 4GB RAM, 40GB disk)
- Kernel: 6.12.0-124.8.1.el10_1.x86_64
- Boot params: isolcpus=1 nohz_full=1 rcu_nocbs=1
- Hugepages: 512 x 2MB (1GB reserved)
- Swappiness: 0
