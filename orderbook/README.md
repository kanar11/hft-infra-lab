# Order Book — 4 warianty matching engine

Cztery niezależne implementacje (każda inny trade-off między prostotą,
szybkością i bogactwem funkcji):

| Wariant | Plik | Kiedy używać |
|---------|------|-------------|
| **v1 basic**       | `orderbook.cpp`         | minimal didactic — `std::map<Price, Qty>` per side |
| **v2 + ID**        | `orderbook.cpp` (rozszerzony) | dodaje cancel/modify z `unordered_map<id, Order>` |
| **FlatOrderBook**  | `orderbook_flat.hpp`    | O(1) add/cancel/modify, zero heap alloc, tablica indeksowana ceną |
| **FullOrderBook**  | `orderbook_pro.hpp`     | **production-grade L3**: FIFO per level, 10 typów zleceń + OCO/bracket/trailing, auction, snapshot recovery, compliance + analytics |

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

Najbardziej rozbudowany wariant — header-only ~5300 linii, pełna L3 księga,
300+ testów w demo. Intrusive FIFO per price level, LIFO pool (zero heap na
hot path), `BookCluster<N>` jako multi-symbol kontener z cross-symbol arb
detection i aggregacjami.

### Order types (10)
- **LIMIT** — standardowy limit order
- **IOC** (Immediate-Or-Cancel) — weź co możesz teraz, resztę kasuj
- **FOK** (Fill-Or-Kill) — cała qty albo nic (atomowy)
- **POST_ONLY** (ALO) — odrzuć jeśli zostałbyś takerem (cross na entry)
- **ICEBERG** — pokazuj tylko `displayed_qty`, refresh z hidden reserve do
  oryginalnego display size (+ opcjonalny anti-detection jitter ±bps);
  po refresh maker traci priority (tail FIFO)
- **STOP** — trigger-on-price, po triggerze staje się LIMIT/MARKET
- **PEG** — peg do best bid/ask + offset (`submit_peg`) albo do midpointu
  (`submit_peg_mid`), opcjonalny twardy cap price; re-quote po zmianie TOB
- **MARKET** — bez ceny, zjadasz aż do wyczerpania
- **HIDDEN** — dark-pool semantyka: niewidoczne w L1/L2, match w cross/auction
- **AON** (All-Or-None) — fill jak FOK, ale persiste w księdze

### Zlecenia złożone
- **OCO** (One-Cancels-Other): `link_oco(a, b)` — completion jednej nogi
  (full fill / cancel / expire / auction) kasuje drugą; deferred-cancel po
  pętli match (bez dangling pointerów w hot path)
- **Bracket**: `submit_bracket(side, entry, qty, tp, sl)` — po full fillu
  entry automatycznie armuje take-profit LIMIT/GTC + stop-loss STOP jako
  parę OCO; cancel entry = disarm
- **Trailing stop**: `submit_trailing_stop(side, offset, ...)` — trigger
  podąża za rynkiem (SELL: high-water − offset, BUY: low-water + offset)
- **MOC** (Market-On-Close): `submit_moc(side, qty)` — kolejka poza księgą,
  wykonanie w closing cross z price priority; resztki nie restują
- **LOC** (Limit-On-Close): `submit_loc(side, price, qty)` — jak MOC, ale
  z limitem; uczestniczy w price discovery crossu
- **Reduce-only**: `submit_reduce_only(...)` — qty clamped do |pozycji|
  konta, nigdy nie powiększy/odwróci pozycji

### Time in force (5) + STP (5 polityk)
DAY / GTC / IOC / FOK / GTD (`expire_gtd()` sweep). Self-trade prevention:
NONE / CANCEL_NEWEST / CANCEL_OLDEST / CANCEL_BOTH / DECREMENT_AND_CXL
(NASDAQ DAC).

### Auction matching (opening/closing cross)
`enter_auction_mode()` → orders czekają bez matchu → `run_auction()` znajduje
single clearing price maksymalizujący matched qty (tie-break: min imbalance),
FIFO przy oversubscription, pełny incremental accounting depth/hidden.
`indicative_auction_info()` — NOII (NASDAQ-style): indicative price + paired
qty + imbalance per side BEZ egzekucji; `run_closing_auction()` — pełny
closing cross z injection MOC/LOC; `try_run_auction(threshold)` — volatility
extension (LSE/Xetra): surplus > próg odracza cross zamiast wykonywać go
z dużym imbalance.

