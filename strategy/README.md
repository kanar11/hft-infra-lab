# Mean Reversion Strategy / Strategia Powrotu do Średniej

Simple mean reversion strategy for the HFT pipeline demo.
*Prosta strategia powrotu do średniej dla demonstracji potoku HFT.*

## Logic / Logika
- Tracks a rolling Simple Moving Average (SMA) per stock
- When price deviates from SMA by more than a threshold:
  - Price > SMA + 0.1% → SELL (overpriced, expect drop)
  - Price < SMA - 0.1% → BUY (underpriced, expect rise)
  - Otherwise → HOLD (no signal)

*Śledzi toczącą się prostą średnią ruchomą (SMA) na akcję. Gdy cena odbiega od SMA o więcej niż próg: Cena > SMA + 0,1% → SPRZEDAJ (przewartościowana, oczekuj spadku). Cena < SMA - 0,1% → KUP (niedowartościowana, oczekuj wzrostu). W przeciwnym razie → CZEKAJ (brak sygnału).*

## Parameters / Parametry
- Window: 20 ticks (SMA lookback)
- Threshold: 0.1% deviation from SMA
- Order size: 100 shares per signal

*Okno: 20 tików (SMA lookback). Próg: odchylenie 0,1% od SMA. Rozmiar zlecenia: 100 akcji na sygnał.*

## Run
```bash
# Standalone demo (200 synthetic AAPL ticks)
python3 strategy/mean_reversion.py

# Integrated with full pipeline (10K ITCH messages, 8 stocks)
python3 simulator/market_sim.py 10000 --strategy

# Compare: direct mode (no strategy) vs strategy mode
python3 simulator/market_sim.py 10000
python3 simulator/market_sim.py 10000 --strategy
```

## Performance
- Decision latency: ~2,300 ns per tick
- Signal rate: ~74% (depends on market volatility)
- Uses `collections.deque` with fixed maxlen for O(1) append and bounded memory
