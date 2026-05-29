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

## Sequence gap detection / Wykrywanie luk sekwencji

UDP **gubi pakiety** (jest unreliable) — przychodzą nie w kolejności, bywają
zdublowane. Prawdziwa giełda numeruje każdą wiadomość monotonicznym `sequence`
żeby odbiorca wykrył lukę i zrobił recovery:

- **gap-fill request** do serwera retransmisji (np. NASDAQ MoldUDP64), albo
- **A/B line arbitration** — odbiór dwóch identycznych feedów (linia A i B),
  bierzesz pakiet który dotarł pierwszy, braki uzupełniasz z drugiej linii.

`SequenceTracker` (w `multicast.hpp`) wykrywa to wszystko — `observe(seq)`
zwraca `OK` / `GAP` / `DUPLICATE` i utrzymuje statystyki: `gaps`, `lost`,
`duplicates`, `loss_rate()`. `MulticastReceiver` woła go automatycznie przy
każdym `receive()`; dostęp przez `sequence_tracker()`.

## MoldUDP64 framing (standard branżowy / NASDAQ)

Prawdziwe giełdy nie wysyłają jednej wiadomości na datagram — używają
**MoldUDP64**, standardu NASDAQ (TotalView-ITCH, BX, PSX i wiele globalnych
giełd). Jeden UDP datagram niesie nagłówek + **wiele** wiadomości:

```
[0..9]   Session         10B ASCII (identyfikator sesji)
[10..17] Sequence Number uint64 BE — numer PIERWSZEJ wiadomości w pakiecie
[18..19] Message Count   uint16 BE — ile wiadomości
potem MessageCount bloków: [length uint16 BE][message data]
```

Pakiety specjalne: `Message Count == 0` → heartbeat (utrzymuje sesję, wykrywa
luki gdy feed idle), `0xFFFF` → end of session. Batchowanie amortyzuje narzut
UDP/IP (28 B nagłówków/pakiet) przy zachowaniu sekwencji per-wiadomość.

```cpp
// Nadawca: spakuj 3 wiadomości w jeden datagram MoldUDP64.
uint8_t pkt[1500];
size_t n = multicast::mold_serialize_packet(pkt, sizeof(pkt), "SESSION001", 100, msgs, 3);

// Odbiorca: parsuj, śledź sekwencję na poziomie pakietu, dostań każdą wiadomość.
multicast::MoldUDP64Header h;
multicast::SequenceTracker trk;
int count = multicast::mold_parse_packet(pkt, n, h, &trk,
    [](const MarketDataMessage& m) { /* obsłuż wiadomość */ });
// trk.observe_packet() automatycznie wykrywa lukę gdy zgubimy CAŁY datagram.
```

```cpp
multicast::MulticastReceiver rx;
rx.init("239.1.1.1", 5001);
multicast::MarketDataMessage msg;
multicast::SequenceTracker::Status st;
while (rx.receive(msg, &st)) {
    if (st == multicast::SequenceTracker::Status::GAP) {
        // zgubiliśmy pakiet(y) — trigger recovery / przełącz na linię B
    }
}
const auto& s = rx.sequence_tracker();
printf("loss rate: %.4f%% (%llu lost)\n", s.loss_rate() * 100, (unsigned long long)s.lost);
```

## Files / Pliki
| File | Description / Opis |
|------|---|
| `multicast.hpp` | C++ header-only — binary serialization, UDP sender/receiver, SequenceTracker (gap detection), MoldUDP64 framing, LatencyStats |
| `multicast_demo.cpp` | unit tests (roundtrip, endian, UDP loopback, sequence gap/duplicate, MoldUDP64 packets) + throughput benchmark |

## Run / Uruchomienie
```bash
make build
./multicast/multicast_demo 100000    # benchmark
./multicast/multicast_demo 0         # tests only
```
