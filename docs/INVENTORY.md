# Legacy firmware inventory (ESP-IDF 5.1.1)

Snapshot of the production firmware before the ESP-IDF 5.4.2 refresh (commit `fcb43e8`, 2026-09-15).

## Device

| Item | Value |
|---|---|
| Board | ESP32-WROOM-32 DevKit (ESP32-D0WD), 4 MB flash, soldered onto a 7x9 cm prototype board, sealed enclosure |
| IP / MAC | `192.168.11.244` / `30:AE:A4:0A:BA:44` |
| Uptime at inventory | since 2026-09-11 23:07 |
| ESP-IDF | v5.1.1 (`sdkconfig` header), app `water-controller.bin` ~957 KB (last local build 2023-07-21) |
| Flow meter | Termipol PM-3/4-B (DN20, 2-45 L/min, open-collector output, spec 477 pulses/L ±10 %) on GPIO4 |
| Valve | Motorised DN20 ball valve, control on GPIO14 (HIGH = open) |
| LEDs | blue GPIO32 (water flowing), red GPIO33 (valve closed) |
| Logging | vendored MalteJ udp_logging → `192.168.11.15:1338` |
| OTA | `https://192.168.11.15:8070/water-controller.bin` at boot and via `POST /su`; bin deleted after success |
| Cloud | JWT (RS256, service account key embedded) → Google ID token → Cloud Function `send-metrics` → BigQuery `water-controller-351109.flow_ds.flow_raw`; older history in `vps1-ceszke-com.water_flow_ds.water_flow` |

### Flash layout (`partitions_two_ota.csv`, default IDF table)

```
bootloader v5.1.1  0x1000   (no app rollback support)
nvs                0x9000   16K
otadata            0xd000   8K
phy_init           0xf000   4K
factory            0x10000  1M
ota_0              0x110000 1M
ota_1              0x210000 1M
(unused)           0x310000-0x400000
```

Same layout as the gate-controller legacy board, so the gate migrator (bootloader + table rewrite over OTA) applies.

## Baseline measurements

| Metric | Value |
|---|---|
| Ping (3 packets from LAN) | 53-84 ms avg 68 ms, 0 % loss (WiFi power save on) |
| Free heap (`/hw-status`) | 115 KB |
| `GET /` without auth | 401 |
| `GET /is-valve-closed`, `/api/status`, `/hw-status` without auth | 200 |

## Current behaviour

- Boot: NVS → WiFi (waits forever) → UDP log → OTA check (blocking) → GPIO (valve **opened**) → httpd → NTP → `pcnt` task.
- `pcnt` task polls the PCNT counter every 1 s. Any new pulse = "running". After the first second without pulses the
  flow event ends and, above 10 pulses and 1 s, is posted to GCP synchronously.
- Protection: one rule, close the valve after `CUTOFF_SECONDS` (1200 s) of continuous flow. Reopening is manual (UI / API).

## Findings

Severity: **C** = protection can fail or be defeated, **H** = wrong data or security hole, **M** = robustness / hygiene.

| # | Sev | Where | Problem | Impact |
|---|---|---|---|---|
| 1 | C | `main.c`, `wifi.c:connect()` | Boot waits for WiFi with `portMAX_DELAY`, then runs a blocking OTA; the flow task starts last | After a power cut without WiFi there is **no protection at all** |
| 2 | C | `pcnt.c` → `gcp.c:send_metrics()` | HTTPS POST (plus token exchange) runs inside the protection loop | A slow or unreachable cloud stalls cutoff checks for tens of seconds |
| 3 | C | `hw.c:reset_gpio()` | Every boot drives the valve open | A reboot after a cutoff reopens the water |
| 4 | C | `wifi.c` event handler | Reboots the device after any reconnect | Combined with #3, a short WiFi outage cancels a cutoff |
| 5 | H | `pcnt.c` | Durations use `time(NULL)` | A late NTP sync jumps the clock and can trigger a false cutoff (or hide a real one) |
| 6 | H | `pcnt.c` | PCNT unit wraps at `INT16_MAX` (~68.7 L at 477 pulses/L); `abs(delta)` then adds ~32k pulses | Long flows are over-counted; history in BigQuery is affected |
| 7 | H | `wifi.c` event handler | `vTaskDelay` up to 60 s inside the default event loop | Blocks all other system events during reconnect (same bug in gate/heating) |
| 8 | H | `web.c` | `/api/status`, `/is-valve-closed`, `/hw-status`, `/test-send-metrics` unauthenticated; `Access-Control-Allow-Origin: *` | Anyone on the LAN (or a malicious web page) can read state; test endpoint pushes fake rows to BigQuery |
| 9 | H | `web.c:close_valve_handler()` | VLA `char buf[length]` sized from `Content-Length`, `sscanf` on a non-terminated buffer | Stack overflow / garbage read from a crafted request (authenticated) |
| 10 | H | `web.c` | Static `Authorization` header compared with `strcmp`, sent by the UI on every call; HTTP only | Credential replay, timing leak; no session, no CSRF protection |
| 11 | M | `web.c:su_handler()` | OTA runs inside the HTTP handler (16 KB httpd stack) | Server blocked for the whole download |
| 12 | M | `hw.c` | `gpio_pulldown_en` on the meter input is dead code: `pcnt_new_channel()` later enables the internal pull-up and disables the pull-down | Misleading; the meter actually runs on the internal pull-up (~45 kΩ); check whether an external pull-up exists |
| 13 | M | `hw.c` | Valve on GPIO14 (MTMS strapping pin, toggles during boot) | Actuator may twitch during reset; see `docs/HARDWARE.md` |
| 14 | M | `gcp.c`, `jwt.c` | `cJSON` root never freed; mbedtls contexts leaked on error paths; shared static buffers; `sprintf` | Slow heap loss (token refresh every hour) |
| 15 | M | `config.h` | `#define TEST` enabled in production | `/test-send-metrics` compiled in |
| 16 | M | `wifi.c` | Default fast scan joins the first AP with the SSID; power save on | Weak AP choice, 50-90 ms latency (fixed in gate-controller) |
| 17 | M | build | No app rollback in the bootloader, 1 MB OTA slots (app at ~96 %) | A bad OTA needs physical access; no room for new features |

## Refresh plan

Stages, protection tiers and decisions: see `CLAUDE.md` and the project plan. Hardware checks: `docs/HARDWARE.md`.
Flow meter calibration: `docs/CALIBRATION.md`.
