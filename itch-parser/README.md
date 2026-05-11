# NASDAQ ITCH 5.0 Binary Protocol Parser

Parses binary market data feed messages from NASDAQ.
*Analizuje binarne komunikaty źródła danych rynkowych z NASDAQ.*

## Performance 

| Metric | C++ (`itch_parser.hpp`) |
|--------|------------------------|
| Throughput / Przepustowość | **60M msg/sec** |
| Latency / Opóźnienie | **16 ns/msg** (p99=50ns) |

## Supported Messages / Obsługiwane wiadomości (9 types / typów)

| Code | Type | Size | Description |
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

## Files 

| File | Description / Opis |
|------|---|
| `itch_parser.hpp` | C++ header-only parser — zero-alloc, inline byte-swap |
| `benchmark_itch.cpp` | C++ benchmark — 10M messages, throughput + latency percentiles |

## Run 

```bash
# C++ benchmark / Benchmark C++
make build
./itch-parser/benchmark_itch
```
