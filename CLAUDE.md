# water-controller

Home anti-flood system: an ESP32 counts pulses from a flow meter on the main water inlet and closes a motorised valve
when water flows for too long. In production since 2020; it has already stopped several leaks (garden hoses left open or
bursting in the heat). **The cutoff is the product. Everything else (UI, telemetry, notifications, anomaly detection) is
optional and must never weaken it.**

## Status

- **Production** still runs the **legacy firmware** (ESP-IDF 5.1.1, snapshot `fcb43e8`, findings in `docs/INVENTORY.md`).
  Its sources and local secrets live in `legacy/water-controller-5.1.1/` (gitignored).
- **`firmware/`** holds the new firmware 3.x (ESP-IDF 5.4.2 + `home-idf`). It is not deployed yet: it needs the OTA
  migrator (bootloader + partition table) rehearsed on the spare board first.

The plan is in `~/.claude/plans/cele-odnosnie-tego-projektu-rosy-moler.md`. Stages:
- [x] 0 inventory
- [x] 1 home-idf
- [~] 2 firmware (built, not on hardware)
- [ ] 3 PWA
- [ ] 4 hc-data
- [ ] 5 migration
- [ ] 6 docs/portfolio
- [ ] 7 anomaly model
- [ ] 8 GCP shutdown

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

## Firmware (`firmware/`)

### Toolchain

ESP-IDF 5.4.2 (same as the sibling projects).
```sh
firmware/build.sh                 # idf.py build; uses ../home-idf (sibling checkout) when present
firmware/build.sh size
firmware/build.sh -p <port> flash monitor
HOME_IDF_FROM_GIT=1 firmware/build.sh   # build against the pinned home-idf tag (main/idf_component.yml)
cmake -S firmware/test -B build-test && cmake --build build-test && ctest --test-dir build-test   # host tests
```
- `sdkconfig` is committed and generated from `sdkconfig.defaults`; never run `idf.py set-target`.
- `home-idf` is a separate repo (`~/dev/home-idf`, private for now). A framework change needs a new tag and a bump in `main/idf_component.yml`.

### Setup

1. `cp firmware/main/config/credentials-example.h firmware/main/config/credentials.h`, then fill in WiFi, the admin
   header, the web password (`home-idf/tools/hash_password.py`), ntfy topics and the MQTT password.
2. `firmware/certs/ota_server_cert_15.pem` (gitignored): trust anchor of the OTA server on 192.168.11.15.

### Layout

```
main/
  main.c        boot: NVS → valve_restore → settings → Tier 1 task → health → log → Tier 2 → WiFi/notify/NTP/OTA/MQTT/auth/httpd
  tier1.c       pure Tier 1 logic (continuous flow + gap + limit), host-tested
  rules.c       pure Tier 2 rules (volume, burst, night window, micro-leak, vacation, snooze), host-tested
  flow.c        Tier 1 task: PCNT (accumulating, glitch filter 10 us) every 1 s, core 1, task watchdog; valve alert;
                pulse-interval diagnostics (GPIO ISR, on demand)
  valve.c       only owner of GPIO14; state + reason persisted in NVS and restored at boot (never changed by a reboot)
  protect.c     Tier 2 task on a sample queue (drops, never blocks Tier 1)
  settings.c    Tier 1 limit, calibration, Tier 2 thresholds in NVS, clamped
  events.c      RAM ring of the last 50 events, daily totals, ntfy notifications
  telemetry.c   MQTT to hc-data through its own queue and task (timestamps fixed once the clock syncs)
  api.c         routes on the home-idf HTTP server
  config/       config.h (committed), credentials.h (never committed)
web/            login.html, app.html (placeholder until stage 3), manifest.webmanifest, apple-touch-icon.png
test/           host unit tests (tier1, rules)
partitions.csv  ota_0 2M / ota_1 1.875M (legacy ota_1 offset kept for the migrator) / coredump
```

### HTTP API (port 80)

Common routes come from home-idf: `/`, `/api/session`, `/api/login`, `/api/logout`, `/api/reboot`, `/api/ota`,
`/admin/su`, `/admin/reboot`, `/admin/hw-status`. Water routes:

| Method | Path | Guard | Description |
|---|---|---|---|
| GET | `/api/status` | session | valve, flow (lpm, liters, Tier 1 remaining), usage, tier2, settings, telemetry, diag, system |
| GET | `/api/events` | session | last 50 events (flow, valve, rule, alert) |
| POST | `/api/valve` | mutation | `{state: "open"\|"closed"}` |
| POST | `/api/settings` | mutation | partial settings (`tier1_limit_s`, `pulses_per_liter`, Tier 2 thresholds), clamped |
| POST | `/api/snooze` | mutation | `{minutes}`: mute Tier 2 volume/burst/night (vacation stays) |
| POST | `/api/diag` | mutation | `{minutes}`: pulse-interval diagnostics |
| POST | `/admin/valve` | `Authorization` header | `{state}` for scripts |
| GET | `/admin/status` | `Authorization` header | same as `/api/status` |

Mutation = session cookie + JSON + `X-CSRF-Token` + Origin equal to Host.

### MQTT (hc-data)

The broker is `mqtt://192.168.11.16:1883`, user `water-controller`, QoS 1. Topics:

| Topic | Payload |
|---|---|
| `water/<mac>/flow` | `{start, stop, pulses, liters, max_lpm, closed, pulses_per_liter}` |
| `water/<mac>/sample` | every 10 s while water flows |
| `water/<mac>/valve` | valve changes |
| `water/<mac>/rule` | Tier 2 triggers |
| `water/<mac>/alert` | alerts |
| `water/<mac>/status` | retained `online` / `offline` (LWT) |

## Legacy firmware (production until migration)

- Legacy HTTP API on :80: `GET /api/status`, `GET /is-valve-closed`, `GET /hw-status`, `POST /close-valve` (body
  `1`/`0`, header auth), `POST /su` (OTA from `https://192.168.11.15:8070/water-controller.bin`), `POST /reboot`.
- `GET /test-send-metrics` sends a fake row to BigQuery: **never call it**.
- The legacy UDP log also goes to `192.168.11.15:1338`.

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

- **Never** commit: `credentials.h`, `certs/*`, `*.pem`, `*.p12`, `gcp/`, `legacy/`, `*.private.env.json`, `secrets.env`.
  - Run `git check-ignore` before `git add`.
- Commit and push to `master` after each verified stage. Commit messages clean, no AI attribution.
- Never close or open the production valve, and never call `/test-send-metrics`, without the owner's explicit OK.
- Anything writing bootloader/partition table: spare board first, owner approval for production.
- `snprintf` only; no VLAs sized from request data; flag leaks and stack buffers crossing tasks.
- Check every change against the protection tiers above:
  - Tier 1 code (`flow.c`, `tier1.c`, `valve.c`) never calls the network, never waits on a queue, and never allocates in its loop.
  - Keep `tier1.c` / `rules.c` free of ESP-IDF includes and cover changes with host tests.
