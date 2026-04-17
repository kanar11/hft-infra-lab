# System Architecture / Architektura systemu

## Data Flow Pipeline / Przepływ danych

This is the complete path a trade takes through the system — from raw market data
arriving on the wire to a filled order with P&L calculated.

*To jest pełna ścieżka, jaką przechodzi transakcja przez system — od surowych danych
rynkowych przychodzących po kablu do zrealizowanego zlecenia z obliczonym P&L.*

```
                         HFT Infrastructure Lab — Full Pipeline
                         ======================================

  ┌─────────────┐     ┌──────────────┐     ┌──────────────┐     ┌─────────────┐
  │   Exchange   │────▶│  Multicast   │────▶│  ITCH 5.0    │────▶│  Order      │
  │  (NASDAQ)    │     │  Receiver    │     │  Parser      │     │  Book       │
  │              │     │              │     │              │     │  (C++)      │
  │  Sends ITCH  │     │  UDP socket  │     │  Binary →    │     │  Matching   │
  │  binary feed │     │  multicast   │     │  structured  │     │  engine     │
  └─────────────┘     │  group join  │     │  data        │     │  17.8M/sec  │
                      └──────────────┘     └──────────────┘     └──────┬──────┘
                                                                       │
                       Market data path (< 1μs total)                  │ Price updates
                       Ścieżka danych rynkowych (< 1μs łącznie)       │ + book state
                                                                       ▼
  ┌─────────────┐     ┌──────────────┐     ┌──────────────┐     ┌─────────────┐
  │   Exchange   │◀────│  FIX 4.2     │◀────│  OMS         │◀────│  Strategy   │
  │  (NASDAQ)    │     │  Sender      │     │              │     │  (Mean Rev) │
  │              │     │              │     │  Order        │     │             │
  │  Receives    │     │  Structured  │     │  lifecycle   │     │  Signal     │
  │  our orders  │     │  → FIX tags  │     │  management  │     │  generation │
  └─────────────┘     └──────────────┘     └──────┬───────┘     └──────┬──────┘
                                                   │                    │
                       Order path (< 5μs total)    │                    │
                       Ścieżka zlecenia             │                    │
                                                   ▼                    │
                                            ┌──────────────┐            │
                                            │  Risk        │◀───────────┘
                                            │  Manager     │
                                            │              │
                                            │  Pre-trade   │
                                            │  checks      │
                                            └──────┬───────┘
                                                   │
                                                   ▼
                      ┌──────────────┐     ┌──────────────┐
                      │  Trade       │◀────│  Smart       │
                      │  Logger      │     │  Router      │
                      │              │     │              │
                      │  Audit trail │     │  Venue       │
                      │  (CSV/mem)   │     │  selection   │
                      └──────────────┘     └──────────────┘
```

## Component Details / Szczegóły komponentów

### Market Data Path / Ścieżka danych rynkowych

The market data path is the **hot path** — it must be as fast as possible because
every nanosecond of delay means worse prices.

*Ścieżka danych rynkowych to **gorąca ścieżka** — musi być jak najszybsza, bo
każda nanosekunda opóźnienia oznacza gorsze ceny.*

```
Exchange ITCH feed (binary, UDP multicast)
    │
    ▼
Multicast Receiver (multicast/mc_receiver.py)
    │  Joins multicast group 239.1.1.1:5001
    │  Measures receive latency (μs precision)
    │
    ▼
ITCH 5.0 Parser
    ├── Python (itch_parser/itch_parser.py)     ~1M msg/sec
    └── C++    (itch_parser/itch_parser.hpp)    60M msg/sec, 16ns/msg
    │
    │  Parses 9 message types: A, F, D, U, E, C, P, S, R
    │  Big-endian byte swapping, zero-copy where possible
    │
    ▼
Order Book (orderbook/orderbook_v2.cpp)
    │  std::map price levels + unordered_map index
    │  17.8M orders/sec, p50=50ns, p99=130ns
    │  Fixed-point int64 prices (no floating point on hot path)
    │
    ▼
Strategy (strategy/mean_reversion.py)
       SMA crossover signals, ~2300ns decision latency
```

