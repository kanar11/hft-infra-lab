# Multicast Market Data Feed / Kanał danych rynkowych Multicast

UDP multicast sender and receiver for simulating exchange market data feeds.
*Nadajnik i odbiornik UDP multicast do symulowania kanałów danych giełdowych.*

## Implementations / Implementacje

| | Python | C++ (`multicast.hpp`) |
|---|---|---|
| Throughput / Przepustowość | ~100K msg/sec | **23.2M msg/sec** |
| Latency / Opóźnienie | ~10 μs/msg | **20 ns/msg** (p50) |
| Wire format / Format sieciowy | Text ("SEQ=0 TS=...") | Binary (40 bytes, big-endian) |
| Use case / Zastosowanie | Testing, demo | Production hot path |

## Binary Wire Format / Binarny format sieciowy (40 bytes)

| Offset | Field | Type | Description / Opis |
|--------|-------|------|----|
| 0-7 | sequence | uint64 BE | Message sequence number / Numer sekwencji |
| 8-15 | timestamp_ns | uint64 BE | Send timestamp (nanoseconds) / Znacznik czasu |
| 16-23 | symbol | char[8] | Ticker, space-padded / Symbol akcji |
| 24-31 | price | int64 BE | Fixed-point (x10000) / Stałoprzecinkowa |
| 32-35 | quantity | uint32 BE | Order quantity / Ilość |
| 36 | side | char | 'B' buy / 'S' sell |
| 37 | msg_type | char | A=Add, D=Delete, T=Trade, U=Update, Q=Quote, S=System |
| 38-39 | padding | - | Zeroed / Wyzerowane |

## Files / Pliki
| File | Description / Opis |
|------|---|
| `mc_sender.py` | Python sender — text-based multicast messages |
| `mc_receiver.py` | Python receiver — basic message display |
| `mc_receiver_latency.py` | Python receiver — per-message latency measurement |
| `multicast.hpp` | C++ header-only — binary serialization, UDP sender/receiver, LatencyStats |
| `multicast_demo.cpp` | 38 unit tests (roundtrip, endian, UDP loopback) + throughput benchmark |

## Run / Uruchomienie
```bash
# C++ (from project root)
make build
./multicast/multicast_demo 100000

# Python (two terminals)
python3 multicast/mc_sender.py          # Terminal 1
python3 multicast/mc_receiver_latency.py  # Terminal 2
```
