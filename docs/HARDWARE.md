# Hardware

## Overview

| Part | Details |
|---|---|
| MCU | ESP32-WROOM-32 DevKit on a 7x9 cm prototype board |
| Power | Screw terminal input, TO-220 regulator |
| Flow meter | Termipol PM-3/4-B, DN20 brass, open-collector output, 5-18 VDC |
| Valve | Motorised DN20 ball valve (blue actuator), single control line |
| Connectors | 3-pin JST for the meter and the valve |
| Indicators | Blue LED (flow), red LED (valve closed) |
| Spare | Identical board for development and destructive tests |

| GPIO | Function | Notes |
|---|---|---|
| 4 | Flow meter pulses (PCNT, rising edge) | The PCNT driver enables the internal pull-up (legacy code set a pull-down that the driver overrode) |
| 14 | Valve control (HIGH = open) | MTMS strapping pin: outputs a signal during boot |
| 32 | Blue LED | |
| 33 | Red LED | |

## Checks before the ESP-IDF 5.4.2 firmware

Goal: **a reboot or power cut must not change the valve state** (owner requirement). The firmware will restore the last
state from NVS as the first thing in `app_main`. This check finds out whether the reset/bootloader window can still move
the actuator, i.e. whether firmware alone is enough.

Do these checks on the spare board.

1. **Valve wiring.** Identify:
   - the actuator model and wiring variant (e.g. CR01: power + control line, CR02/CR05: two direction lines, with or without feedback);
   - what the TO-92 transistor drives;
   - what the actuator does with the control line floating, LOW and HIGH.
2. **Boot window.** Scope or logic analyser on GPIO14 (and on the transistor collector) during:
   - `EN` reset,
   - power-on,
   - a software `esp_restart()`.

   Measure how long the line is in the wrong state before firmware drives it.
3. **Actuator reaction.** With the valve closed, apply the measured glitch to the actuator (or reset the spare wired to a
   spare valve). Does the ball move?
4. **Meter input.** Measure the idle voltage on GPIO4 with the meter connected and no flow. Check whether an external
   pull-up exists; the internal one (~45 kΩ) is weak for a 30 cm+ cable near a motor. Run the pulse diagnostics
   (`POST /api/diag`) with the valve actuator moving to see noise.

## Possible outcomes

| Result | Fix |
|---|---|
| No movement during the boot window | Restore from NVS early in `app_main` (plan default) |
| Short glitch moves the actuator | Hardware: pull resistor on the transistor base to hold the "no change" level, or move valve control to a non-strapping pin (e.g. GPIO25/26/27) |
| Actuator needs a held level and the level is lost during reset | Latching driver (e.g. flip-flop / bistable relay) so the MCU only sends open/close pulses |

Results and the final wiring diagram go here, and are also used for the README schematic.
