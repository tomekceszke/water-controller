#!/usr/bin/env python3
"""Flow history from hc-data (read-only role wc_read), cached in server/model/data/flows.parquet.

  uv run --with-requirements server/model/requirements.txt server/model/data.py [--refresh]

The password is PG_READ_PASS from server/secrets.env (or the environment).
"""
import argparse
import os
import pathlib

import pandas as pd

HERE = pathlib.Path(__file__).resolve().parent
CACHE = HERE / "data" / "flows.parquet"
SECRETS = HERE.parent / "secrets.env"
HOST = "192.168.11.16"
PRODUCTION = "30aea40aba44"  # the spare board's test flows (2026-09-15) are left out

SQL = """
SELECT device, start_ts, stop_ts, liters, max_lpm, closed, suspect
FROM flow
WHERE device IN ('bigquery', %s)
ORDER BY start_ts
"""


def _password():
    if "PG_READ_PASS" in os.environ:
        return os.environ["PG_READ_PASS"]
    for line in SECRETS.read_text().splitlines():
        if line.startswith("PG_READ_PASS="):
            return line.split("=", 1)[1].strip()
    raise SystemExit(f"PG_READ_PASS missing in {SECRETS}")


def fetch():
    import psycopg
    with psycopg.connect(host=HOST, dbname="water", user="wc_read", password=_password(), connect_timeout=10) as c:
        cur = c.execute(SQL, (PRODUCTION,))
        cols = [d.name for d in cur.description]
        df = pd.DataFrame(cur.fetchall(), columns=cols)
    for c in ("start_ts", "stop_ts"):
        df[c] = pd.to_datetime(df[c], utc=True)
    df["liters"] = df["liters"].astype(float)
    df["max_lpm"] = df["max_lpm"].astype(float)
    df["suspect"] = df["suspect"].astype(bool)
    return df


def load_flows(refresh=False):
    if refresh or not CACHE.exists():
        df = fetch()
        CACHE.parent.mkdir(exist_ok=True)
        df.to_parquet(CACHE, index=False)
    return pd.read_parquet(CACHE)


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--refresh", action="store_true", help="download again from hc-data")
    df = load_flows(ap.parse_args().refresh)
    print(f"{len(df)} flows, {df['start_ts'].min()} .. {df['stop_ts'].max()}, cache {CACHE}")
