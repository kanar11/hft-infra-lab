# Market Data Simulator

End-to-end HFT pipeline that connects all modules together.

## Pipeline
```
ITCH Generator → ITCH Parser → OMS (risk checks) → Fill Engine → P&L
```

## What It Does
1. Generates realistic ITCH 5.0 binary messages (ADD_ORDER, EXECUTE, CANCEL, TRADE, SYSTEM_EVENT)
2. Parses them through the ITCH parser
3. Routes orders through the OMS with pre-trade risk checks
4. Tracks positions and realized P&L per symbol

## Run
```bash
# Default: 10,000 messages
make simulate

# Custom message count
python3 simulator/market_sim.py 50000
```

## Performance (Red Hat EL10, VirtualBox 2-core VM)
- End-to-end: ~130K msg/sec (generate + parse + OMS)
- Simulates 8 NASDAQ stocks with realistic price distributions
