# Order Book — 4 warianty matching engine

Cztery niezależne implementacje (każda inny trade-off między prostotą,
szybkością i bogactwem funkcji):

| Wariant | Plik | Kiedy używać |
|---------|------|-------------|
| **v1 basic**       | `orderbook.cpp`         | minimal didactic — `std::map<Price, Qty>` per side |
| **v2 + ID**        | `orderbook.cpp` (rozszerzony) | dodaje cancel/modify z `unordered_map<id, Order>` |
| **FlatOrderBook**  | `orderbook_flat.hpp`    | O(1) add/cancel/modify, zero heap alloc, tablica indeksowana ceną |
| **FullOrderBook**  | `orderbook_pro.hpp`     | **production-grade L3**: FIFO per level, 8 typów zleceń, snapshot recovery, analytics |

## Wydajność (Red Hat EL10, VirtualBox 2-core VM)
| Metric | v1/v2 | FlatOrderBook | FullOrderBook |
|--------|-------|---------------|---------------|
| Throughput (add)    | 17.8M ops/sec | ~25M ops/sec  | ~10-15M ops/sec |
| Add p50             | 50 ns         | ~40 ns        | ~80 ns          |
| Add p99             | 130 ns        | ~85 ns        | ~150 ns         |
| Cancel by ID        | O(log N)      | O(1)          | O(1)            |
| Memory footprint    | dynamic       | fixed         | fixed pool      |

FullOrderBook trade'uje 2× narzut na add za bogactwo funkcji (FIFO traversal,
event callbacks, multi-level depth). Wybór zależy od use case.

## FullOrderBook — production-grade matching engine

Najbardziej rozbudowany wariant — header-only ~1100 linii, pełna L3 księga.

### Order types (8)
- **LIMIT** — standardowy limit order
- **IOC** (Immediate-Or-Cancel) — weź co możesz teraz, resztę kasuj
- **FOK** (Fill-Or-Kill) — cała qty albo nic (atomowy)
- **POST_ONLY** (ALO) — odrzuć jeśli zostałbyś takerem (cross na entry)
- **ICEBERG** — pokazuj tylko `displayed_qty`, ukrywaj resztę; po refresh
  maker traci priority (przesuwany na tail FIFO)
- **STOP** — trigger-on-price, po triggerze staje się LIMIT/MARKET
- **PEG** — peg do mid albo best bid/ask + offset
- **MARKET** — bez ceny, zjadasz aż do wyczerpania

### Time in force (5)
DAY / GTC / IOC / FOK / GTD (Good-Til-Date z `expire_ts_ns`).

### Self-Trade Prevention (5 polityk)
- `NONE` — pozwól wash trade (testowe)
- `CANCEL_NEWEST` — przychodzące zlecenie pada
- `CANCEL_OLDEST` — resting zlecenie pada
- `CANCEL_BOTH` — oba pada
- `DECREMENT_AND_CXL` — NASDAQ DAC

### Lifecycle events (przez callback)
`ACCEPT` / `REJECT` / `FILL` / `CANCEL` / `EXPIRE` / `REPLACE` / `BOOK_UPDATE`

Konsument rejestruje:
```cpp
book.set_event_callback([](const BookEvent& ev, void* ctx) {
    // dispatch do logger / risk / strategy
}, my_ctx);
```

### Reject reasons (10)
- `PRICE_OUT_OF_RANGE` — cena poza siatką
- `QTY_ZERO_OR_NEGATIVE`
- `POOL_EXHAUSTED` — pula Orderów wyczerpana
- `POST_ONLY_WOULD_CROSS`
- `FOK_NOT_FILLABLE`
- `SELF_TRADE_BLOCKED`
- `DUPLICATE_ID`
- `LOCKED_MARKET` / `CROSSED_MARKET` — market integrity

### L1 / L2 / L3 views
```cpp
TopOfBook tob = book.top_of_book();        // best bid/ask + qty + count
int32_t mid   = book.mid_ticks();
int32_t micro = book.microprice_ticks();   // size-weighted (lepszy predictor)
int32_t imb   = book.imbalance_bps();      // (b-a)/(b+a)*10000

DepthLevel bids[10], asks[10];
int32_t bn=0, an=0;
book.depth(10, bids, asks, &bn, &an);      // L2 depth — N best per side

int32_t pos = book.queue_position(order_id);  // L3 — ile zleceń przede mną
```

### Trade tape + VWAP
Ring buffer ostatnich 1024 egzekucji:
```cpp
Trade recent[100];
size_t n = book.recent_trades(recent, 100);
int32_t vwap = book.tape_vwap_ticks();
```

### Snapshot recovery (full L3 dump)
```cpp
std::vector<uint8_t> buf(book.snapshot_size_estimate());
size_t written = book.serialize_snapshot(buf.data(), buf.size());
// ... send do peer / dysk

Book b2;
b2.load_snapshot(buf.data(), written);     // rebuild księgi bytewise
```

Wire format: 4 B magic `"OBPO"` + 4 B version + 8 B count + N×packed `OrderRecord`.
Zero alokacji podczas serialize/load.

## Build & Run

```bash
# wszystkie warianty
make build

# Basic v1 + v2 matching engine demo
./orderbook/orderbook

# FlatOrderBook (sanity + benchmark vs std::map)
./orderbook/orderbook_flat 1000000

# FullOrderBook — 17 testów + percentyle benchmark
./orderbook/orderbook_pro_demo 100000

# Throughput + latency histogram (v2)
./orderbook/benchmark_orderbook
./orderbook/latency_histogram 1000000
```

## Files / Pliki

| File | Purpose |
|------|---------|
| `orderbook.cpp` | v1/v2 matching engine demo |
| `orderbook_flat.hpp` + `orderbook_flat.cpp` | flat-array O(1) variant + sanity + bench |
| `orderbook_pro.hpp` | **FullOrderBook** header-only L3 production-grade |
| `orderbook_pro_demo.cpp` | FullOrderBook — 17 testów (LIMIT/IOC/FOK/POST_ONLY/ICEBERG/STP/snapshot/depth/queue/microprice/imbalance/VWAP) + benchmark z percentylami |
| `benchmark_orderbook.cpp` | Throughput benchmark across order counts |
| `latency_histogram.cpp` | Per-order latency percentiles (HFT-relevant) |