### Order Path / Ścieżka zlecenia

```
Strategy generates signal (BUY/SELL)
    │
    ▼
Risk Manager (risk/risk_manager.py)
    │  ✓ Order value limit
    │  ✓ Position limit (per-symbol + portfolio)
    │  ✓ Circuit breaker (daily loss limit)
    │  ✓ Kill switch (emergency halt)
    │  ✓ Rate limiting (orders/second)
    │  ✓ Drawdown tracking
    │
    ▼ (ACCEPT or REJECT)
    │
Smart Order Router (router/smart_router.py)
    │  Strategies: best_price, lowest_latency, split
    │  Venue selection based on price, latency, fees
    │
    ▼
OMS — Order Management System (oms/oms.py)
    │  Order lifecycle: PENDING → SUBMITTED → FILLED/CANCELLED
    │  Position tracking with average cost basis
    │  Real-time P&L calculation
    │
    ▼
OUCH 4.2 Encoder (ouch-protocol/ouch_sender.py)
    │  Structured → binary (1.7M msg/sec)
    │
    ▼
FIX 4.2 Sender (fix-protocol/fix_parser.py)
    │  Structured → FIX tag=value format
    │
    ▼
Exchange receives our order
```

### Support Systems / Systemy wspomagające

```
Trade Logger (logger/trade_logger.py)
    │  Records every event with nanosecond timestamps
    │  Audit trail for regulatory compliance (SEC, MiFID II)
    │  CSV export, filtering by order/symbol/type
    │
Monitoring (monitoring/infra_monitor.py)
    │  CPU, memory, network, disk monitoring
    │  Alert thresholds with notifications
    │
Infrastructure
    ├── DPDK Bypass (dpdk-bypass/)       5.6x latency reduction
    ├── Lock-free Queue (lockfree/)      17.6M msg/sec SPSC
    ├── Cache Latency (memory-latency/)  L1=1.6ns, L2=4.3ns, L3=154ns
    ├── Kernel Config (kernel-config/)   hugepages, CPU isolation, IRQ affinity
    └── Linux Tuning (linux-tuning/)     sysctl, NUMA, scheduler tuning
```

## Latency Budget / Budżet opóźnień

Where time is spent in the pipeline (approximate):
*Gdzie jest spędzany czas w potoku (przybliżone):*

```
Component                  Latency        Cumulative
─────────────────────────  ─────────────  ──────────
Network receive (UDP)      ~1,000 ns      1,000 ns
ITCH parse (C++)               16 ns      1,016 ns
Orderbook update               50 ns      1,066 ns
Strategy decision           2,300 ns      3,366 ns
Risk check                    100 ns      3,466 ns
Router selection              200 ns      3,666 ns
OMS submit                    500 ns      4,166 ns
OUCH encode                   590 ns      4,756 ns
Network send (TCP)         ~1,000 ns      5,756 ns
                           ─────────────
Total tick-to-trade:       ~5.8 μs
```

> Production HFT firms achieve 1-10 μs tick-to-trade with FPGA + kernel bypass.
> Our ~5.8 μs is competitive for a software-only VM-based implementation.
>
> Produkcyjne firmy HFT osiągają 1-10 μs z FPGA + kernel bypass.
> Nasze ~5.8 μs jest konkurencyjne dla implementacji czysto programowej na VM.

## Technology Stack / Stos technologii

```
┌────────────────────────────────────────────┐
│              Application Layer              │
│  Python: OMS, Risk, Router, Strategy, ITCH │
│  C++: Orderbook, ITCH, SPSC Queue, Cache   │
├────────────────────────────────────────────┤
│               OS / Kernel Layer             │
│  Red Hat EL10, hugepages, CPU isolation,   │
│  IRQ affinity, sysctl tuning               │
├────────────────────────────────────────────┤
│              Hardware Layer                 │
│  VirtualBox VM, 2 CPU, 4GB RAM             │
│  (Production: bare metal, NUMA, 10GbE)     │
└────────────────────────────────────────────┘
```
