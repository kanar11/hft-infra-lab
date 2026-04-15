# Smart Order Router (SOR) / Inteligentny Router Zleceń

Routes orders to the best execution venue based on price, latency, and liquidity.
*Kieruje zlecenia do najlepszej giełdy wykonania w oparciu o cenę, opóźnienie i płynność.*

## Routing Strategies / Strategie Routingu
- **BEST_PRICE**: Route to venue with best bid/ask (ties broken by fee, then latency)
*Kieruje do giełdy z najlepszą ceną kupna/sprzedaży (remisy rozstrzygane przez opłatę, następnie opóźnienie).*

- **LOWEST_LATENCY**: Route to fastest venue (for latency-sensitive strategies)
*Kieruje do najszybszej giełdy (dla strategii wrażliwych na opóźnienia).*

- **SPLIT**: Split large orders across venues proportional to available liquidity
*Dzieli duże zlecenia między giełdy proporcjonalnie do dostępnej płynności.*

## Venues
Each venue has: name, round-trip latency (ns), fee per share, live bid/ask quotes.
Negative fees represent maker rebates (common on electronic exchanges).

## Run
```bash
# Standalone demo (3 venues, 200 mixed orders)
python3 router/smart_router.py

# Unit tests (10)
python3 tests/test_router.py

# Integrated with full pipeline
python3 simulator/market_sim.py 10000 --strategy --router
```

## Performance
- Routing decision latency: ~3,000 ns per order
- Supports venue deactivation (circuit breaker integration)
- Split orders fill across venues up to available liquidity
