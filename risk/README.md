# Risk Manager

Standalone pre-trade risk engine with circuit breaker and kill switch.

## Features
- **Per-symbol position limit**: Max shares in any single instrument
- **Portfolio exposure limit**: Max total absolute exposure across all symbols
- **Daily P&L loss limit**: Circuit breaker trips and activates kill switch
- **Drawdown limit**: Max drawdown from peak P&L triggers kill switch
- **Order rate limiting**: Max orders per second
- **Order value limit**: Max notional value per order
- **Kill switch**: Halts all trading instantly (manual or automatic)

## Run
```bash
# Standalone demo (200 orders, circuit breaker demo)
python3 risk/risk_manager.py

# Unit tests (10)
python3 tests/test_risk.py
```

## Performance
- Risk check latency: ~4,500 ns per order
- All checks run in a single pass (no early exit optimization needed at this latency)
