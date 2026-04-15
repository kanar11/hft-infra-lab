# Smart Order Router (SOR)

Routes orders to the best execution venue based on price, latency, and liquidity.

## Routing Strategies
- **BEST_PRICE**: Route to venue with best bid/ask (ties broken by fee, then latency)
- **LOWEST_LATENCY**: Route to fastest venue (for latency-sensitive strategies)
- **SPLIT**: Split large orders across venues proportional to available liquidity

## Venues
Each venue has: name, round-trip latency (ns), fee per share, live bid/ask quotes.
Negative fees represent maker rebates (common on electronic exchanges).

## Run
```bash
# Standalone demo (3 venues, 200 mixed orders)
python3 router/smart_router.py

# Unit tests (10)
python3 tests/test_router.py
```

## Performance
- Routing decision latency: ~3,000 ns per order
- Supports venue deactivation (circuit breaker integration)
- Split orders fill across venues up to available liquidity
