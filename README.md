# HFT Infrastructure Lab

![Tests](https://github.com/kanar11/hft-infra-lab/actions/workflows/tests.yml/badge.svg)

Complete low-latency infrastructure lab for HFT systems — kernel tuning, networking, order management, and monitoring.

## Performance Highlights (Red Hat EL10, VirtualBox 2-core VM)
- Order book matching: **17.8M orders/sec** (C++, p50=50ns, p99=130ns)
- ITCH parser: **~1M msg/sec** (Python, ~1000ns/msg)
- OUCH encoding: **1.7M msg/sec** (Python)
- Lock-free SPSC queue: **17.6M msg/sec** (C++, 10M messages benchmarked)
- Cache latency: L1=1.6ns, L2=4.3ns, L3=154ns, RAM=100-110ns
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
- Boot params: isolcpus=1 nohz_ful