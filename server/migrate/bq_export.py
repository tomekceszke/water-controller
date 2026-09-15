#!/usr/bin/env python3
"""One-off export of the GCP flow history (water-controller-351109.flow_ds.flow_raw) to server/migrate/out/.

The legacy firmware posted one row per flow: start_time, stop_time (TIMESTAMP, UTC), consumption (pulses).
Credentials: the device service account key of the legacy firmware (read-only use).

  uv run --with google-cloud-bigquery server/migrate/bq_export.py [path/to/device_key.pem]
"""
import csv
import gzip
import json
import pathlib
import sys

from google.cloud import bigquery
from google.oauth2 import service_account

PROJECT = "water-controller-351109"
TABLE = f"{PROJECT}.flow_ds.flow_raw"
SERVICE_ACCOUNT = "6105493247-compute@developer.gserviceaccount.com"
ROOT = pathlib.Path(__file__).resolve().parents[2]
KEY = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "legacy/water-controller-5.1.1/certs/water_controller_1e47.pem"
OUT = pathlib.Path(__file__).resolve().parent / "out"


def main():
    creds = service_account.Credentials.from_service_account_info({
        "type": "service_account", "project_id": PROJECT, "private_key": KEY.read_text(),
        "client_email": SERVICE_ACCOUNT, "token_uri": "https://oauth2.googleapis.com/token",
    })
    client = bigquery.Client(project=PROJECT, credentials=creds)
    OUT.mkdir(exist_ok=True)
    table = client.get_table(TABLE)
    count, first, last = 0, None, None
    with gzip.open(OUT / "flow_raw.csv.gz", "wt", newline="") as f:
        w = csv.writer(f)
        w.writerow(["start_time", "stop_time", "consumption"])
        for row in client.list_rows(table, page_size=100_000):     # tabledata.list: no query cost
            w.writerow([row["start_time"].isoformat(), row["stop_time"].isoformat(), row["consumption"]])
            count += 1
            first = row["start_time"] if first is None else min(first, row["start_time"])
            last = row["start_time"] if last is None else max(last, row["start_time"])
    summary = {"rows": count, "table_num_rows": table.num_rows,
               "first": first.isoformat() if first else None, "last": last.isoformat() if last else None}
    (OUT / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary))


if __name__ == "__main__":
    main()
