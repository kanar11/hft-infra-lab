# Market Data Simulator

End-to-end HFT pipeline that connects all modules together.

## Pipeline
```
ITCH Generator → ITCH Parser → [Strategy] → [Router] → OMS (risk checks) → Fill Engine → P&L
```

## What It Does
1. Generates realistic ITCH 5.0 binary messages (ADD_ORDER, EXECUTE, CANCEL, TRADE, SYSTEM_EVENT)
2. Parses them through the ITCH parser
3. Optionally feeds prices to the Mean Reversion Strategy for signal generation
4. Optionally routes orders through the Smart Order Router (venue selection)
5. Routes orders through the OMS with pre-trade risk checks
6. Tracks positions and realized P&L per symbol

## Run
```bash
# Default: 10,000 messages, direct mode
make simulate

# Custom message count
python3 simulator/market_sim.py 50000

# With mean reversion strategy
python3 simulator/market_sim.py 10000 --strategy

# With strategy + smart order routing
python3 simulator/market_sim.py 10000 --strategy --router

# Router only (no strategy, all orders routed through SOR)
python3 simulator/market_sim.py 10000 --router
```

## Performance (Red Hat EL10, VirtualBox 2-core VM)
- End-to-end: ~90K msg/sec (generate + parse + strategy + router + OMS)
- Simulates 8 NASDAQ stocks with realistic price distributions
- Strategy signal rate: ~75% (mean reversion)
- Router decision latency: ~2,500 ns
