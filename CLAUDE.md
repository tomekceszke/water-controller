# water-controller

Home anti-flood system: an ESP32 counts pulses from a flow meter on the main water inlet and closes a motorised valve
when water flows for too long. In production since 2020; it has already stopped several leaks (garden hoses left open or
bursting in the heat). **The cutoff is the product. Everything else (UI, telemetry, notifications, anomaly detection) is
optional and must never weaken it.**

## Status

- **Production runs firmware 3.2.0** (OTA 2026-09-16 17:19, sha256 `ca0bc89a…`: home-idf 0.1.8 app shell, 410 pulses/L default). 3.1.0 moved it to the ESP-IDF 5.4.2 bootloader and partition table over the air on 2026-09-15 at 23:02, see `docs/IDF5_MIGRATION.md`.
  - Updates from now on: `tools/build_release.sh`, publish `releases/water-controller.bin` as `water-controller.bin` on the OTA server, `POST /admin/su` (or the Device tab).
- The legacy firmware (5.1.1, snapshot `fcb43e8`, findings in `docs/INVENTORY.md`) lives in `legacy/water-controller-5.1.1/` (gitignored) for reference.

The plan is in `~/.claude/plans/cele-odnosnie-tego-projektu-rosy-moler.md`. Stages:
- [x] 0 inventory
- [x] 1 home-idf
- [~] 2 firmware (built, not on hardware)
- [x] 3 PWA
- [x] 4 hc-data (deployed 2026-09-15, history imported; Grafana not done)
- [x] 5 migration (production 2026-09-15 23:02)
- [x] 6 docs/portfolio (README, LICENSE, CI)
- [ ] 7 anomaly model
- [ ] 8 GCP shutdown

## Protection tiers (design rule for every change)

- **Tier 0**: close the valve after **60 minutes** of continuous flow.
  - Hard-coded in `tier0.h`: no setting, API, snooze or NVS value can change it.
  - Own flow tracking (pause > 5 s ends a flow) in the flow task; test builds shorten it (`WATER_TEST_TIER0_S`).
  - Owner decision 2026-09-15: after an hour of water nothing is left to save; a long fill means reopening once an hour.

