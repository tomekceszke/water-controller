# Testing

Protection is tested at two levels: pure logic on the host, then the real firmware on the spare board.

## Host unit tests

`firmware/test/` compiles `tier0.c`, `tier1.c`, `rules.c` and `learned.c` without ESP-IDF:

```sh
cmake -S firmware/test -B build-test && cmake --build build-test && ctest --test-dir build-test
```

| Suite | Covers |
|---|---|
| `test_tier0` | hard-coded 60 min, trips once, a trickle with 5 s pauses is continuous, a longer pause restarts the hour, water turned on again gets a new hour, reopening while water still runs restarts the limit, no repeated trips without a reopen |
| `test_tier1` | flow start/end with the pause gap, trip exactly once at the limit, trickle flow keeps the timer, trip during a pause, fresh state after a trip, limit lowered during a flow, 2 h of high flow without overflow |
| `test_rules` (tier caps) | Tier 2 time cap 3/4 of Tier 1, volume cap 12 L/min × that time, at the shortest, default and longest Tier 1 (always inside Tier 1 and Tier 0) |
| `test_learned` | config parser (valid, wrong array length, wrong version, values clamped to the hard bounds), duration and volume notifications once per flow, the limit of the hour the flow started in, a new flow re-arms, night flow count (tiny flows ignored, once per night, reset the next night), duration and volume limits capped under Tier 1, nothing without a clock or a config |
| `test_rules` | all rules off, max volume (once per flow, exact pulse threshold), reset between flows, burst duration, night window across midnight, night rule off without a clock, snooze vs vacation, snooze expiry, micro-leak notification once, leak counter reset |

The server side has its own unit tests (no database): `server/model/test_model.py` (hourly grid, DST, wrap fix, split,
level correction, recency weights, meter reconciliation, invoice parsing on synthetic text, leak injection, the config
`publish.py` builds, the hourly check) and `server/ingest/test_wc_ingest.py`.
The config format is also checked end to end: a real `publish.py --dry-run` payload parses with `learned.c`.

A deliberately broken `tier1.c` (trip flag not latched) makes the suite fail, so the tests do catch regressions.

## Hardware test (spare board)

The test build (`-DWATER_TEST_PULSES=1`, never shipped) adds admin-only endpoints and shortens the clock-bound limits,
keeping their order (Tier 1 < Tier 0), so the same logic runs in seconds:
- a square-wave generator on the flow meter pin (LEDC drives the pad, PCNT counts it back);
- a WiFi outage switch and a Tier 2 hang switch;
- Tier 0 at 45 s instead of 60 min, Tier 1 settable from 20 s, and a throwaway web password
  (`-DWATER_TEST_SALT_HEX/-DWATER_TEST_HASH_HEX`).

Spare builds (`-DWATER_SPARE=1`) publish MQTT under `water-spare/`, which neither wc-ingest nor the Apple Home bridge
reads: before that, test shut-offs reached the history and raised "Water leak" critical alerts in Apple Home.

`tools/hw_test.py` drives the device over HTTP, section by section; every section ends with the settings restored
and the valve open, also after a failure:

```sh
firmware/build.sh -B build-testhw -DWATER_TEST_PULSES=1 -DWATER_SPARE=1 -DWATER_TEST_SALT_HEX=.. -DWATER_TEST_HASH_HEX=.. \
    -p /dev/cu.usbserial-0001 flash
WC_PASSWORD=... WC_ADMIN=... tools/hw_test.py <spare-ip>                  # everything, about 8.5 min
WC_PASSWORD=... WC_ADMIN=... tools/hw_test.py <spare-ip> --only tier1,caps # just what changed
# the learned section: WC_MODEL_MQTT_PASS=... WC_DEVICE_ID=<mac> uv run --with paho-mqtt tools/hw_test.py ...
```

Result on 2026-09-30 (firmware 3.4.0, ESP32-D0WDQ6 spare board): all sections pass; a full run takes 518 s (was
about 17 min with the 150 s Tier 0 and 60 s Tier 1 of earlier test builds).

| Section | Check | Result |
|---|---|---|
| security | status without session, mutation without CSRF, foreign Host, foreign Origin, admin without header | 401 / 403 / 403 / 403 / 401 |
| counting | 100 Hz × 20 s | 4.193 L (expected 4.193), rate 12.5 L/min; diag 2 000 edges, min interval ~10 ms, 0 glitches |
| counting | 1000 Hz × 36 s, over the 16-bit hardware limit | 35 992 of 36 000 pulses, no wrap |
| tier1 | limit 20 s, continuous flow | closed after 20-21 s; alert "still flowing" while pulses continue |
| reboot | reboot while closed / open | state unchanged, Tier 1 limit persisted, reset reason software |
| tier2 | max volume 2 L at 100 Hz | closed after 10 s; snoozed rule does not close |
| caps | Tier 1 set to 4 h; Tier 2 5000 L; Tier 1 lowered to 10 min | 45 min; 180 L; re-clamped to 90 L |
| tier0 | Tier 1 at 45 min, ceiling 45 s | closed by Tier 0 after 45-46 s; again 45 s after an immediate reopen |
| wifi | WiFi off for 40 s during the flow | closed by Tier 1, no reboot |
| hang | Tier 2 task suspended (its 1 L rule would fire at ~10 s) | closed by Tier 1 after 20 s |
| learned | retained `water-spare/<mac>/config`, 5 L at every hour | applied; notice after 24 s with limit 5 L; valve untouched; limits survive a reboot and a deleted retained message |

The hardware test found these bugs:
- logging started before the network stack crashed the device;
- volume limits compared whole liters, so they fired one liter late;
- (3.1.0) a flow reopened before the water stopped kept the "already tripped" flag and had no limit any more. Tier 0 and Tier 1 now restart when the valve is opened.
- (3.4.0) the flow task noticed a reopen by sampling the valve state once a second, so a close and a reopen inside
  one sample were missed and the flow stayed "tripped", without a limit. `valve_open_count()` counts transitions to
  open, and the flow task compares the count instead (found by the faster tier0 section, which reopens at once).

## Still to verify on hardware

- Valve line and actuator behaviour during reset and bootloader (`docs/HARDWARE.md`).
- Real meter signal quality (`POST /api/diag` on production after the migration).
- Migration from the legacy image with the OTA migrator (stage 5).
