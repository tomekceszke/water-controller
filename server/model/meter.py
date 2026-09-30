#!/usr/bin/env python3
"""Telemetry against the utility's main water meter: per billing interval, per year, and for firmware 3.x.

  uv run --with-requirements server/model/requirements.txt server/model/meter.py

Needs server/model/meter_readings.csv (invoices.py, private) and read access to hc-data (raw and corrected pulses).
Prints the full table locally and writes out/meter_check.json with **ratios only** (telemetry / meter, implied
pulses per liter): the report publishes those, never the readings.

Calibration rule: once firmware 3.x (no counter wraps, no lost flows) has at least CALIBRATION_MIN_M3 of complete
meter intervals, a pulses-per-liter value more than CALIBRATION_TOLERANCE away from the setting is recommended.
"""
import json
import pathlib
import sys

import pandas as pd

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from data import HOST, PRODUCTION, _password  # noqa: E402
from features import OUT, RECONCILE_END, TZ, clean_flows, load_readings, reconcile_factors  # noqa: E402

K = 410                                    # pulses per liter in use (docs/CALIBRATION.md)
CALIBRATION_MIN_M3 = 36                    # 1 m³ reading resolution: about ±3 % at this volume
CALIBRATION_TOLERANCE = 0.03
SQL = """SELECT device, start_ts, stop_ts, pulses, pulses_corrected, pulses_per_liter, suspect FROM flow_event
         WHERE device IN ('bigquery', %s) AND start_ts >= %s AND start_ts < %s"""


def pulse_table(readings, flows):
    """Per interval: meter liters and telemetry pulses as raw, import-corrected and fully wrap-fixed."""
    f = flows.copy()
    f["raw"] = f["pulses"].astype(float)
    f["imp"] = f["pulses_corrected"].fillna(f["pulses"]).astype(float)
    ppl = f["pulses_per_liter"].fillna(K).astype(float)
    wrap = clean_flows(f.assign(liters=f["imp"] / ppl), readings=None)
    f["clean"] = wrap["liters"] * ppl
    f["fw3"] = f["device"] != "bigquery"
    rows = []
    for a, b, liters in zip(readings["start"], readings["end"], readings["liters"]):
        g = f[(f["start_ts"] >= a) & (f["start_ts"] < b)]
        rows.append({"start": a, "end": b, "meter_l": liters, "raw": g["raw"].sum(), "imp": g["imp"].sum(),
                     "clean": g["clean"].sum(), "fw3_only": bool(len(g)) and bool(g["fw3"].all())})
    t = pd.DataFrame(rows)
    for v in ("raw", "imp", "clean"):
        t[f"k_{v}"] = t[v] / t["meter_l"]           # implied pulses per liter
    return t


def summary(t):
    def ratio(dd, v):  # telemetry volume at K against the meter
        return round(float(dd[v].sum() / K / dd["meter_l"].sum()), 3) if len(dd) else None

    year = t["start"].dt.tz_convert(TZ).dt.year
    pre = t[t["end"] <= pd.Timestamp("2023-07-01", tz=TZ)]     # before the legacy wrap bug (first 2023-07-03)
    fw3 = t[t["fw3_only"]]
    out = {
        "intervals": len(t), "first": str(t["start"].min().tz_convert(TZ).date()),
        "last": str(t["end"].max().tz_convert(TZ).date()),
        "by_year": {int(y): {v: ratio(t[year == y], v) for v in ("raw", "imp", "clean")} for y in sorted(year.unique())},
        "pre_wrap_bug": {v: ratio(pre, v) for v in ("raw", "imp", "clean")},
        "overall": {v: ratio(t, v) for v in ("raw", "imp", "clean")},
        "interval_ratio_clean": [round(float(x), 3) for x in t["clean"] / K / t["meter_l"]],
        "interval_scatter_clean": round(float((t["clean"] / K / t["meter_l"]).std()), 3),
        "implied_k_2026": round(float(t.loc[year == 2026, "clean"].sum() / t.loc[year == 2026, "meter_l"].sum()), 1),
    }
    fw3_m3 = float(fw3["meter_l"].sum() / 1000)
    out["fw3"] = {"intervals": len(fw3), "enough": fw3_m3 >= CALIBRATION_MIN_M3}
    if out["fw3"]["enough"]:
        implied = float(fw3["raw"].sum() / fw3["meter_l"].sum())
        out["fw3"]["implied_k"] = round(implied, 1)
        out["fw3"]["recommend_k"] = round(implied) if abs(implied / K - 1) > CALIBRATION_TOLERANCE else None
    return out


def main():
    import psycopg
    readings = load_readings()
    if readings is None:
        raise SystemExit("server/model/meter_readings.csv missing: run invoices.py on the invoice PDFs first")
    lo, hi = readings["start"].min() - pd.Timedelta(days=1), readings["end"].max() + pd.Timedelta(days=1)
    with psycopg.connect(host=HOST, dbname="water", user="wc_read", password=_password(), connect_timeout=10) as c:
        cur = c.execute(SQL, (PRODUCTION, lo.to_pydatetime(), hi.to_pydatetime()))
        flows = pd.DataFrame(cur.fetchall(), columns=[d.name for d in cur.description])
    for col in ("start_ts", "stop_ts"):
        flows[col] = pd.to_datetime(flows[col], utc=True)
    flows["suspect"] = flows["suspect"].astype(bool)
    t = pulse_table(readings, flows)
    s = summary(t)
    view = t.assign(start=t["start"].dt.tz_convert(TZ).dt.date, end=t["end"].dt.tz_convert(TZ).dt.date)
    print(view[["start", "end", "meter_l", "k_raw", "k_imp", "k_clean", "fw3_only"]].round(1).to_string())
    print(json.dumps({k: v for k, v in s.items() if k != "interval_ratio_clean"}, indent=1))
    factors = reconcile_factors(clean_flows(
        flows.assign(liters=flows["pulses_corrected"].fillna(flows["pulses"]) / K), readings=None), readings)
    s["reconcile_factor"] = {"mean": round(float(factors["factor"].mean()), 3),
                             "min": round(float(factors["factor"].min()), 3),
                             "max": round(float(factors["factor"].max()), 3), "until": str(RECONCILE_END.date())}
    OUT.mkdir(exist_ok=True)
    (OUT / "meter_check.json").write_text(json.dumps(s, indent=1))


if __name__ == "__main__":
    main()
