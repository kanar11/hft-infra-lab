# Risk Manager / Zarządca Ryzyka

Standalone pre-trade risk engine with circuit breaker and kill switch.
*Autonomiczny silnik kontroli ryzyka przed zawarciem transakcji z wyłącznikiem obwodu i przyciskiem wyłączenia.*

## Features / Funkcje
- **Per-symbol position limit**: Max shares in any single instrument
*Limit pozycji na symbol: maksymalna liczba akcji w dowolnym instrumencie.*

- **Portfolio exposure limit**: Max total absolute exposure across all symbols
*Limit ekspozycji portfela: maksymalna całkowita ekspozycja bezwzględna na wszystkie symbole.*

- **Daily P&L loss limit**: Circuit breaker trips and activates kill switch
*Limit dzienny straty zysku/straty: przełącznik obwodu się włącza i aktywuje przycisk wyłączenia.*

- **Drawdown limit**: Max drawdown from peak P&L triggers kill switch
*Limit drawdownu: maksymalny drawdown od szczytowego zysku/straty uruchamia przycisk wyłączenia.*

- **Order rate limiting**: Max orders per second
*Ograniczenie szybkości zleceń: maksymalna liczba zleceń na sekundę.*

- **Order value limit**: Max notional value per order
*Limit wartości zlecenia: maksymalna wartość nominalna na zlecenie.*

- **Kill switch**: Halts all trading instantly (manual or automatic)
*Przycisk wyłączenia: natychmiast wstrzymuje całą transakcję (ręcznie lub automatycznie).*

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