- **Tier 1**: close the valve after continuous flow longer than a configurable limit (settable from the app, hard min/max,
  never above Tier 0's 60 min, never disabled).
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
- `home-idf` is a separate public repo (`~/dev/home-idf`, github.com/tomekceszke/home-idf). A framework change needs a new tag, a bump in `main/idf_component.yml` (firmware and migrator) **and deleting `dependencies.lock`** (the component manager keeps a locked git commit even when the tag in the manifest changes), then `HOME_IDF_FROM_GIT=1 firmware/build.sh` to regenerate it.
- CI (GitHub Actions): `tests.yml` (host tests + ingest validation) and `firmware.yml` (firmware + migrator build, placeholder secrets). There is no CD: releases go out through `tools/build_release.sh` + the OTA server.

### Setup

1. `cp firmware/main/config/credentials-example.h firmware/main/config/credentials.h`, then fill in WiFi, the admin
   header, the web password (`home-idf/tools/hash_password.py`), ntfy topics and the MQTT password.
   - Secrets are stored obfuscated (`home-idf/tools/obfuscate.py` → `"obf1:..."`, `--reveal` to read back); that is
     not encryption. Scripts that need a value (e.g. the admin header for `tools/hw_test.py`) reveal it the same way.
2. `firmware/certs/ota_server_cert_15.pem` (gitignored): trust anchor of the OTA server on 192.168.11.15.

### Layout

```
main/
  tier0.c       pure Tier 0 logic (hard-coded 60 min ceiling), host-tested
  main.c        boot: NVS → valve_restore → settings → Tier 0/1 task → health → log → Tier 2 → WiFi/notify/NTP/OTA/MQTT/auth/httpd
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
web/            app.html (tabs Live/History/Settings on the home-idf app shell, vanilla, ~11.4 KB gzip), manifest, icon
                (the sign-in page comes from home-idf: home_idf_login_page() in main/CMakeLists.txt)
test/           host unit tests (tier0, tier1, rules)
migrator/       one-shot OTA image (home-idf hi_migrator); build with tools/build_release.sh [--spare]
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

### UI work

```sh
tools/dev_proxy.py <device-ip> --port 8765   # serves firmware/web/app.html rendered with the home-idf shell, forwards /api and /admin
```
- Language: English.
- Layout, controls and the tab bar come from home-idf (`web/app_shell.css`, `web/app_shell.js`, rendered by
  `home_idf_app_page()`); `app.html` sets its palette in `:root` and holds only the water parts. Same layout as
  gate-controller (owner decision 2026-09-16): wordmark + status, headline, three numbers, main view, latest events,
  swipe and small actions docked above the tabs.
- Shutting off and turning on are both a slide of the knob to the end (fires on arrival, no hold, no dialog).

## Legacy firmware (production until migration)

- Legacy HTTP API on :80: `GET /api/status`, `GET /is-valve-closed`, `GET /hw-status`, `POST /close-valve` (body
  `1`/`0`, header auth), `POST /su` (OTA from `https://192.168.11.15:8070/water-controller.bin`), `POST /reboot`.
- `GET /test-send-metrics` sends a fake row to BigQuery: **never call it**.
- The legacy UDP log also goes to `192.168.11.15:1338`.

## Infrastructure

- **OTA server**: `https://192.168.11.15:8070` (`~/apps/ota-server` on .15, started on demand).
- **UDP logs**: `192.168.11.15:1338` (`nc -ul 1338`).
- **hc-data**: `192.168.11.16`, Mosquitto + PostgreSQL shared with `../heating-controller`.
  - Water part: `server/` (DB `water`, `wc-ingest`, backups); see `server/README.md`.
  - Mosquitto passwd/acl are assembled from `/etc/mosquitto/{passwd.d,acl.d}` fragments by both projects' `install.sh`.
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

## TODO

- [x] Production migration to 3.1.0 (2026-09-15).
- [ ] Off-host backups of hc-data dumps (`/var/backups/water`, `/var/backups/heating` live only on the CT disk): choose a location.
- [x] **Valve shut-off test on production** (2026-09-16 11:14, passed).
  - Run with the water already flowing, then closed from the app: flow stopped **6 s** after the command (actuator
    travel < 10 s), ~0.4 L passed after it. `flow_event` 11:14:28-11:14:46, 353 pulses, `closed=true`, no `alert_event`.
  - The first-floor tap kept running ~1.5 min afterwards, weakening: the riser draining by gravity, zero pulses through
    the meter. Worth remembering for the next test, it looks alarming and is not.
  - Reopened from the app, water flows again.
  - Closing with the water already running tests more than the TODO's original order (close first, then open a tap):
    it shows the cutoff itself, not just the absence of flow.
- [x] **Calibration against the house water meter** (2026-09-16, three runs, `docs/CALIBRATION.md`).
  - Measured 366 pulses/L at 5 L/min, 408 at 9.8, 425 at 13.4: the meter under-reads at low flow and sits below the
    datasheet's 477 ±10 %. Factor set to **410** (the value at 8-11 L/min, where most household draw happens); 477 was
    under-reporting volume by ~14 %.
  - Settles the cistern question: the Grohe full flush is 3620 pulses = 8.8 L, so it is set to 9 L.
  - `PULSES_PER_LITER_DEFAULT` in `config.h` is now 410; it only reaches the device with the next OTA release.
- [x] **3.2.0 on production** (2026-09-16 17:19): shared app shell from home-idf 0.1.8 (tabs Live/History/Settings,
  slide to shut off and to turn on) plus the 410 pulses/L default.
- [ ] **UI polish** (owner wants another round), noted so far:
  - a dripping-leak rule event shows "0 L, 0 L/min" (firmware sends the current flow, which is zero between drips);
  - regenerate README screenshots after UI changes (`docs/img/app-*.png`, 390×844 at 2x, rendered with mocked API data in headless Chrome).
- [~] **Unified sign-in page for all projects, moved to home-idf** (owner request 2026-09-15):
  - done in home-idf 0.1.6: `web/login.html` + `home_idf_login_page(<lib> NAME "w-controller" ACCENT "#56c2e6" [ICONS ON|OFF])`,
    rendered and gzipped at build time under the symbols `hi_httpd` already reads; water-controller uses it and no
    longer has its own `web/login.html`;
  - design (owner picked it on 2026-09-16 from mocks): left-aligned wordmark split on the name's first hyphen,
    accent `W-` at 1.45x over `controller`, no subtitle, field and button both 58 px with a 20 px gap;
  - still to do: gate (`g-controller`), heating and floor-heating when they move to home-idf.
- [ ] Stage 7 anomaly model; stage 8 GCP shutdown (not before 2026-09-29).
- [ ] Sibling projects: CI everywhere (gate: none, heating: not on GitHub yet), move them to home-idf.
