# Trade Logger / Audit Trail

Thread-safe trade event logger with nanosecond timestamps.
*Bezpieczny wątkowo logger zdarzeń handlowych z nanosekundowymi znacznikami czasu.*

## Why This Exists / Dlaczego to istnieje

Every HFT firm must keep a complete audit trail of all trading activity.
Regulators (SEC, MiFID II) require that every order, fill, cancel, and risk
rejection is logged with precise timestamps and can be replayed later.

*Każda firma HFT musi prowadzić pełną ścieżkę audytu całej aktywności handlowej.
Regulatorzy (SEC, MiFID II) wymagają, aby każde zlecenie, realizacja, anulowanie
i odrzucenie ryzyka były logowane z precyzyjnymi znacznikami czasu.*

## Event Types / Typy zdarzeń

| Event | Description | Opis |
|-------|-------------|------|
| `ORDER_SUBMIT` | Strategy sends order | Strategia wysyła zlecenie |
| `RISK_ACCEPT` | Risk manager approved | Menedżer ryzyka zatwierdził |
| `RISK_REJECT` | Risk manager blocked | Menedżer ryzyka zablokował |
| `ORDER_FILL` | Exchange filled order | Giełda zrealizowała zlecenie |
| `ORDER_PARTIAL` | Partial fill received | Częściowa realizacja |
| `ORDER_CANCEL` | Order cancelled | Zlecenie anulowane |
| `KILL_SWITCH` | Emergency stop | Wyłącznik awaryjny |
| `SYSTEM_START` | Session opened | Sesja otwarta |
| `SYSTEM_STOP` | Session closed | Sesja zamknięta |

## Features / Funkcje

- **Nanosecond timestamps** via `time.time_ns()` — precise event ordering
- **Sequence numbers** — monotonic counter guarantees ordering even with same timestamp
- **Thread-safe** — mutex lock protects concurrent writes
- **CSV export** — optional file output for audit compliance
- **Filtering** — query by order ID, event type, or symbol (like `grep`)
- **Order trail** — full lifecycle of any order in one call

## Usage / Użycie

```bash
# Run demo
python3 logger/trade_logger.py

# Run tests
python3 tests/test_logger.py
```

## In Real HFT / W prawdziwym HFT

| This Lab | Production |
|----------|------------|
| Python list + mutex | Lock-free ring buffer in shared memory |
| `time.time_ns()` | Hardware TSC (RDTSC instruction) |
| CSV file output | Binary log + async flush to SSD |
| Single process | Separate logger process (no latency on hot path) |

## Files / Pliki

| File | Description |
|------|-------------|
| `trade_logger.py` | Logger module with TradeLogger class |
| `../tests/test_logger.py` | 10 unit tests |
