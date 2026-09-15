# water-controller

Home anti-flood system: an ESP32 counts pulses from a flow meter on the main water inlet and closes a motorised valve
when water flows for too long. In production since 2020; it has already stopped several leaks (garden hoses left open or
bursting in the heat). **The cutoff is the product. Everything else (UI, telemetry, notifications, anomaly detection) is
optional and must never weaken it.**

## Status

The repo holds the **legacy firmware** (ESP-IDF 5.1.1, snapshot `fcb43e8`) that runs in production. Findings are in
`docs/INVENTORY.md`.

A refresh is in progress:
- ESP-IDF 5.4.2, installed through an OTA migrator;
- shared framework `home-idf` (separate public repo);
- protection tiers;
- PWA;
- telemetry to hc-data (MQTT + PostgreSQL) instead of GCP;
- anomaly detection;
- README for the portfolio.

The plan is in `~/.claude/plans/cele-odnosnie-tego-projektu-rosy-moler.md`.

## Protection tiers (design rule for every change)

- **Tier 1**: close the valve after continuous flow longer than a configurable limit (settable from the app, hard min/max,
  can be raised but never disabled).
  - Must work with no WiFi, NTP, MQTT, ntfy, httpd, Tier 2 or valid NVS (defaults from `config.h`).
  - Own task, monotonic clock, no network, no blocking on queues.
- **Tier 2**: anomaly rules on the device (volume, burst, night window, micro-leak, vacation) plus thresholds learned on hc-data.
  - May only *request* a close.
  - A hang or failure in Tier 2 must not delay Tier 1.
- **Valve state survives reboot and power loss**: firmware never changes it at boot, it restores the last commanded state from NVS.
- Telemetry and notifications are fire-and-forget (non-blocking enqueue, drop when full).

## Hardware

| Item | Value |
|---|---|
| Board | ESP32-WROOM-32 DevKit on a prototype board, 4 MB flash, sealed enclosure (hard to reach: **OTA only**) |
| Production | `192.168.11.244`, MAC `30:AE:A4:0A:BA:44` |
| Spare | identical board with USB for development and destructive tests |
| Flow meter | Termipol PM-3/4-B DN20, open collector, spec 477 pulses/L ±10 % (calibration: `docs/CALIBRATION.md`) |
| Valve | motorised DN20 ball valve, single control line (wiring checks: `docs/HARDWARE.md`) |

| GPIO | Function |
|---|---|
| 4 | Flow meter pulses (PCNT, rising edge) |
| 14 | Valve control, HIGH = open (strapping pin MTMS) |
| 32 | Blue LED, water flowing |
| 33 | Red LED, valve closed |

## Legacy firmware (`firmware/water-controller/`)

The toolchain (ESP-IDF 5.1.1) is **not installed**. It is only needed to rebuild the legacy image for the migration rehearsal.

```
main/
  main.c      boot: NVS → WiFi (blocks) → UDP log → OTA → GPIO → httpd → NTP → pcnt task
  pcnt.c      1 s polling, flow events, CUTOFF_SECONDS rule, send_metrics()
  hw.c        GPIO, valve, LEDs
  web.c       httpd :80 (see API)
  wifi.c ota.c tools.c (NTP)
  gcp.c jwt.c base64url.c   JWT → Google ID token → Cloud Function send-metrics → BigQuery
  config.h credentials.h (gitignored, template credentials-example.h)
components/udp-logging   vendored MalteJ udp_logging → 192.168.11.15:1338
www/index.html           Bootstrap UI (gzipped into the image by deploy.sh)
certs/                   OTA server cert, GCP CA + service account key (gitignored)
```

### Legacy HTTP API (port 80)

| Method | Path | Auth | Description |
|---|---|---|---|
| GET | `/` | header | UI |
| GET | `/api/status` | none | `{is_closed, is_running, start_time, stop_time, consumption}` (consumption in pulses) |
| GET | `/is-valve-closed` | none | `1` / `0` |
| GET | `/hw-status` | none | `{up_since, free_kb}` |
| POST | `/close-valve` | header | body `1` close, `0` open |
| POST | `/su` | header | OTA from `https://192.168.11.15:8070/water-controller.bin` |
| POST | `/reboot` | header | restart |
| GET | `/test-send-metrics` | none | sends a fake row to BigQuery. **Never call it** |

Auth = `Authorization` header equal to `HEADER_AUTHORIZATION_VALUE`.

## Infrastructure

- **OTA server**: `https://192.168.11.15:8070` (`~/apps/ota-server` on .15, started on demand).
- **UDP logs**: `192.168.11.15:1338` (`nc -ul 1338`).
- **hc-data**: `192.168.11.16`, Mosquitto + PostgreSQL. Shared with `../heating-controller`, whose `server/install.sh` owns the Mosquitto passwd/ACL files today.
- **GCP (legacy, to be shut down after cutover)**:
  - project `water-controller-351109`: Cloud Function `send-metrics`, dataset `flow_ds.flow_raw`;
  - older history in `vps1-ceszke-com.water_flow_ds.water_flow`;
  - local sources in `gcp/` (gitignored).

## Sibling projects

- `../gate-controller`: ESP-IDF 5.4.2 reference (auth, API guards, PWA, health/rollback, OTA migrator).
- `../heating-controller`: MQTT telemetry and hc-data server; it migrated off GCP on 2026-09-11.
- `../floor-heating-controller`: ntfy notifications, CI workflow.

## Rules

- **Never** commit: `credentials.h`, `certs/*`, `*.pem`, `*.p12`, `gcp/`, `*.private.env.json`, `secrets.env`.
  - Run `git check-ignore` before `git add`.
- Commit and push to `master` after each verified stage. Commit messages clean, no AI attribution.
- Never close or open the production valve, and never call `/test-send-metrics`, without the owner's explicit OK.
- Anything writing bootloader/partition table: spare board first, owner approval for production.
- `snprintf` only; no VLAs sized from request data; flag leaks and stack buffers crossing tasks.
- Check every change against the protection tiers above.
