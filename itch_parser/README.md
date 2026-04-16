# NASDAQ ITCH 5.0 Binary Protocol Parser

Parses binary market data feed messages from NASDAQ. Two implementations:
**Python** for prototyping and **C++** for the hot path.

*Analizuje binarne komunikaty źródła danych rynkowych z NASDAQ. Dwie implementacje:
Python do prototypowania i C++ na krytyczną ścieżkę.*

## Implementations / Implementacje

| | Python (`itch_parser.py`) | C++ (`itch_parser.hpp`) |
|---|---|---|
| Throughput / Przepustowość | ~1M msg/sec | **60M msg/sec** |
| Latency / Opóźnienie | ~1000 ns/msg | **16 ns/msg** (p99=50ns) |
| Use case / Zastosowanie | Testing, prototyping | Production hot path |

## Supported Messages / Obsługiwane wiadomości (9 types / typów)

| Code | Type | Size | Description / Opis |
|------|------|------|---|
| A | ADD_ORDER | 34 B | New limit order / Nowe zlecenie z limitem |
| F | ADD_ORDER_MPID | 38 B | New order with Market Participant ID / Nowe zlecenie z ID uczestnika |
| D | DELETE_ORDER | 17 B | Cancel order / Anuluj zlecenie |
| U | REPLACE_ORDER | 33 B | Modify order (new price/size) / Modyfikuj zlecenie |
| E | ORDER_EXECUTED | 29 B | Order filled / Zlecenie zrealizowane |
| C | ORDER_CANCELLED | 21 B | Partial cancel / Częściowe anulowanie |
| P | TRADE | 42 B | Matched trade / Dopasowana transakcja |
| S | SYSTEM_EVENT | 10 B | Market open/close/halt / Otwarcie/zamknięcie rynku |
| R | STOCK_DIRECTORY | 18 B | Stock metadata / Metadane akcji |

## ITCH vs OUCH vs FIX

- **ITCH**: binary, receive market data FROM exchange (read-only)
- **OUCH**: binary, send orders TO exchange (write-only)
- **FIX**: text-based, bidirectional communication

*ITCH: binarne, odbieranie danych rynkowych Z giełdy (tylko odczyt)*
*OUCH: binarne, wysyłanie zleceń DO giełdy (tylko zapis)*
*FIX: tekstowe, komunikacja dwukierunkowa*

## Files / Pliki

| File | Description / Opis |
|------|---|
| `itch_parser.py` | Python parser — all 9 message types, with demo |
| `itch_parser.hpp` | C++ header-only parser — zero-alloc, inline byte-swap |
| `benchmark_itch.cpp` | C++ benchmark — 10M messages, throughput + latency percentiles |

## Run / Uruchomienie

```bash
# Python demo / Demo Pythona
python3 itch_parser/itch_parser.py

# C++ benchmark / Benchmark C++
make build
./itch_parser/benchmark_itch
```
