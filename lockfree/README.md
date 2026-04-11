# Lock-Free SPSC Queue

Single Producer Single Consumer lock-free queue for inter-thread communication in HFT.

## Why Lock-Free
- Mutexes cause thread blocking = unpredictable latency
- Lock-free uses atomic operations = no waiting
- SPSC pattern: market data thread -> trading logic thread

## Design
- Power-of-2 buffer size for bitwise modulo
- alignas(64) prevents false sharing between cache lines
- memory_order_acquire/release for correct ordering

## Performance
- 10M messages processed
- ~1.5M msg/sec on 2-core VM
- Production target: 50-100M msg/sec on dedicated hardware

## Build & Run
```bash
g++ -O2 -pthread -o spsc_queue spsc_queue.cpp
./spsc_queue
```
