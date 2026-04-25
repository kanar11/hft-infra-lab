# Multicast Market Data Feed / Kanał danych rynkowych Multicast

UDP multicast sender and receiver for simulating exchange market data feeds.
*Nadajnik i odbiornik UDP multicast do symulowania kanałów danych giełdowych.*

## Performance / Wydajność

| Metric | Result |
|---|---|
| Throughput / Przepustowość | **23.2M msg/sec** |
| Latency / Opóźnienie | **20 ns/msg** (p50) |
| Wire format / Format sieciowy | Binary (40 bytes, big-endian) |

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
| `multicast.hpp` | C++ header-only — binary serialization, UDP sender/receiver, LatencyStats |
| `multicast_demo.cpp` | 38 unit tests (roundtrip, endian, UDP loopback) + throughput benchmark |

## Run / Uruchomienie
```bash
make build
./multicast/multicast_demo 100000    # benchmark
./multicast/multicast_demo 0         # tests only
```
