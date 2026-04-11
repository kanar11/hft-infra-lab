- 16MB socket buffers prevent packet drops under burst
- `tcp_low_latency=1` disables Nagle-like batching
- `swappiness=0` keeps everything in RAM — disk is 1000x slower

## Step 5: Polling vs Interrupts

I built a DPDK kernel bypass simulator comparing interrupt-driven I/O vs poll mode:

| Mode | Avg Latency |
|------|------------|
| Interrupt-driven | ~450 μs |
| Poll mode (busy-wait) | ~80 μs |

Polling burns 100% CPU but responds instantly. In HFT, that CPU cost is worth it.

## Results Summary

| Optimisation | Impact |
|-------------|--------|
| Hugepages | Fewer TLB misses, predictable memory access |
| CPU isolation | Zero scheduler interference on trading core |
| IRQ affinity | Zero interrupts on isolated CPU |
| Network tuning | No packet drops, lower TCP latency |
| Poll mode | ~5.6x latency reduction vs interrupts |

## What I Would Do Next

- Real DPDK with supported NIC for true kernel bypass
- NUMA-aware memory allocation on multi-socket server
- Real-time kernel (PREEMPT_RT) for deterministic scheduling
- Hardware timestamping with PTP for nanosecond-accurate timing

## Key Takeaway

Low-latency is not about faster hardware — it is about removing unnecessary work. Every optimisation above removes something: TLB misses, scheduler decisions, interrupts, kernel overhead. The fastest code is the code that never runs.
