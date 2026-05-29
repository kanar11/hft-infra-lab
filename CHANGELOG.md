# Changelog

All notable changes to this project are documented here.
Format based on [Keep a Changelog](https://keepachangelog.com/),
versioning loosely follows [SemVer](https://semver.org/).

## [v2.0] — 2026-05-26

Major expansion since v1.0 — 105 commits. The lab grew from a core
matching-engine + OMS pipeline into a full low-latency trading stack with
four data sources, three strategy families, six lock-free primitives, three
logger variants, Python bindings, and industry-standard market-data framing.

### Added

**Data sources / feeds**
- WebSocket live feed — minimal RFC 6455 client (`feed/ws_client.hpp`) with
  HTTP upgrade, frame parsing, ping/pong, masking, random Sec-WebSocket-Key,
  plus a self-contained mock server demo (`feed/feed_demo`).
- WebSocket over TLS (`wss://`) — `feed/wss_client.hpp` on OpenSSL with SNI,
  hostname verification, cert-chain check, TLS 1.2+; `feed/wss_demo` connects
  to a real Binance stream (optional `make wss`).
- Real Nasdaq order replay through the OMS pipeline from LOBSTER CSVs
  (`replay/lobster_demo`), plus a deterministic fixture generator
  (`replay/gen_fixture.py`) and a bundled 5k-event sample.
- epoll-based async TCP server with self-test FIX ingestion
  (`network/fix_server_demo`).

**Strategies**
- Market Maker — proactive two-sided quoter with inventory skew
  (`strategy/market_maker.hpp`).
- Execution algorithms — TWAP and VWAP (U-shape volume profile) with
  slippage measured in basis points (`strategy/exec_algo.hpp`,
  `strategy/exec_demo`).

**Order book**
- `FlatOrderBook<LEVELS>` — flat-array O(1) add/cancel/modify variant with
  ID tracking; head-to-head benchmark vs `std::map` with a latency histogram
  (p50/p95/p99/p99.9).

**Lock-free primitives** (`lockfree/`)
- Expanded from SPSC to six: MPSC, MPMC, Sequencer (LMAX-style),
  WaitableMPSC, and VarlenRingBuffer.
- TSAN-instrumented stress suite covering all six (`lockfree/lockfree_stress`).

**Loggers** (`logger/`)
- `LockfreeTradeLogger` — same audit semantics on `lockfree::SPSCQueue`.
- `MmapTradeLogger` — mmap-backed persistent audit ring.

**Market data transport** (`multicast/`)
- `SequenceTracker` — packet-loss / duplicate / reorder detection.
- MoldUDP64 framing (NASDAQ industry standard) — multi-message datagrams with
  session + sequence + message-count header, heartbeats, end-of-session, and
  packet-level gap detection.

**Tooling / bindings**
- pybind11 Python extension exposing OMS, RiskManager, FlatOrderBook
  (`bindings/pyhft`).
- Runtime `config.yaml` loading — zero hardcoded params; symbol universe in config.
- Threaded simulator pipeline (`run_pipeline_threaded`) handing off via SPSCQueue.

### Changed

- Smart Order Router now selects on **effective price** (quote ± maker/taker
  fee), not raw quote; `RouteDecision` exposes `effective_price`, `total_fee`,
  `num_venues`.
- Consolidated `Side` enum, `sym_to_key`, and time helpers into `common/`.
- `Signal::side` migrated from `char[5]` to the `Side` enum.
- Risk Manager and OMS track **pending exposure** across submit/fill/cancel for
  correct HFT position limits.
- Fixed-point int64 prices on the hot path; O(1) hotspots in Risk and Strategy.
- Polish inline documentation across risk, oms, logger, strategy, lockfree,
  simulator, router, and multicast modules.

### CI / Infrastructure

- Six CI jobs: static-analysis (cppcheck), build-and-test (g++ + clang++),
  sanitizers (ASAN+UBSAN+TSAN), clang-tidy (bugprone + performance),
  python-bindings.
- `build/` directory with automatic header-dependency tracking.
- Benchmarks workflow that refreshes `BENCHMARKS.md` from a CI run.

### Fixed

- Numerous hardening fixes surfaced by sanitizers and static analysis:
  bounded FIX `strlen`, `window<=0` / NaN/Inf guards in Strategy, SPSC `size()`
  race, overflow-safe logger counters, multicast segfault, and ~30
  cppcheck/clang-tidy findings (narrowing conversions, uninit members,
  not-null-terminated results, inefficient vector ops).

## [v1.0]

Initial release — C++ HFT infrastructure lab: order-book matching engine,
OMS with risk checks and P&L, ITCH/FIX/OUCH parsers, mean-reversion strategy,
smart order router, trade logger, UDP multicast feed, monitoring, DPDK-bypass
simulator, cache-latency and kernel-tuning experiments.
