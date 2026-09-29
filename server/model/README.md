# Usage model and anomaly detector

Offline analysis on the hc-data flow history: an hourly usage forecast and per-flow / per-hour anomaly thresholds.
Results and method: [`docs/USAGE_MODEL.md`](../../docs/USAGE_MODEL.md). Nothing here runs on the device or is needed
for protection.

## Run

Needs `PG_READ_PASS` in `server/secrets.env` (role `wc_read`, SELECT only) and access to 192.168.11.16.

```sh
R="uv run --with-requirements server/model/requirements.txt"
$R server/model/data.py --refresh   # flows -> server/model/data/flows.parquet (cache, gitignored)
$R server/model/train.py            # hourly models: selection, test, final fit     (~1.5 min)
$R server/model/anomaly.py          # thresholds: calibration, synthetic leaks, test (~1.5 min)
$R server/model/report.py           # public: docs/USAGE_MODEL.md, docs/img/model-*.png, out/report_public.html
                                    # private, exact dates: out/USAGE_MODEL_full.md, out/report.html
$R python -m unittest server/model/test_model.py
```

Outputs in `server/model/out/` (gitignored): `hourly.joblib`, `anomaly.joblib`, `thresholds.json` (per hour of week:
expected liters and flows, alarm thresholds), `metrics_hourly.json`, `metrics_anomaly.json`.

## Ask it

```sh
$R server/model/predict.py "thu 17:00"                 # expected usage and thresholds for that hour
$R server/model/predict.py "2026-12-24 19:00"
$R server/model/predict.py --flow "tue 03:00" 15m 90   # score one flow: start, duration, liters
```

## Files

| File | Role |
|---|---|
| `data.py` | read `flow` from hc-data, cache |
| `features.py` | wrap fix, hourly grid, coverage mask, calendar, split |
| `models.py` | hour-of-week tables, Poisson GLM, gradient boosting behind one interface |
| `train.py` | hourly liters / flows: candidates, training window, level correction, quantiles |
| `anomaly.py` | flow and hour indicators, calibration to a false-alarm budget, synthetic leaks, Mahalanobis comparison |
| `predict.py` | CLI on the saved models |
| `report.py`, `report_template.html` | the markdown report, charts and the interactive HTML page |
| `known_events.csv` | **gitignored**: unusual periods kept out of "normal" training (lawn 2023, pool, hose) with exact dates; label the `unlabelled` rows |
| `known_events.example.csv` | its format, with month-level placeholder dates (used when the real file is missing) |

## Privacy

Water use per hour describes how a household lives. Everything committed is aggregated or coarsened: the typical
week is a model average over years, single events and alarms are shown by month (alarms also night / day), and the
example week in the charts is stitched from days of different weeks with weekday labels only. The raw flows
(`data/`), the models and the exact-date reports (`out/`) and `known_events.csv` never go to git.
