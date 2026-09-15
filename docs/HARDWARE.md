# Hardware

## Overview

| Part | Details |
|---|---|
| MCU | ESP32-WROOM-32 DevKit on a 7x9 cm prototype board |
| Power | Screw terminal input, TO-220 regulator (whether the actuator shares this supply is not verified) |
| Flow meter | Termipol PM-3/4-B, DN20 brass, Hall sensor, NO open-collector output (10 mA), 5-18 VDC, 2-45 L/min, 477 pulses/L ±10 %; wires: black GND, red +, yellow signal ([datasheet](datasheet-flow-meter-pm3-4-b.pdf)) |
| Valve | DN20 ball valve with an HP Control **A80 4-wire, 9-24 V DC** actuator ([manual](Manual_A80_4-wires_9-24VDC.pdf)): see [Valve actuator](#valve-actuator-hp-control-a80-4-wire-9-24-v-dc) |
| Connectors | 3-pin JST for the meter and the valve |
| Indicators | Blue LED (flow), red LED (valve closed) |
| Spare | Identical board for development and destructive tests |

| GPIO | Function | Notes |
|---|---|---|
| 4 | Flow meter pulses (PCNT, rising edge) | The PCNT driver enables the internal pull-up (legacy code set a pull-down that the driver overrode) |
| 14 | Valve control (HIGH = open) | MTMS strapping pin: outputs a signal during boot |
| 32 | Blue LED | |
| 33 | Red LED | |

## Valve actuator (HP Control A80 4-wire, 9-24 V DC)

From the manual ([Manual_A80_4-wires_9-24VDC.pdf](Manual_A80_4-wires_9-24VDC.pdf)):

| Item | Value |
|---|---|
| Supply | 9-24 V DC, permanently on red `+` and black `-` |
| Control | blue-green **shorted = open**, **open circuit = close** |
| Power | 3-6 W while moving; limit switches cut the current at rest |
| Travel | < 10 s for 90°, torque 8 Nm, ISO5211 F03 |
| Position feedback | **none** (only the 7-wire variant has open/closed signal contacts) |
| Manual override | remove the cap, press the release button underneath, turn with a 5 mm hex key; **disconnect the actuator supply first** |
| Environment | -10 to 50 °C, IP54 |

What this means for the firmware (checked against the code, nothing to change):
- **Control level.** GPIO14 HIGH turns the transistor on, which shorts blue-green and opens the valve. This matches `VALVE_LEVEL_OPEN = 1`, which the legacy firmware used for six years.
- **No drive pulses.** It is a level, not a pulse: no minimum on-time, no charge time. The 2-wire A80 needs its capacitor charged for 5 min; the 4-wire does not.
- **No position feedback.** The firmware cannot read the valve position. A failed close shows up only as flow that continues after the command: the `EV_ALERT` "water still flowing with the valve closed" after `VALVE_CLOSING_S` (15 s, above the < 10 s travel).
- **Manual override.** Turning the valve by hand while the contact holds the other position makes the actuator drive back once it is re-engaged and powered. Hence "disconnect the supply first".

## What the actuator does on a reset

The actuator has no memory of commands: it drives towards whichever position the contact state means, and its limit
switches stop it there.

| Event | Contact | Actuator |
|---|---|---|
| Firmware drives GPIO14 HIGH | closed | opens (or stays open) |
| Firmware drives GPIO14 LOW | open | closes (or stays closed) |
| ESP32 reset / bootloader (GPIO14 not driven) | open, unless something pulls the base up | **starts closing** until the firmware drives the pin again |
| ESP32 loses power, actuator still powered | open | **closes fully** (fail-safe) |
| Both lose power | - | stays where it is |

Consequences:
- **Closed valve.** A reset never moves it.
- **Open valve.** The reset window lasts about 0.9 s: ~0.35 s bootloader plus 0.56 s until `valve_restore()` in firmware
  3.x (app log on the spare board). It lets the ball travel about 1/11 of its stroke towards closed, then back. A short
  flow dip; the state does not change. Still to measure with a scope and a real actuator.
- **ESP32 power loss with the actuator still powered.** The valve closes. That is safe, but the firmware then restores "open" when it boots.
- **Legacy firmware.** Its reset window was longer (WiFi + OTA check before GPIO init, seconds).

To remove the movement completely:
- A pull-up on the transistor base keeps "open" during reset, but then a closed valve would twitch open.
- Only a latching element (bistable relay) is glitch-free both ways.
- The A80 7-wire variant (open/closed signal contacts, [HP Control wiring variants](https://hpcontrol.pl/katalog/ONLINE-HPCONTROL/WEB/Electric_Actuators/Manual_A80_A82_all.pdf)) would also give real position feedback.

## Checks before the ESP-IDF 5.4.2 firmware

Goal: **a reboot or power cut must not change the valve state** (owner requirement). The firmware will restore the last
state from NVS as the first thing in `app_main`. This check finds out whether the reset/bootloader window can still move
the actuator, i.e. whether firmware alone is enough.

Do these checks on the spare board.

1. **Valve wiring.** The actuator is known (A80 4-wire 9-24 V DC). Still to check:
   - how the TO-92 transistor switches the blue-green contact;
   - whether the board and the actuator share one supply.
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
