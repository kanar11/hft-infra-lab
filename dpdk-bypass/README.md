# DPDK Kernel Bypass Simulator / Symulator DPDK Kernel Bypass

Simulates DPDK-style poll mode driver for low-latency packet processing.
*Symuluje DPDK-style sterownik w trybie sondowania do przetwarzania pakietów o niskich opóźnieniach.*

## Concepts Demonstrated / Demonstrujące koncepcje
- **Poll Mode Driver**: busy-wait loop vs interrupt-driven I/O
- **CPU Pinning**: process pinned to isolated CPU core
- **Zero-interrupt processing**: no kernel scheduling overhead

## Performance / Wydajność
- Polling: ~80μs avg latency
- Interrupt-driven: ~450μs avg latency
- ~5.6x improvement with polling

## Run / Uruchomienie
Terminal 1: `python3 kernel_bypass_sim.py`
Terminal 2: `python3 ../multicast/mc_sender.py`
