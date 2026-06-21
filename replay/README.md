# Real-data replay

Drives the lab's pipeline (parser → OMS → logger) from **actual
historical Nasdaq order events** instead of the synthetic LCG-generated
stream in `simulator/`.

## Data source: LOBSTER

[LOBSTER](https://lobsterdata.com) reconstructs Nasdaq ITCH order events
into a documented CSV format. They publish free sample days for a handful
of tickers — drop in any of those files and `lobster_demo` will replay
them through the same OMS, Risk, Logger, and Strategy that the synthetic
simulator uses.

### Sample files shipped with the repo

- **`sample_aapl.csv`** — 20 hand-crafted events. Smoke-test sized, CI
  runs it in <1ms.
- **`generated_aapl_day.csv`** — 5000 synthetic events (~210 KB,
  bundled in the repo). Represents ~50 seconds of trading with realistic
  distributions (Poisson inter-arrival, ~50%/20%/15%/14%/1% mix of submit/
  cancel/delete/execute/halt, discrete sizes with a round-lot preference).
  CI uses this file as a "longer pipeline test" — we push 5k events
  through the OMS + Logger to confirm nothing breaks at a larger scale.

### Generating your own fixtures

If you want a different size / symbol / seed:

```bash
python3 replay/gen_fixture.py                    # default: 10000 events, AAPL
python3 replay/gen_fixture.py -n 50000           # 50k events
python3 replay/gen_fixture.py -n 100000 -s MSFT  # 100k MSFT
python3 replay/gen_fixture.py -o my_day.csv      # custom output
```

The output is **deterministic for a fixed seed** (default=42), so you can
use it for regression tests without worrying that the file will change.

Distribution details: see the docstring in `replay/gen_fixture.py`.

### Run the real thing

1. Download a free message-file sample from <https://lobsterdata.com/info/DataSamples.php>
   (e.g. `AAPL_2012-06-21_34200000_57600000_message_5.csv`).
2. Build the lab: `make build`.
3. Replay:

   ```
   ./replay/lobster_demo /path/to/AAPL_2012-06-21_messages.csv
   ```

   The demo extracts the ticker from the filename, opens an `OMS`, opens
   a `TradeLogger`, and pushes every CSV row through. At the end it
   prints how many submits / executes / cancels were processed.

## CSV format (no header)

| Column | Meaning                                  |
|--------|------------------------------------------|
| 1      | timestamp — seconds since 09:30:00 ET    |
| 2      | event_type (see table)                   |
| 3      | order_id (exchange-side)                 |
| 4      | size (shares)                            |
| 5      | price (dollars × 10000, e.g. 2238100 = $223.81) |
| 6      | direction (+1 buy, -1 sell)              |

`event_type` values:

| Code | Meaning                       | OMS action       |
|------|-------------------------------|------------------|
| 1    | submit limit order            | `submit_order`   |
| 2    | partial cancel                | `cancel_order`   |
| 3    | full delete                   | `cancel_order`   |
| 4    | visible execution             | `fill_order`     |
| 5    | hidden execution              | (skipped)        |
| 6    | cross trade                   | (skipped)        |
| 7    | trading halt                  | (counted, no OMS call) |

## Why this matters

The synthetic simulator demonstrates *throughput* on fake data. The
LOBSTER replay demonstrates that the **same pipeline** handles the
event mix and order-id semantics of real Nasdaq data — irregular
inter-arrival times, cancels that arrive long after the submit they
target, executions that partially consume a resting order. Nothing in
the OMS / Logger / Risk path changes between the two; only the source
of events differs.
