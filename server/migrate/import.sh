#!/usr/bin/env bash
# Loads server/migrate/out/flow_raw.csv.gz (from bq_export.py) into the water DB on hc-data. Idempotent.
# Usage: server/migrate/import.sh [root@host]
set -euo pipefail
HOST=${1:-root@192.168.11.16}
OUT="$(cd "$(dirname "$0")" && pwd)/out"
[ -f "$OUT/flow_raw.csv.gz" ] || { echo "run bq_export.py first" >&2; exit 1; }

ssh "$HOST" 'install -d -m 755 /tmp/wc-import'
scp -q "$OUT/flow_raw.csv.gz" "$HOST:/tmp/wc-import/"
ssh "$HOST" 'chmod 644 /tmp/wc-import/* && runuser -u postgres -- psql -d water -v ON_ERROR_STOP=1' <<'SQL'
\timing on
BEGIN;
CREATE TEMP TABLE staging (start_time timestamptz, stop_time timestamptz, consumption integer);
\copy staging FROM PROGRAM 'gunzip -c /tmp/wc-import/flow_raw.csv.gz' WITH (FORMAT csv, HEADER true)
SELECT count(*) AS staged, count(*) - count(DISTINCT start_time) AS duplicate_starts FROM staging;
-- Legacy firmware: the free-running 16-bit counter wrapped every 32767 pulses and abs(delta) then added ~32767
-- pulses. A flow faster than the meter maximum (45 L/min + 10 %) cannot be real: remove the fewest wraps that make
-- it plausible (residual error per wrap <= 2 s of flow). Slow long flows with a wrap stay undetectable.
-- Rows with an unsynced clock (1970) are skipped.
INSERT INTO flow_event (device, start_ts, stop_ts, pulses, pulses_per_liter, max_lpm, closed, suspect, pulses_corrected)
SELECT 'bigquery', start_time, greatest(stop_time, start_time), consumption, NULL, NULL, NULL,
       consumption > max_pulses,
       CASE WHEN consumption > max_pulses
            THEN greatest(consumption - 32767 * ceil((consumption - max_pulses) / 32767.0)::integer, 0) END
FROM (SELECT *, 49.5 / 60 * 477 * greatest(extract(epoch FROM stop_time - start_time), 1) AS max_pulses
      FROM staging) s
WHERE consumption >= 0 AND start_time >= '2020-01-01'
ON CONFLICT (device, start_ts) DO NOTHING;
COMMIT;
ANALYZE flow_event;
SELECT count(*) AS history_rows, count(*) FILTER (WHERE suspect) AS suspect_rows,
       round(sum(pulses) / 477000.0) AS raw_m3, round(sum(coalesce(pulses_corrected, pulses)) / 477000.0) AS corrected_m3,
       min(start_ts) AS first, max(start_ts) AS last
FROM flow_event WHERE device = 'bigquery';
SQL
ssh "$HOST" 'rm -rf /tmp/wc-import'
