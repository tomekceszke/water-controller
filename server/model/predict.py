#!/usr/bin/env python3
"""Expected usage for an hour and the alarm thresholds that apply then; optionally score one flow.

  uv run --with-requirements server/model/requirements.txt server/model/predict.py "thu 17:00"
  uv run --with-requirements server/model/requirements.txt server/model/predict.py "2026-10-01 17:00"
  uv run --with-requirements server/model/requirements.txt server/model/predict.py --flow "tue 03:00" 15m 90

Needs out/hourly.joblib (train.py) and out/anomaly.joblib (anomaly.py).
"""
import argparse
import pathlib
import re
import sys

import joblib
import numpy as np
import pandas as pd

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from anomaly import FLOW_IND, band_of, flow_limits  # noqa: E402
from features import TZ, calendar  # noqa: E402

OUT = pathlib.Path(__file__).resolve().parent / "out"
DAYS = ["mon", "tue", "wed", "thu", "fri", "sat", "sun"]


def parse_when(text, now=None):
    """'2026-10-01 17:00', or '<weekday> HH[:MM]' for its next occurrence (local time)."""
    now = now or pd.Timestamp.now(tz=TZ)
    m = re.fullmatch(r"\s*([a-z]{3})[a-z]*\s+(\d{1,2})(?::(\d{2}))?\s*", text.lower())
    if m and m.group(1) in DAYS:
        day = DAYS.index(m.group(1))
        t = now.normalize() + pd.Timedelta(days=(day - now.dayofweek) % 7,
                                           hours=int(m.group(2)), minutes=int(m.group(3) or 0))
        return t if t > now else t + pd.Timedelta(days=7)
    t = pd.Timestamp(text)
    return t.tz_localize(TZ) if t.tzinfo is None else t.tz_convert(TZ)


def parse_duration(text):
    """'15m', '90s', '1h', '1h30m' or plain seconds."""
    if re.fullmatch(r"\d+(\.\d+)?", text):
        return float(text)
    parts = re.findall(r"(\d+(?:\.\d+)?)([hms])", text.lower())
    if not parts or "".join(a + b for a, b in parts) != text.lower():
        raise argparse.ArgumentTypeError(f"bad duration: {text}")
    return sum(float(v) * {"h": 3600, "m": 60, "s": 1}[u] for v, u in parts)


def expected(hourly, X):
    out = {}
    for target, b in hourly.items():
        out[target] = {"mean": float(b["mean"].predict(X)[0] * b["level"])}
        for a, q in sorted(b["quantiles"].items()):
            out[target][f"p{a * 100:g}"] = float(q.predict(X)[0] * b["level"])
    return out


def score_flow(anomaly, start, duration_s, liters):
    X = calendar(pd.DatetimeIndex([start.tz_convert("UTC")]))
    X["duration_s"], X["liters"] = duration_s, liters
    X["log_dur"], X["log_vol"] = np.log(duration_s), np.log(liters)
    X["rate"] = liters / (duration_s / 60)
    X["log_rate"] = np.log(max(X["rate"].iloc[0], 1e-3))
    lim = flow_limits(anomaly["flow_models"], anomaly["choice"], X).iloc[0]
    rows = []
    for ind, spec in FLOW_IND.items():
        v, t = X[spec["col"]].iloc[0], lim[ind]
        applies = duration_s >= spec["min_dur"]
        fired = applies and (v > t if spec["side"] == "upper" else v < t)
        rows.append((ind, float(np.exp(v)), float(np.exp(t)), applies, fired))
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("when", nargs="?", help="'2026-10-01 17:00' or 'thu 17:00'")
    ap.add_argument("--flow", nargs=3, metavar=("START", "DURATION", "LITERS"), help="score one flow")
    args = ap.parse_args()
    hourly = joblib.load(OUT / "hourly.joblib")
    anomaly = joblib.load(OUT / "anomaly.joblib")
    units = {"duration": "s", "volume": "L", "rate_high": "L/min", "rate_low": "L/min"}

    if args.flow:
        start = parse_when(args.flow[0])
        dur, liters = parse_duration(args.flow[1]), float(args.flow[2])
        print(f"Flow {start:%a %Y-%m-%d %H:%M}, {dur:.0f} s, {liters:g} L ({liters / dur * 60:.1f} L/min)")
        any_fired = False
        for ind, v, t, applies, fired in score_flow(anomaly, start, dur, liters):
            rel = ">" if FLOW_IND[ind]["side"] == "upper" else "<"
            state = "ALARM" if fired else ("ok" if applies else "n/a (too short)")
            print(f"  {ind:10} {v:9.1f} {units[ind]:5}  alarm if {rel} {t:.1f}  {state}")
            any_fired |= fired
        print("ANOMALY" if any_fired else "normal")
        return

    when = (parse_when(args.when) if args.when else pd.Timestamp.now(tz=TZ)).floor("h")
    X = calendar(pd.DatetimeIndex([when.tz_convert("UTC")]))
    e = expected(hourly, X)
    print(f"{when:%a %Y-%m-%d %H:00} ({TZ})")
    for target, label in (("liters", "liters in the hour"), ("flows", "flows in the hour")):
        v = e[target]
        print(f"  {label:20} expected {v['mean']:6.1f}   " + "  ".join(
            f"{k} {x:.1f}" for k, x in v.items() if k != "mean"))
    X["duration_s"] = X["liters"] = 1.0
    lim = flow_limits(anomaly["flow_models"], anomaly["choice"], X).iloc[0]
    print("  alarm for one flow: longer than {:.0f} s, more than {:.1f} L, faster than {:.1f} L/min, "
          "slower than {:.2f} L/min (flows >= 5 min)".format(*(np.exp(lim[i]) for i in FLOW_IND)))
    a, m = anomaly["choice"]["hour_liters"][band_of(when.hour).item()]
    b = hourly["liters"]
    print(f"  alarm for the hour: more than {max(b['quantiles'][a].predict(X)[0] * b['level'], 1) * np.exp(m):.0f} L")
    print(f"  night (00-06): alarm above {anomaly['choice']['night_flows']['threshold']} flows")


if __name__ == "__main__":
    main()
