# N64 IGR wiring (ELEGOO Nano V3 / ATmega328P, 5 V)

| N64 point                         | Nano  | Parts / notes |
|-----------------------------------|-------|---------------|
| 5 V (U13 leg, measure ~5.0 V)     | `5V`  | Not `VIN`: VIN goes through the regulator and wants 7 V or more. |
| GND (controller port 1)           | `GND` | |
| Controller port 1 DATA            | `D2`  | 1 kΩ series resistor. Input only, no pull-up. |
| Reset-switch signal (pulled-up side) | `D3` | 220 Ω series resistor. Driven open-drain: LOW to reset, otherwise high-Z. |

Before connecting, measure the idle voltage on DATA (expect ~3.3 V) and on the
reset-switch signal (expect 3.3 V or 5 V, dropping to 0 V when you press reset).

To switch to an NPN transistor later (collector to reset, emitter to GND,
D3 → 2.2 kΩ → base, 47 kΩ base → GND), set `RESET_VIA_NPN 1` in the sketch.

## Programming

Unplug the 5 V wire (or keep the console's power cable out) while USB is connected.
Otherwise the Nano's USB 5 V back-feeds the whole N64 5 V rail.

Board: "Arduino Nano", processor "ATmega328P (Old Bootloader)" for most CH340 clones.

## Checking it works

The D13 LED blinks about once a second while a game is polling controller 1.
If it doesn't blink, the sniffer isn't decoding polls, so check DATA and GND first.

Default combo: L + R + C-Down held for 0.5 s (`RESET_COMBO` and `HOLD_MS` in the sketch).
