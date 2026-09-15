# Flow meter calibration

Meter: Termipol PM-3/4-B, DN20. Datasheet: 477 pulses/L ±10 %, rated range 2-45 L/min (±3 %), open-collector output.

The ±10 % tolerance means the real factor is anywhere between ~430 and ~525 pulses/L, so it has to be measured.

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
- The new firmware records the shortest pulse interval and a glitch counter. Any interval far below 2.8 ms points to noise, not water.

## Procedure (works with the current firmware)

1. Make sure nothing else uses water: close the other taps, no toilet refill, no garden watering.
2. Put a bucket with a measured volume mark (ideally weighed: 1 kg = 1 L) under a tap after the meter.
3. Open the tap, fill exactly to the mark (e.g. 10 L), close it, and wait 3 s for the flow event to end.
4. Read the pulse count:
   ```sh
   curl -s http://192.168.11.244/api/status
   ```
   Use the `consumption` value.
5. Repeat at three rates: trickle (~2-3 L/min), medium (~8-10 L/min), full open.
   - The legacy counter is never cleared and wraps every 32767 pulses (~69 L of total use since boot).
     - A wrap during a run adds tens of thousands of pulses.
     - Discard any run that is wildly off and repeat it.
     - Keep runs short (10 L).
   - Note the fill time, so rate = volume / time.

## Results

| Run | Volume [L] | Time [s] | Rate [L/min] | Pulses | Pulses/L |
|---|---|---|---|---|---|
| 1 | | | | | |
| 2 | | | | | |
| 3 | | | | | |
| 4 | | | | | |
| 5 | | | | | |
| 6 | | | | | |

`FLOW_PULSES_PER_LITER` = average of the runs inside the rated range. If trickle runs differ by more than ~5 %, keep a note:
the meter is non-linear below 2 L/min, so micro-leak volumes are approximate.
