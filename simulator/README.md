# Market Data Simulator / Symulator Danych Rynkowych

End-to-end HFT pipeline that connects all modules together.
*Pełny potok HFT łączący wszystkie moduły.*

## Pipeline / Potok
```
ITCH Generator → ITCH Parser → [Strategy] → [Router] → OMS (risk checks) → Fill Engine → P&L
```

*Generator ITCH → Parser ITCH → [Strategia] → [Router] → OMS (kontrole ryzyka) → Silnik wypełniania → Zysk i strata.*

## What It Does / Co robi

1. Generates realistic ITCH 5.0 binary messages (ADD_ORDER, EXECUTE, CANCEL, TRADE, SYSTEM_EVENT)
*Generuje realistyczne wiadomości binarne ITCH 5.0 (ADD_ORDER, EXECUTE, CANCEL, TRADE, SYSTEM_EVENT).*

2. Parses them through the ITCH parser
*Analizuje je za pomocą parsera ITCH.*

3. Optionally feeds prices to the Mean Reversion Strategy for signal generation
*Opcjonalnie przekazuje ceny do strategii powrotu do średniej w celu generowania sygnałów.*

4. Optionally routes orders through the Smart Order Router (venue selection)
*Opcjonalnie kieruje zlecenia przez inteligentny router zleceń (wybór giełdy).*

5. Routes orders through the OMS with pre-trade risk checks
*Kieruje zlecenia przez system zarządzania zleceniami z wstępnymi kontrolami ryzyka.*

6. Tracks positions and realized P&L per symbol
*Śledzi pozycje i zrealizowany zysk/stratę na symbol.*

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
