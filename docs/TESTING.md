# Testing

Protection is tested at two levels: pure logic on the host, then the real firmware on the spare board.

## Host unit tests

`firmware/test/` compiles `tier1.c` and `rules.c` without ESP-IDF:

```sh
cmake -S firmware/test -B build-test && cmake --build build-test && ctest --test-dir build-test
```

| Suite | Covers |
|---|---|
| `test_tier1` | flow start/end with the pause gap, trip exactly once at the limit, trickle flow keeps the timer, trip during a pause, fresh state after a trip, limit lowered during a flow, 2 h of high flow without overflow |
| `test_rules` | all rules off, max volume (once per flow, exact pulse threshold), reset between flows, burst duration, night window across midnight, night rule off without a clock, snooze vs vacation, snooze expiry, micro-leak notification once, leak counter reset |

A deliberately broken `tier1.c` (trip flag not latched) makes the suite fail, so the tests do catch regressions.

## Hardware test (spare board)

The test build (`-DWATER_TEST_PULSES=1`, never shipped) adds admin-only endpoints:
- a square-wave generator on the flow meter pin (LEDC drives the pad, PCNT counts it back);
- a WiFi outage switch;
- a Tier 2 hang switch.

`tools/hw_test.py` drives the device over HTTP:

```sh
firmware/build.sh -B build-testhw -DWATER_TEST_PULSES=1 -p /dev/cu.usbserial-0001 -b 115200 flash
WC_PASSWORD=... WC_ADMIN=... tools/hw_test.py <spare-ip>
```

Result on 2026-09-15 (firmware 3.0.0, ESP32-D0WDQ6 spare board): **31/31 checks passed**.

| Area | Check | Result |
|---|---|---|
| Security | status without session, mutation without CSRF, foreign Host, foreign Origin, admin without header | 401 / 403 / 403 / 403 / 401 |
| Counting | 100 Hz × 20 s | 4.193 L (expected 4.193), rate 12.5 L/min |
| Counting | 400 Hz × 30 s | 11 997 of 12 000 pulses |
| Counting | 600 Hz × 60 s, over the 16-bit hardware limit | 35 993 of 36 000 pulses, no wrap |
| Diagnostics | GPIO edge timing at 100 Hz | 2 000 edges, min interval 9 999 µs, 0 glitches |
| Tier 1 | limit 60 s, continuous flow | closed after 61 s; alert "still flowing" while pulses continue |
| Tier 1 | WiFi off for 90 s during the flow | closed by Tier 1, no reboot |
| Tier 1 | Tier 2 task suspended | closed by Tier 1 after 61 s |
| Valve | reboot while closed / open | state and reason unchanged |
| Settings | reboot | Tier 1 limit persisted |
| Tier 2 | max volume 5 L at 100 Hz | closed after 25 s (5 L at 24 s) |
| Tier 2 | snooze | rule muted |

The hardware test found two bugs:
- logging started before the network stack crashed the device;
- volume limits compared whole liters, so they fired one liter late.

## Still to verify on hardware

- Valve line and actuator behaviour during reset and bootloader (`docs/HARDWARE.md`).
- Real meter signal quality (`POST /api/diag` on production after the migration).
- Migration from the legacy image with the OTA migrator (stage 5).
