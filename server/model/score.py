#!/usr/bin/env python3
"""Hourly check on hc-data: liters in each of the last complete hours against the learned hour limit.

  WC_PG_DSN="dbname=water" server/model/score.py [--hours 3]

The device checks single flows live (learned.c); what it cannot see is a total over an hour made of many normal
looking flows (a hose with pauses), and public holidays, which it treats as ordinary weekdays. Alarms go to the
model_alert table (one per hour and kind, so re-runs do not repeat them) and to ntfy when NTFY_URL is set.
"""
import argparse
import json
import pathlib
import sys

import pandas as pd

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from data import PRODUCTION  # noqa: E402
from features import OUT, TZ, calendar, clean_flows, spread_hourly  # noqa: E402
from publish import DAYS, notify  # noqa: E402

SQL = """
SELECT device, start_ts, stop_ts, liters, suspect FROM flow
WHERE device = %s AND stop_ts >= %s AND start_ts < %s
"""


def hour_alarms(liters, table):
    """liters: Series of liters per UTC hour. Returns [(hour, liters, limit)] over the hour limit."""
    rows = {(r["profile"], r["hour"]): r for r in table}
    cal = calendar(liters.index)
    out = []
    for ts, v in liters.items():
        c = cal.loc[ts]
        profile = "holiday" if c["holiday"] else DAYS[int(c["dow"])]
        limit = rows[(profile, int(c["hour"]))].get("alarm_hour_liters")
        if limit is not None and v > limit:
            out.append((ts, float(v), float(limit)))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--hours", type=int, default=3, help="complete hours to check (re-runs are deduplicated)")
    args = ap.parse_args()
    import psycopg

    table = json.loads((OUT / "thresholds.json").read_text())["table"]
    end = pd.Timestamp.now(tz="UTC").floor("h")
    index = pd.date_range(end - pd.Timedelta(hours=args.hours), end, freq="h", inclusive="left")
    with psycopg.connect("dbname=water") as conn:
        cur = conn.execute(SQL, (PRODUCTION, index[0].to_pydatetime(), end.to_pydatetime()))
        flows = pd.DataFrame(cur.fetchall(), columns=[d.name for d in cur.description])
        if flows.empty:
            return
        for c in ("start_ts", "stop_ts"):
            flows[c] = pd.to_datetime(flows[c], utc=True)
        flows["liters"] = flows["liters"].astype(float)
        liters = spread_hourly(clean_flows(flows), index)
        for ts, v, limit in hour_alarms(liters, table):
            new = conn.execute(
                "INSERT INTO model_alert (device, hour, kind, value, usual_limit) VALUES (%s, %s, 'hour_liters', %s, %s)"
                " ON CONFLICT DO NOTHING RETURNING 1", (PRODUCTION, ts.to_pydatetime(), v, limit)).fetchone()
            if new:
                local = ts.tz_convert(TZ)
                msg = (f"{v:.0f} L between {local:%H}:00 and {local + pd.Timedelta(hours=1):%H}:00, "
                       f"usually at most {limit:.0f} L")
                print(msg)
                notify("Unusual water use", msg)


if __name__ == "__main__":
    main()
