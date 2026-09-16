# Flow meter calibration

Meter: Termipol PM-3/4-B, DN20. Datasheet: 477 pulses/L ±10 %, rated range 2-45 L/min (±3 %), open-collector output.

The ±10 % tolerance means the real factor is anywhere between ~430 and ~525 pulses/L, so it has to be measured.
**Measured on 2026-09-16 it turned out lower than that and flow-dependent: 366-425 pulses/L (see Results).**

## Pulse timing (edge counting threshold)

| Flow | Frequency at 477 pulses/L | Period | Half period |
|---|---|---|---|
| 2 L/min | 16 Hz | 63 ms | 31 ms |
| 15 L/min | 119 Hz | 8.4 ms | 4.2 ms |
| 45 L/min | 358 Hz | 2.8 ms | 1.4 ms |

- The firmware counts rising edges only.
- The PCNT glitch filter (`PCNT_MAX_GLITCH_NS`) is currently 3 µs. Anything shorter than the filter is ignored.
  - Real pulses are at least ~1.4 ms long, so the filter can safely go up to the ESP32 hardware maximum (~12.8 µs) to reject more contact/EMI noise.
  - The filter cannot reject slower noise, e.g. a floating input. That needs a proper pull-up (see `docs/HARDWARE.md`).
- The firmware records the shortest pulse interval and a glitch counter (`/api/diag`, Protection → Signal check). Any
  interval far below 2.8 ms points to noise, not water.

## Procedure (house water meter)

The house water meter sits directly upstream of the valve and the flow meter, so everything the house draws passes
through both. That makes it the reference: no bucket, no isolating taps, and runs can be large enough for the reading
error to stop mattering.

Reference meter here: Metering Flowis+ / ETW-MES-ECO, no. 22097104, Q3 4.0, R160-H, smallest roll 1 L.

1. With no water running, note the house meter reading and the device pulse counter:
   ```sh
   ADMIN=$(python3 ~/dev/home-idf/tools/obfuscate.py --reveal \
     "$(grep HEADER_AUTHORIZATION_VALUE firmware/main/config/credentials.h | cut -d'"' -f2)")
   curl -s -H "Authorization: $ADMIN" http://192.168.11.244/admin/status \
     | jq '{counter:.flow.counter, flowing:.flow.flowing, uptime:.system.uptime_s}'
   ```
   `flow.counter` is pulses since boot, accumulated, and does not wrap. It is independent of the configured
   `pulses_per_liter`, so it stays valid while the factor is still wrong.
2. Open one tap, **set the rate once and do not touch it again**, and let it run.
3. Close the tap, wait 3 s for the flow event to end, read the meter and the counter again.
4. `pulses_per_liter = Δcounter / Δliters`.
5. Repeat at three rates: low (~5 L/min), typical (~10 L/min), high (~13 L/min or full open).

Rules that matter:
- **Never change the rate mid-run.** A run at a varying rate gives an average that belongs to no single point on the
  curve. (Run 2 below was such a run; it happened to land on the curve anyway, but that was luck.)
- Draw enough water. The smallest roll is 1 L and it is usually mid-turn at both ends, so the reading is ±1 L:
  50 L gives 2 %, 90 L gives ~1 %.
- Check `system.uptime_s` grew between the two readings. A reboot zeroes `flow.counter` and voids the run.
- Tier 1 closes the valve after `tier1_limit_s` (1800 s in production), so keep runs well under that.
- Cold water only: no reason to add the boiler to the measurement.
- Other water use during a run does not spoil it — it passes through both meters — but it does blur the rate.

## Results (2026-09-16, production board, house meter 623.746 -> 624.001 m³)

| Run | Volume [L] | Time [s] | Rate [L/min] | Pulses | Pulses/L |
|---|---|---|---|---|---|
| 1 | 124 | 554 | 13.4 | 52689 | 425 |
| 2 | 44 | 523 | 5.0 (varied 3-6, rate was adjusted mid-run) | 16092 | 366 |
| 3 | 87 | 535 | 9.8 | 35464 | 408 |

The three points are monotonic in flow rate: the lower the rate, the fewer pulses per liter. That is the normal
characteristic of a turbine meter — bearing friction makes it under-read at low flow. Nothing is broken in the
electronics; this particular meter simply sits below the datasheet's ±10 % band.

Consistency check on run 3: the device reported 8.3-8.5 L/min using 477; rescaled to 408 that is 9.8 L/min, and the
house meter gives 87 L / 535 s = 9.76 L/min.

**`FLOW_PULSES_PER_LITER` set to 410**, the value the curve holds at 8-11 L/min, where most household draw happens
(shower, bath, washing machine, cistern refill, kitchen tap). The previous 477 under-reported volume by ~14 %.

Consequences:
- Volume below ~5 L/min is under-reported by up to 10 % even at 410, so micro-leak volumes stay approximate. The
  micro-leak rule is about *duration* of continuous trickle, not liters, so this does not weaken it.
- Tier 0 and Tier 1 measure time, not liters: unaffected by the factor.
- Tier 2 volume thresholds are expressed in liters, so re-check them after changing the factor. At the time of this
  calibration all of them were off except `vacation_max_liters`.
- hc-data stores raw pulses and the `pulses_per_liter` in force for every event, so older rows can be recomputed.

Cross-check that fell out of this: the Grohe Rapid SL full flush is 3620 pulses = 8.8 L at 410, so the cistern is set
to 9 L. The earlier "6 L would mean ~600 pulses/L" hypothesis is dead.
