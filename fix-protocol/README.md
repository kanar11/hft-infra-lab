# FIX 4.2 Protocol Parser

Parses Financial Information eXchange messages for order routing.
*Analizuje komunikaty Financial Information eXchange do routingu zamówień.*

## Performance 

| Metric | C++ |
|--------|-----|
| **Throughput** | **5.5M msg/sec** |
| **Latency (p50)** | **150 ns** |
| **Latency (p99)** | **250 ns** |

## Supported Message Types / Obsługiwane typy wiadomości

- D = New Order
- G = Modify
- F = Cancel
- 8 = Execution Report
- 0 = Heartbeat

## Key Tags / Kluczowe znaczniki

- 8  = BeginString (ZAWSZE pierwszy)
- 9  = BodyLength (ZAWSZE drugi)
- 35 = MsgType
- 55 = Symbol
- 54 = Side (1=Buy, 2=Sell)
- 44 = Price
- 38 = OrderQty
- 10 = CheckSum (ZAWSZE ostatni)

## Walidacja sesji (standard branżowy)

Każdy zgodny silnik FIX MUSI walidować pola obowiązkowe — wcześniej parser
ich nie sprawdzał. Teraz:

| Co | Metoda | Opis |
|----|--------|------|
| **Delimiter SOH** | auto | Prawdziwy wire FIX używa `\x01` (ASCII SOH), nie `\|`. Parser auto-wykrywa: SOH jeśli obecny, inaczej `\|` (human-readable do logów/testów). |
| **CheckSum (tag 10)** | `checksum_valid()` | Suma modulo-256 wszystkich bajtów do delimitera przed `10=`. Wykrywa uszkodzenie transmisji. |
| **BodyLength (tag 9)** | `body_length_valid()` | Liczba bajtów body (od pola po tagu 9 do delimitera przed tagiem 10). |
| **Wymagany nagłówek** | `has_required_header()` | Obecność tagów 8, 9, 35, 10. |
| **Pełna walidacja** | `is_valid()` | nagłówek + checksum + bodylength. |

Budowanie poprawnych wiadomości (z auto-policzonym tagiem 9 i 10):

```cpp
char msg[256];
FIXMessage::build_message(msg, sizeof(msg),
    "35=D\x01" "55=AAPL\x01" "54=1\x01" "44=150.25\x01" "38=100\x01");
// → "8=FIX.4.2<SOH>9=<len><SOH>35=D<SOH>...<SOH>10=<checksum><SOH>"

FIXMessage m;
m.parse(msg);
if (m.is_valid()) { /* przyjmij zlecenie */ }
```

## Files / Pliki

| File | Description |
|------|-------------|
| `fix_parser.hpp` | C++ header-only — parser + walidacja CheckSum/BodyLength + builder |
| `fix_demo.cpp` | C++ demo with 31 unit tests (incl. checksum/bodylength/SOH) + throughput benchmark |

## Run 

```bash
# C++ (build + run)
make build
./fix-protocol/fix_demo              # tests + benchmark (1M parses)
./fix-protocol/fix_demo 5000000      # 5M parses
```
