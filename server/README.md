# hc-data: water-controller backend

Telemetry storage on the Proxmox LXC `hc-data` (192.168.11.16, Debian 13), next to heating-controller.
The device publishes to Mosquitto; `wc-ingest` stores the messages in PostgreSQL database `water`.
**Nothing here is needed for protection**: if hc-data is down, the controller keeps cutting off water and the
esp-mqtt outbox (bounded, 1 h) loses what does not fit.

```
ESP32 --MQTT QoS1 water/<mac>/{flow,sample,valve,rule,alert,status}--> Mosquitto :1883
                                                                          └─ wc-ingest (persistent session, ack after commit)
                                                                               └─ PostgreSQL water ── wc-model-score (hourly)
ESP32 <--retained water/<mac>/config (learned limits)-- Mosquitto <-- publish.py (workstation)
```

## Shared broker

- heating-controller and water-controller use the same Mosquitto instance.
- Mosquitto supports one `password_file` and one `acl_file`, so both `install.sh` scripts write per-project
  fragments (`/etc/mosquitto/passwd.d/<project>`, `/etc/mosquitto/acl.d/<project>.acl`) and concatenate them.
- On the first run on a heating-only broker, the water installer turns the existing files into heating's fragments.
- Water provisioning only **reloads** Mosquitto and PostgreSQL. Heating devices stay connected.
- heating-controller `server/install.sh` must use the same fragment scheme. Otherwise its next deploy removes the water users.

| MQTT user | Access |
|---|---|
| `water-controller` | write `water/+/{flow,sample,valve,rule,alert}`, readwrite `water/+/status`, read `water/+/config` |
| `wc-ingest` | read `water/#` |
| `wc-model` | write `water/+/config` (retained learned limits for the device) |

## Database `water`

| Table | Content |
|---|---|
| `flow_event` | one row per continuous flow: start, stop, pulses, calibration, max L/min, closed by protection; history from BigQuery with `device = 'bigquery'` |
| `flow_sample` | pulses every 10 s while water flows (profile of long flows) |
| `valve_event`, `rule_event`, `alert_event` | valve changes with reason, Tier 2 triggers (learned limits add `measured`, `usual_limit`), alerts |
| `model_alert` | hourly totals over the learned limit (`score.py`), one row per hour and kind |
| `device_status` | last `online`/`offline` (LWT) |
| `setting` | `default_pulses_per_liter` for rows without calibration |

Views (Europe/Warsaw local time):

| View | Content |
|---|---|
| `flow` | liters, duration, average L/min |
| `usage_hourly`, `usage_daily` | totals per hour and per day |
| `usage_hour_of_week` | the typical week, the baseline for anomaly detection |

Roles:
- `wc_ingest`: peer auth, insert.
- `wc_read`: password, SELECT from 192.168.11.0/24 (DataGrip, Grafana, model training on a workstation).
- `wc_model`: peer auth, SELECT, INSERT on `model_alert` (hourly check).

## History from GCP

`water-controller-351109.flow_ds.flow_raw` holds 269 541 flows from 2020-06 to 2026-09.

The legacy firmware counted with a free-running 16-bit counter and `abs(delta)`, so every wrap added about 32 767 pulses (docs/INVENTORY.md #6).
- The import marks 4 020 physically impossible flows (above the meter maximum) as `suspect`.
- For those rows, `pulses_corrected` removes the fewest wraps that make the flow plausible.
- Raw total is 911 m³, corrected 635 m³.
- A slow, long flow that crossed a wrap cannot be detected.
- The older `vps1-ceszke-com` dataset is unreachable (its service account no longer exists).

```sh
uv run --with google-cloud-bigquery server/migrate/bq_export.py   # -> server/migrate/out/ (gitignored)
server/migrate/import.sh                                          # idempotent
```

## Usage model (`wc-model`)

Training runs on a workstation (the LXC has 1 CPU and 1 GB; training takes about 2 minutes on a laptop-class
machine and would compete with ingest here). `model/publish.py` sends the limits as the retained `water/<mac>/config`
and copies `thresholds.json` to `/var/lib/wc-model/out/`. The server keeps only the hourly check, in a small venv
`/opt/wc-server/model-venv` (`model/requirements-score.txt`), as role `wc_model` over the local socket.

| Unit | When | What |
|---|---|---|
| `wc-model-score.timer` | hourly at :05 | `score.py`: liters per hour against the learned hour limit (holidays included); `model_alert` + ntfy (`NTFY_URL`) |

Retraining (every few months, from the repository root):

```sh
R="uv run --with-requirements server/model/requirements.txt"
$R server/model/data.py --refresh && $R server/model/train.py && $R server/model/anomaly.py
$R server/model/publish.py --dry-run     # check size and guard
$R server/model/publish.py               # retained config + thresholds.json to hc-data
```

## Operations

```sh
cp server/secrets.env.example server/secrets.env   # fill; MQTT_DEVICE_PASS = MQTT_PASS in credentials.h
server/deploy.sh                                   # copy to /opt/wc-server + idempotent install.sh
ssh root@192.168.11.16 journalctl -u wc-ingest -f
ssh root@192.168.11.16 'runuser -u postgres -- psql water'
uv run --with paho-mqtt python -m unittest server/ingest/test_wc_ingest.py
```

Backups: `wc-pg-backup.timer`, nightly `pg_dump -Fc water` into `/var/backups/water`, kept for 14 days.
