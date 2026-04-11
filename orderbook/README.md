# Order Book Matching Engine

C++ order book with price-time priority matching.

## Performance
- 23M orders/sec on 2-core VM
- 1M orders processed in 43ms
- Compiled with -O2 optimisation

## How it works
- Bids sorted highest-first (std::map with std::greater)
- Asks sorted lowest-first (std::map)
- Matching: when best bid >= best ask, trade executes

## Build & Run
```bash
