# Multicast Market Data Feed

UDP multicast sender and receiver for simulating exchange market data feeds.

## Performance

| Metric | Result |
|---|---|
| Throughput | **23.2M msg/sec** |
| Latency | **20 ns/msg** (p50) |
| Wire format | Binary (40 bytes, big-endian) |

## Binary Wire Format (40 bytes)

| Offset | Field | Type | Description |
|--------|-------|------|----|
| 0-7 | sequence | uint64 BE | Message sequence number |
| 8-15 | timestamp_ns | uint64 BE | Send timestamp (nanoseconds) |
| 16-23 | symbol | char[8] | Ticker, space-padded |
| 24-31 | price | int64 BE | Fixed-point (x10000) |
| 32-35 | quantity | uint32 BE | Order quantity |
| 36 | side | char | 'B' buy / 'S' sell |
| 37 | msg_type | char | A=Add, D=Delete, T=Trade, U=Update, Q=Quote, S=System |
| 38-39 | padding | - | Zeroed |

## Sequence gap detection

UDP **drops packets** (it is unreliable) — they arrive out of order and can be
duplicated. A real exchange numbers every message with a monotonic `sequence`
so the receiver can detect a gap and recover:

- a **gap-fill request** to a retransmission server (e.g. NASDAQ MoldUDP64), or
- **A/B line arbitration** — receive two identical feeds (line A and B),
  take whichever packet arrives first, fill gaps from the other line.

`SequenceTracker` (in `multicast.hpp`) detects all of this — `observe(seq)`
returns `OK` / `GAP` / `DUPLICATE` and maintains statistics: `gaps`, `lost`,
`duplicates`, `loss_rate()`. `MulticastReceiver` calls it automatically on
every `receive()`; access it via `sequence_tracker()`.

## MoldUDP64 framing (industry standard / NASDAQ)

Real exchanges don't send one message per datagram — they use **MoldUDP64**,
a NASDAQ standard (TotalView-ITCH, BX, PSX, and many global exchanges). One
UDP datagram carries a header + **multiple** messages:

```
[0..9]   Session         10B ASCII (session identifier)
[10..17] Sequence Number uint64 BE — sequence of the FIRST message in the packet
[18..19] Message Count   uint16 BE — how many messages
then MessageCount blocks: [length uint16 BE][message data]
```

Special packets: `Message Count == 0` → heartbeat (keeps the session alive,
detects gaps while the feed is idle), `0xFFFF` → end of session. Batching
amortizes UDP/IP overhead (28 B of headers/packet) while preserving per-message
sequencing.

```cpp
// Sender: pack 3 messages into one MoldUDP64 datagram.
uint8_t pkt[1500];
size_t n = multicast::mold_serialize_packet(pkt, sizeof(pkt), "SESSION001", 100, msgs, 3);

// Receiver: parse, track the sequence at the packet level, get each message.
multicast::MoldUDP64Header h;
multicast::SequenceTracker trk;
int count = multicast::mold_parse_packet(pkt, n, h, &trk,
    [](const MarketDataMessage& m) { /* handle the message */ });
// trk.observe_packet() automatically detects a gap when we lose a WHOLE datagram.
```

```cpp
multicast::MulticastReceiver rx;
rx.init("239.1.1.1", 5001);
multicast::MarketDataMessage msg;
multicast::SequenceTracker::Status st;
while (rx.receive(msg, &st)) {
    if (st == multicast::SequenceTracker::Status::GAP) {
        // we lost packet(s) — trigger recovery / switch to line B
    }
}
const auto& s = rx.sequence_tracker();
printf("loss rate: %.4f%% (%llu lost)\n", s.loss_rate() * 100, (unsigned long long)s.lost);
```

## Files
| File | Description |
|------|---|
| `multicast.hpp` | C++ header-only — binary serialization, UDP sender/receiver, SequenceTracker (gap detection), MoldUDP64 framing, LatencyStats |
| `multicast_demo.cpp` | unit tests (roundtrip, endian, UDP loopback, sequence gap/duplicate, MoldUDP64 packets) + throughput benchmark |

## Run
```bash
make build
./multicast/multicast_demo 100000    # benchmark
./multicast/multicast_demo 0         # tests only
```
