# How I Tuned Linux for Low-Latency Trading

## Why This Matters

In high-frequency trading, microseconds decide who profits and who loses. The difference between a well-tuned and default Linux server can be 10-100x in latency.

## The Environment

- Ubuntu 24.04 LTS Server on VirtualBox
- 2 vCPUs, 4GB RAM, 25GB disk
- Kernel: 6.8.0-107-generic

## Step 1: Hugepages

Default Linux uses 4KB memory pages. The CPU has a TLB that caches page mappings. With 4KB pages, the TLB constantly misses — each miss costs ~10-30ns.

I reserved 512 hugepages (2MB each = 1GB total). Result: fewer TLB misses, more predictable memory access.

## Step 2: CPU Isolation

By default, the scheduler moves processes between CPUs freely, causing cache pollution.

I isolated CPU 1: isolcpus=1 nohz_full=1 rcu_nocbs=1

Result: CPU 1 is completely clean for the trading process.

## Step 3: IRQ Affinity

I pinned all hardware interrupts to CPU 0 so CPU 1 receives zero interrupts.

## Step 4: Network Stack Tuning

16MB socket buffers, tcp_low_latency=1, swappiness=0 — everything stays in RAM.

## Step 5: Polling vs Interrupts

Poll mode: ~80us avg latency. Interrupt-driven: ~450us. Polling is 5.6x faster.

## Results

| Optimisation | Impact |
|-------------|--------|
| Hugepages | Fewer TLB misses |
| CPU isolation | Zero scheduler interference |
| IRQ affinity | Zero interrupts on trading core |
| Network tuning | No packet drops, lower latency |
| Poll mode | 5.6x latency reduction |

## What I Would Do Next

- Real DPDK with supported NIC
- NUMA-aware memory allocation
- Real-time kernel (PREEMPT_RT)
- Hardware timestamping with PTP

## Key Takeaway

Low-latency is not about faster hardware — it is about removing unnecessary work. The fastest code is the code that never runs.
