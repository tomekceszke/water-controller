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

Reference meter here: Metering Flowis+ / ETW-MES-ECO, no. `<METER_ID>`, Q3 4.0, R160-H, smallest roll 1 L.

1. With no water running, note the house meter reading and the device pulse counter:
   ```sh
   ADMIN=$(python3 ~/dev/home-controllers/home-idf/tools/obfuscate.py --reveal \
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

## Results (2026-09-16, production board, 255 L on the house meter over the three runs)

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

## Checked against the utility meter (2026-09-30)

The water utility bills from a radio-read main meter, about once a month. Its readings on the invoices (42
consecutive intervals from 2023-03 to 2026-09, 1 m³ resolution) are a long-term reference for the whole flow mix,
which a bucket test cannot give. The readings stay private (`server/model/invoices.py` writes the gitignored
`server/model/meter_readings.csv`); only ratios are published. A garden sub-meter on the invoices sits behind the
flow meter and is ignored.

`server/model/meter.py` compares every billing interval:

- **410 pulses/L holds**: the 2026 intervals imply 410.
- **The legacy counter-wrap bug (INVENTORY #6) began in July 2023**: before, raw pulses equal the corrected ones;
  after, raw pulses run at 1.4-1.7x the meter.
- **Legacy telemetry lost part of the water** (flows it failed to send), most in 2024. Legacy volumes are therefore
  scaled to the meter per interval before modelling (`features.clean_flows`); see docs/USAGE_MODEL.md.
- One interval carries ±1 m³ in about 12 m³ (±8 %), so the check needs several months. Rule: once firmware 3.x has
  36 m³ of complete intervals (about ±3 %), `meter.py` recommends a new factor if the implied one differs by more than
  3 %; set it from the app (Settings, Flow meter).

With new invoices: `uv run --with pypdf server/model/invoices.py <dir>`, then `server/model/meter.py`.

## Checked against the radio-read house meter (2026-10-13)

Since 2026-10-07 meters-controller reads the house meter itself over radio (FLOWIS+ add-on, 1 L resolution, frames
every ~15 s on weekdays ~08:04-17:00, silent in the evenings and at weekends) into hc-data, database `meters`. That
gives a reference with far finer time resolution than the invoices. Compared over 2026-10-07 10:32 – 10-12 16:57,
exactly between two radio frames (no interpolation), with `flow_event` of the production board:

- **The flow meter counts 5.9 % less than the house meter.** The same in the daytime intervals between frames and
  across the night and weekend gaps, so it is a property of the meter, not of a time of day.
- Fitting house meter = a × flow meter + b × hours over the intervals between consecutive radio frames:
  - **a ≈ 1.025**: at normal draw the flow meter under-reads by ~2.5 %, i.e. the effective factor is ~400 pulses/L,
    not 410. Consistent with the 2026-09-16 curve (408 at 9.8 L/min, less at lower flow).
  - **b ≈ 0.7 L/h (~17 L/day) that the flow meter does not see at all.** In intervals where the flow meter counted
    < 0.5 L the house meter advanced twice as much, and 15 intervals had +1 L on the house meter with no flow event.
    It looked like a continuous trickle below the turbine's starting flow; **it is not one**, see the next section.
- 4 days of data and 1 L resolution: the split between a and b is indicative, the total is solid.

## No trickle; recalibrated to 386 (2026-10-13)

The trickle the fit above suggested is not there:

- **Owner's check**: with no water in use, the hundredths of the house meter register did not move for about an hour
  (the flow indicator is covered by the radio add-on, so only the register can be read). On its own that is too coarse
  to rule out 0.7 L/h (10 L per hundredth), but the radio data settles it.
- **Radio data**: 26 windows inside the radio hours with no flow event, 17 h between their first and last frame. A
  0.7 L/h trickle would add ~12 L spread evenly over them. The house meter advanced 7 L, each time +1 L a few minutes
  after a draw (or just before the next one), and otherwise stood still for an hour and more (e.g. 2026-10-07
  12:22-13:27, 13:28-14:36): water at the very end of a draw, e.g. a fill valve closing slowly, below the turbine's
  starting flow.
  That water belongs to the draws, so it is part of what a per-liter factor has to cover.
- A frame gap fakes a trickle: on 2026-10-13 a 6 L draw at 09:45 reached the radio data only at 10:00. Compare a
  window from the first frame *after* the preceding flow.

Over the whole radio span, 2026-10-07 10:32 to 10-13 11:15, flows wholly between two frames, none straddling:
**2491 L on the house meter, 960 438 pulses = 385.6 pulses/L** (at 410 the device reported 2342 L, 6.0 % low).

**`pulses_per_liter` set to 386** (Settings, Flow meter; `PULSES_PER_LITER_DEFAULT` follows with the next OTA).
Totals now match the house meter over the real mix of draws. A single factor cannot follow the turbine's curve:
long constant draws at ~10 L/min (408 on 2026-09-16) now read ~5 % high, slow ones and draw tails are what they were
missing. High is the safe direction for the volume rules.

Consequences:
- Tier 0 and Tier 1 measure time: unaffected.
- Tier 2 volume thresholds and Tier 3 learned volume limits act ~6 % earlier for the same water.
- hc-data keeps `pulses_per_liter` per event: flows before the change stay at 410 in `flow.liters`, so the learned
  volume limits are trained on liters ~6 % lower than the device now reports. Rescale old flows (pulses / 386) when the
  model is retrained, or let new data take over.
- Recheck *Kontrola wody* on the hc-viz *Dom* dashboard after a week: it should sit near 0 %.
