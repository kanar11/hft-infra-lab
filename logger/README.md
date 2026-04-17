# Trade Logger / Audit Trail

Thread-safe trade event logger with nanosecond timestamps.
*Bezpieczny wątkowo logger zdarzeń handlowych z nanosekundowymi znacznikami czasu.*

## Performance Comparison / Porównanie wydajności

| Metric | Python | C++ |
|--------|--------|-----|
| **Throughput** | ~200K events/sec | **14.3M events/sec** (71x faster) |
| **Latency (p50)** | ~4,700 ns | **41 ns** |
| **Latency (p99)** | ~8,000 ns | **60 ns** |

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

## Files / Pliki

| File | Description |
|------|-------------|
| `trade_logger.py` | Python implementation (reference, with beginner comments) |
| `trade_logger.hpp` | C++ header-only implementation |
| `logger_demo.cpp` | C++ demo with 30 unit tests + throughput benchmark |
| `../tests/test_logger.py` | 10 Python unit tests |

## Run / Uruchomienie

```bash
# Python
python3 logger/trade_logger.py
python3 tests/test_logger.py

# C++ (build + run)
make build
./logger/logger_demo              # tests + benchmark (500K events)
./logger/logger_demo 2000000      # 2M events
```

## In Real HFT / W prawdziwym HFT

| This Lab | Production |
|----------|------------|
| Python list + mutex | Lock-free ring buffer in shared memory |
| `time.time_ns()` | Hardware TSC (RDTSC instruction) |
| CSV file output | Binary log + async flush to SSD |
| Single process | Separate logger process (no latency on hot path) |