### Compliance / market integrity
- **LULD** circuit breaker (Limit Up / Limit Down bandy)
- **SSR** — uptick rule (SEC Rule 201): short ≤ best bid odrzucany przy
  aktywnym SSR; auto-trigger circuit breaker na spadku od reference
- **MIFID II RTS27/28** — effective spread, signed price impact, executions
- **Quote stuffing detection** (SEC 15c3-5) — per-account cancel rate flags
- **Kill switch**: `mass_cancel(client_id)` — obejmuje też pending STOPy
- **Per-account exposure** — open/filled net+gross qty per client_id
- **Audit log** + **event seq numbers** (gap detect / dedup u konsumenta)

### Analytics (mikrostruktura)
- **Flow**: VPIN, flow imbalance, Cont-Kukanov OFI, signed-volume EMA,
  Markov chain kierunku trade'ów, Lee-Ready classification (+accuracy)
- **Impact/TCA**: Kyle's lambda (globalna + per-side), implementation
  shortfall, effective vs quoted spread, slippage guard, fill bands,
  per-account VWAP
- **Price**: microprice, mid/microprice ring + momentum, TWAP-of-mid,
  realized volatility, Hurst R/S, price-change histogram, trend classifier
- **Depth**: multi-level imbalance, depth concentration, pyramid steepness,
  volume profile + point-of-control, top-K levels/orders, hidden ratio
- **Tempo**: trades/sec, inter-trade gaps (+ Fano factor), quote life,
  TOB stability streak, burst detector, first-fill latency, order age

### Integrity audit
```cpp
book.audit_book_integrity();   // 0 == księga spójna
```
Weryfikuje 6 invariantów (prev/next, price==level, order_count,
Σ displayed == total_qty, Σ (T−F−D) == total_hidden, tail). Audit znalazł
5 realnych bugów accountingu (partial-rest depth, hidden drift, auction
aggregates, iceberg modify, stop-cancel gap w kill switchu).

### L1 / L2 / L3 views
```cpp
TopOfBook tob = book.top_of_book();        // best bid/ask + qty + count
int32_t mid   = book.mid_ticks();
int32_t micro = book.microprice_ticks();   // size-weighted (lepszy predictor)
int32_t imb   = book.imbalance_bps_n(3);   // depth-3 imbalance (trudny do spoofu)

DepthLevel bids[10], asks[10];
int32_t bn=0, an=0;
book.depth(10, bids, asks, &bn, &an);      // L2 depth — N best per side
int32_t pos = book.queue_position(order_id);   // L3 — ile zleceń przede mną
book.predicted_vwap_ticks(Side::BUY, 500);     // pre-trade impact (walk-the-book)
```

### Trade tape + VWAP
Ring buffer ostatnich 1024 egzekucji:
```cpp
Trade recent[100];
size_t n = book.recent_trades(recent, 100);
int32_t vwap  = book.tape_vwap_ticks();
int32_t vwapN = book.last_n_vwap_ticks(50);    // rolling, responsywniejszy
```

### Snapshot recovery (full L3 dump)
```cpp
std::vector<uint8_t> buf(book.snapshot_size_estimate());
size_t written = book.serialize_snapshot(buf.data(), buf.size());
Book b2;
b2.load_snapshot(buf.data(), written);   // rebuild + audit-clean + id continuity
```

Wire format: 4 B magic `"OBPO"` + 4 B version (aktualna: 2) + 8 B count +
N×packed `OrderRecord` (70 B). Zero alokacji; `next_order_id_` kontynuuje
za max wczytanym id; iceberg refresh size przeżywa round-trip.

### L2 delta protocol
18-bajtowy wire format (typy A/D/M/T), `enable_delta_queue(true)` + drain —
incremental feed dla downstream konsumentów.

## Build & Run

```bash
# wszystkie warianty
make build

# Basic v1 + v2 matching engine demo
./orderbook/orderbook

# FlatOrderBook (sanity + benchmark vs std::map)
./orderbook/orderbook_flat 1000000

# FullOrderBook — 300+ testów + percentyle benchmark
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
| `orderbook_pro_demo.cpp` | FullOrderBook — 300+ testów (typy zleceń, STP, OCO/bracket/trailing, auction, snapshot, integrity audit, analytics) + benchmark z percentylami |
| `benchmark_orderbook.cpp` | Throughput benchmark across order counts |
| `latency_histogram.cpp` | Per-order latency percentiles (HFT-relevant) |
