# Order Book — 4 matching-engine variants

Four independent implementations (each a different trade-off between simplicity,
speed, and feature richness):

| Variant | File | When to use |
|---------|------|-------------|
| **v1 basic**       | `orderbook.cpp`         | minimal didactic — `std::map<Price, Qty>` per side |
| **v2 + ID**        | `orderbook.cpp` (extended) | adds cancel/modify with `unordered_map<id, Order>` |
| **FlatOrderBook**  | `orderbook_flat.hpp`    | O(1) add/cancel/modify, zero heap alloc, price-indexed array + occupancy bitmap (64-level/word cursor scans) |
| **FullOrderBook**  | `orderbook_pro.hpp`     | **production-grade L3**: FIFO per level, 10 order types + OCO/bracket/trailing, auction, snapshot recovery, compliance + analytics |

## Performance (Red Hat EL10, VirtualBox 2-core VM)
| Metric | v1/v2 | FlatOrderBook | FullOrderBook |
|--------|-------|---------------|---------------|
| Throughput (add)    | 17.8M ops/sec | ~25M ops/sec  | ~10-15M ops/sec |
| Add p50             | 50 ns         | ~40 ns        | ~80 ns          |
| Add p99             | 130 ns        | ~85 ns        | ~150 ns         |
| Cancel by ID        | O(log N)      | O(1)          | O(1)            |
| Memory footprint    | dynamic       | fixed         | fixed pool      |

FullOrderBook trades ~2× the add overhead for feature richness (FIFO traversal,
event callbacks, multi-level depth). The choice depends on the use case.

## FullOrderBook — production-grade matching engine

The most elaborate variant — header-only ~5300 lines, a full L3 book,
300+ tests in the demo. Intrusive FIFO per price level, LIFO pool (zero heap on
the hot path), `BookCluster<N>` as a multi-symbol container with cross-symbol arb
detection and aggregations.

### Order types (10)
- **LIMIT** — standard limit order
- **IOC** (Immediate-Or-Cancel) — take what you can now, cancel the rest
- **FOK** (Fill-Or-Kill) — full qty or nothing (atomic)
- **POST_ONLY** (ALO) — reject if you would become a taker (cross on entry)
- **ICEBERG** — show only `displayed_qty`, refresh from the hidden reserve up to
  the original display size (+ optional anti-detection jitter ±bps);
  after a refresh the maker loses priority (tail FIFO)
- **STOP** — trigger-on-price; after triggering it becomes LIMIT/MARKET
- **PEG** — peg to best bid/ask + offset (`submit_peg`) or to the midpoint
  (`submit_peg_mid`), optional hard cap price; re-quote when the TOB changes
- **MARKET** — no price, eat until exhausted
- **HIDDEN** — dark-pool semantics: invisible in L1/L2, matches in cross/auction
- **AON** (All-Or-None) — fills like FOK, but persists in the book

### Compound orders
- **OCO** (One-Cancels-Other): `link_oco(a, b)` — completion of one leg
  (full fill / cancel / expire / auction) cancels the other; deferred-cancel
  after the match loop (no dangling pointers in the hot path)
- **Bracket**: `submit_bracket(side, entry, qty, tp, sl)` — after a full fill of
  the entry it automatically arms a take-profit LIMIT/GTC + stop-loss STOP as an
  OCO pair; cancelling the entry = disarm
- **Trailing stop**: `submit_trailing_stop(side, offset, ...)` — the trigger
  follows the market (SELL: high-water − offset, BUY: low-water + offset)
- **MOC** (Market-On-Close): `submit_moc(side, qty)` — queued off-book,
  executed in the closing cross with price priority; remainders don't rest
- **LOC** (Limit-On-Close): `submit_loc(side, price, qty)` — like MOC, but
  with a limit; participates in the price-discovery cross
- **Reduce-only**: `submit_reduce_only(...)` — qty clamped to |account position|,
  never increases/flips the position

### Time in force (5) + STP (5 policies)
DAY / GTC / IOC / FOK / GTD (`expire_gtd()` sweep). Self-trade prevention:
NONE / CANCEL_NEWEST / CANCEL_OLDEST / CANCEL_BOTH / DECREMENT_AND_CXL
(NASDAQ DAC).

### Auction matching (opening/closing cross)
`enter_auction_mode()` → orders wait without matching → `run_auction()` finds a
single clearing price maximizing matched qty (tie-break: min imbalance),
FIFO on oversubscription, full incremental depth/hidden accounting.
`indicative_auction_info()` — NOII (NASDAQ-style): indicative price + paired
qty + imbalance per side WITHOUT execution; `run_closing_auction()` — full
closing cross with MOC/LOC injection; `try_run_auction(threshold)` — volatility
extension (LSE/Xetra): surplus > threshold defers the cross instead of executing
it with a large imbalance; wash-trade surveillance flags clients on both sides
of a cross.

### Session lifecycle + halt
A full session day: `begin_pre_open()` → `open_market()` (opening cross) →
continuous → `begin_closing()` → `close_market()` (closing cross + CLOSED,
submits rejected, cancel allowed). Two halt models: hard halt
(`halt()` — submits fail) and LULD-style pause (`halt_for_auction()` —
order entry continues, `resume_with_auction()` does a reopen cross;
`set_luld_halt_to_auction(true)` ties the pause to a band breach).

