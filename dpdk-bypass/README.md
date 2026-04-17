# DPDK Kernel Bypass Simulator / Symulator DPDK Kernel Bypass

Simulates DPDK-style poll mode driver for low-latency packet processing.
*Symuluje DPDK-style sterownik w trybie sondowania do przetwarzania pakietów o niskich opóźnieniach.*

## Concepts Demonstrated / Demonstrujące koncepcje
- **Poll Mode Driver**: busy-wait loop vs interrupt-driven I/O
- **CPU Pinning**: process pinned to isolated CPU core
- **Zero-interrupt processing**: no kernel scheduling overhead
- **Lock-free SPSC ring buffer**: power-of-2 masking, cache-line aligned packets

## Files / Pliki
| File | Description |
|------|-------------|
| `kernel_bypass_sim.py` | Python simulator — poll vs interrupt benchmark |
| `kernel_bypass_sim.hpp` | C++ header-only — lock-free PacketRing, alignas(64) SimPacket |
| `dpdk_demo.cpp` | 17 unit tests + throughput benchmark |

## Performance / Wydajność

| Metric | Python | C++ |
|--------|--------|-----|
| Poll mode | ~80μs avg | **19.9M pkt/sec** (50ns/pkt) |
| Interrupt mode | ~450μs avg | 8.4M pkt/sec (119ns/pkt) |
| Speedup | ~5.6x | **2.3x** |

## Run / Uruchomienie
```bash
# C++ (from project root)
make build
./dpdk-bypass/dpdk_demo 50000

# Python
python3 kernel_bypass_sim.py
```
