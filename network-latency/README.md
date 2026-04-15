# Network Latency Monitor

Real-time network latency measurement tool for HFT infrastructure monitoring.

## Environment
- OS: Red Hat Enterprise Linux 10.1 (Coughlan)
- VM: VirtualBox (2 vCPUs, 4GB RAM)

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

## Why This Matters / Dlaczego to ma znaczenie
In HFT, network latency directly impacts order execution speed.
- **ICMP latency** — baseline round-trip time
- **TCP latency** — real trading protocol latency
- **Jitter** — latency variance, critical for predictable execution

*W HFT opóźnienie sieci bezpośrednio wpływa na szybkość wykonania zlecenia.*
*- **Opóźnienie ICMP** — bazowy czas podróży w obie strony*
*- **Opóźnienie TCP** — opóźnienie rzeczywistego protokołu handlowego*
*- **Jitter** — wariancja opóźnienia, krytyczna dla przewidywalnego wykonania*