### Compliance / market integrity
- **LULD** circuit breaker (Limit Up / Limit Down bands)
- **SSR** — uptick rule (SEC Rule 201): a short ≤ best bid is rejected while
  SSR is active; auto-triggers the circuit breaker on a drop from reference
- **MIFID II RTS27/28** — effective spread, signed price impact, executions
- **Quote stuffing detection** (SEC 15c3-5) — per-account cancel rate flags
- **Rate limiting** — token bucket per account (msg-rate throttle); reject
  RATE_LIMITED, internal engine resubmits bypass it
- **Wash-trade surveillance** — flags clients with fills on both sides of a
  single auction cross
- **Kill switch**: `mass_cancel(client_id)` — also covers pending STOPs
- **Per-account exposure** — open/filled net+gross qty per client_id
- **Audit log** + **event seq numbers** (gap detect / dedup at the consumer)
- **Drop copy** — a separate event stream for a monitored account (full
  lifecycle: ACCEPT/REJECT/FILL/REPLACE/CANCEL/EXPIRE, continuous + auction)

### Analytics (microstructure)
- **Flow**: VPIN, flow imbalance, Cont-Kukanov OFI, signed-volume EMA,
  Markov chain of trade direction, Lee-Ready classification (+accuracy)
- **Impact/TCA**: Kyle's lambda (global + per-side), implementation
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
book.audit_book_integrity();   // 0 == book is consistent
```
Verifies 6 invariants (prev/next, price==level, order_count,
Σ displayed == total_qty, Σ (T−F−D) == total_hidden, tail). The audit found
5 real accounting bugs (partial-rest depth, hidden drift, auction
aggregates, iceberg modify, stop-cancel gap in the kill switch).

### L1 / L2 / L3 views
```cpp
TopOfBook tob = book.top_of_book();        // best bid/ask + qty + count
int32_t mid   = book.mid_ticks();
int32_t micro = book.microprice_ticks();   // size-weighted (better predictor)
int32_t imb   = book.imbalance_bps_n(3);   // depth-3 imbalance (hard to spoof)

DepthLevel bids[10], asks[10];
int32_t bn=0, an=0;
book.depth(10, bids, asks, &bn, &an);      // L2 depth — N best per side
int32_t pos = book.queue_position(order_id);   // L3 — how many orders ahead of me
book.predicted_vwap_ticks(Side::BUY, 500);     // pre-trade impact (walk-the-book)
```

### Trade tape + VWAP
Ring buffer of the last 1024 executions:
```cpp
Trade recent[100];
size_t n = book.recent_trades(recent, 100);
int32_t vwap  = book.tape_vwap_ticks();
int32_t vwapN = book.last_n_vwap_ticks(50);    // rolling, more responsive
```

### Snapshot recovery (full L3 dump)
```cpp
std::vector<uint8_t> buf(book.snapshot_size_estimate());
size_t written = book.serialize_snapshot(buf.data(), buf.size());
Book b2;
b2.load_snapshot(buf.data(), written);   // rebuild + audit-clean + id continuity
```

Wire format: 4 B magic `"OBPO"` + 4 B version (current: 2) + 8 B count +
N×packed `OrderRecord` (70 B). Zero allocation; `next_order_id_` continues
past the max loaded id; iceberg refresh size, pending STOPs (return to the
trigger queue) and PEGs (recover their reprice) survive the round-trip.
`BookCluster` has its own snapshot (magic `"OBCL"`) — multi-symbol recovery in
one buffer.

### L2 delta protocol
18-byte wire format (types A/D/M/T), `enable_delta_queue(true)` + drain —
an incremental feed for downstream consumers.

## Build & Run

```bash
# all variants
make build

# Basic v1 + v2 matching engine demo
./orderbook/orderbook

# FlatOrderBook (sanity + benchmark vs std::map)
./orderbook/orderbook_flat 1000000

# FullOrderBook — 300+ tests + percentile benchmark
./orderbook/orderbook_pro_demo 100000

# Throughput benchmark: std::map baseline vs FlatOrderBook, multi-trial
./orderbook/benchmark_orderbook                  # report only (CI-safe)
./orderbook/benchmark_orderbook 1000000 7 17.8   # gate: exit 1 unless the
                                                 # SLOWEST of 7 trials beats
                                                 # 17.8M orders/sec

# Latency histogram (v2)
./orderbook/latency_histogram 1000000
```

## Files

| File | Purpose |
|------|---------|
| `orderbook.cpp` | v1/v2 matching engine demo |
| `orderbook_flat.hpp` + `orderbook_flat.cpp` | flat-array O(1) variant + sanity + bench |
| `orderbook_pro_types.hpp` | data model — constants, enums, Order/PriceLevel/Trade/BookStats/BookEvent (standalone, no engine) |
| `orderbook_pro.hpp` | **FullOrderBook** — header-only L3 production-grade matching engine (includes `_types`) |
| `orderbook_pro_cluster.hpp` | `BookCluster<N>` — multi-symbol container + cross-symbol arb + snapshot (includes `orderbook_pro.hpp`) |
| `orderbook_pro_demo.cpp` | FullOrderBook — 370+ tests (order types, STP, OCO/bracket/trailing, auction, snapshot, integrity audit, analytics) + percentile benchmark |
| `benchmark_orderbook.cpp` | Multi-trial throughput benchmark (std::map vs FlatOrderBook) + optional min-trial threshold gate |
| `latency_histogram.cpp` | Per-order latency percentiles (HFT-relevant) |
