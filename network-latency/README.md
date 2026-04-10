# Network Latency Monitor

Real-time network latency measurement tool for HFT infrastructure monitoring.

## Environment
- OS: Ubuntu 24.04 LTS (WSL2)
- CPU: AMD Ryzen 7 5700X
- RAM: 16GB

## What it measures

| # | Metric | Tool |
|---|---|---|
| 1 | ICMP latency | ping |
| 2 | TCP latency | hping3 |
| 3 | Jitter | ping + awk |

## Usage
```bash
chmod +x net_latency.sh
./net_latency.sh 8.8.8.8
```

## Why This Matters
In HFT, network latency directly impacts order execution speed.
- **ICMP latency** — baseline round-trip time
- **TCP latency** — real trading protocol latency
- **Jitter** — latency variance, critical for predictable execution
