# Lock-Free SPSC Queue

Single Producer Single Consumer lock-free queue for inter-thread communication in HFT.

*Kolejka bez blokad Single Producer Single Consumer do komunikacji między wątkami w HFT.*

## Why Lock-Free / Dlaczego bez blokad
- Mutexes cause thread blocking = unpredictable latency
- Lock-free uses atomic operations = no waiting
- SPSC pattern: market data thread -> trading logic thread

*Muteksy powodują blokowanie wątków = nieprzewidywalne opóźnienia*
*Bez blokad stosuje operacje atomowe = bez czekania*
*Wzorzec SPSC: wątek danych rynkowych -> wątek logiki handlowej*

## Design / Projekt
- Power-of-2 buffer size for bitwise modulo
- alignas(64) prevents false sharing between cache lines
- memory_order_acquire/release for correct ordering

*Rozmiar bufora potęgi 2 do modulo bitowego*
*alignas(64) zapobiega fałszywemu udostępnianiu między liniami pamięci podręcznej*
*memory_order_acquire/release dla prawidłowego porządkowania*

## Performance (Red Hat EL10, VirtualBox 2-core VM)
- 10M messages processed
- 17.6M msg/sec throughput
- 762ns avg inter-thread latency
- Production target: 50-100M msg/sec on dedicated hardware

## Build & Run / Budowanie i uruchamianie
```bash
g++ -O2 -pthread -o spsc_queue spsc_queue.cpp
./spsc_queue
```
