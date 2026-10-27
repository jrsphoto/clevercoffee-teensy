# Teensy 4.1 pinout review (Argenta SME control board)

Phase 0 of the CleverCoffee to Teensy 4.1 + ESP32-S3 port. Reviews the Teensy pin
assignments drafted in the KiCad schematic
`clevercoffee-hardware/CleverCoffee_Teensy41_Dev_SMD/` (root sheet +
`relays.kicad_sch`), verified against the real Teensy 4.1 peripheral map. Completed and
ERC-clean 2026-10-26. This table is the source of truth for `src/hardware/pinmapping.h`.

## Method

The schematic was read as a netlist (pins matched to global labels by coordinate), not
eyeballed from the PNG. Every bus assignment was cross-checked against the Teensy core
source (PaulStoffregen/cores `teensy4/pwm.c` for the FlexPWM/QuadTimer table,
PaulStoffregen/SPI for the LPSPI4 default pins) and against the symbol's own pin-name
labels, which are taken from PJRC's pinout card.

## Final pin table (= pinmapping.h)

Teensy digital pin numbers.

| Function | Teensy pin | Peripheral used |
|---|---|---|
| Heater SSR (to ULN2003 I1) | 2 | GPIO out |
| Fill valve (to ULN2003 I4) | 3 | GPIO out |
| Group valve (to ULN2003 I3) | 5 | GPIO out |
| Pump PWM (to driver board; AC mode via Pmode jumper to ULN2003 I2) | 9 | FlexPWM2_2_B |
| Flow sensor (DM60 pulse) | 10 | QuadTimer1_0 (interrupt / input-capture) |
| SPI MOSI (both MAX31865 RTDs) | 11 | SPI (LPSPI4) MOSI |
| SPI MISO (both RTDs) | 12 | SPI MISO |
| SPI SCK (both RTDs) | 13 | SPI SCK (also onboard LED) |
| Boiler RTD chip-select | 15 | GPIO |
| Group RTD chip-select | 16 | GPIO |
| I2C SDA (encoder, PCF8574, pressure) | 18 | Wire (I2C0) SDA |
| I2C SCL (same three) | 19 | Wire (I2C0) SCL |
| Brew switch | 24 | GPIO in |
| Encoder interrupt (ENC_INT) | 25 | GPIO in (R13 pull-up) |
| Water-level probe | 26 | GPIO in (R14 pull-up) |
| HX711 data 1 (SC_DAT) | 38 | GPIO in |
| HX711 clock (SC_CLK, shared) | 39 | GPIO out |
| HX711 data 2 (SC_DAT2) | 40 | GPIO in |
| ESP32-S3 link RX (Serial1 RX1) | 0 | UART |
| ESP32-S3 link TX (Serial1 TX1) | 1 | UART |

Spare pins: D4, D6, D7, D8 (freed by removing the OLED; D7/D8 = Serial2 if a second UART
is ever wanted), plus D14, D17, D20-D23, D27-D37, D41, and the analog-only A-pins.

## Why these are correct

- All hardware buses sit on their native Teensy pins: SPI on 11/12/13, I2C (Wire) on
  18/19, Serial1 on 0/1. The RTD chip-selects (15, 16) and the native hardware CS (10,
  reused here for flow) are fine because any GPIO works as a software SPI chip-select.
- Teensy 4.1 has no boot/strapping-sensitive pins among 0-54, and its onboard SD card and
  QSPI flash are on dedicated pads that do not touch pins 0-54. No assignment collides
  with the SD or flash, and there are no boot-gotcha pins to design around.
- D10 for flow is a good pick: it is QuadTimer1_0, so it supports hardware input-capture,
  not just attachInterrupt. The DM60 runs under ~1 kHz, so even plain interrupts suffice.
- D13 doubles as the onboard LED. Standard on Teensy; the LED flickers with SPI traffic,
  which is harmless.

## Findings and how they were resolved

1. **No ESP32-S3 link pins (blocker).** The schematic had no UART anywhere. Resolved:
   added Serial1 on the free D0/D1 pair (Teensy D1 TX1 to ESP32 RX; Teensy D0 RX1 from
   ESP32 TX). Both 3.3 V, no level shifter.

2. **Pump PWM had no default-off bias (safety).** The four ULN2003 outputs are held off
   at boot by input pull-downs R15-R18, but the PWM pump path bypasses the ULN2003 and had
   no pull-down, so the driver board would see a floating input from power-on until
   firmware takes over. Resolved: added a pull-down on the PMP_PWM net. (Teensy pins power
   up as floating inputs, so this bias is what guarantees "pump off at boot.")

3. **SPI OLED was a leftover from the ESP32 Rev 1.2 board.** The new architecture puts the
   color LCD on the ESP32-S3 module; the Teensy's status indicator is the encoder RGB LED.
   Resolved: removed DISPLAY1 and its four nets, freeing D4, D6, D7, D8.

4. **Pump PWM shared a FlexPWM submodule with the old OLED backlight.** D9 (pump) is
   FlexPWM2_2_B and D6 (OLED_BL) was FlexPWM2_2_A, the same submodule, so the two would
   have shared one PWM frequency. Resolved by finding 3 (OLED removed, D6 freed), so the
   pump now owns FlexPWM2_2 alone. If a second PWM load is ever needed, these are on
   separate submodules and safe alongside the pump: D4 (FlexPWM2_0), D28/D29 (FlexPWM3_1).

## ERC notes (schematic power-pin typing)

The Teensy module sources 3.3 V on-board, which confused ERC until the power-pin types
were set correctly. The rule: every supply/ground net needs exactly one declared source,
which is either one pin set to "Power output" or one PWR_FLAG. Final state:

- **+3.3 V:** one Teensy 3V3 pin set to "Power output" (the regulator source), the other
  3V3 pins set to Passive. No PWR_FLAG needed on this rail.
- **+5 V and GND:** sourced from the power-in connector and declared by the existing
  PWR_FLAGs there. All chip GND pins stay "Power input."
- **+24 V:** appears only on the ULN2003 output/COM side and the relay headers (24 V never
  touches a Teensy pin).

Pin electrical types, in plain terms: "Power output" = supplies a voltage (one per rail);
"Power input" = consumes a supply, which is every chip VDD and every GND pin; "Passive" =
a plain connection point and the spare duplicate supply pads of a module.

## KiCad changes made (for the record)

1. Added a Conn_01x04 link connector; global labels UART_TX on Teensy D1, UART_RX on D0;
   connector pins labelled UART_TX (to ESP32 RX), UART_RX (to ESP32 TX), +3V3, GND.
2. Added a 10k pull-down from the PMP_PWM net to GND near the PUMP_PWM1 header.
3. Removed the DISPLAY1 connector, its decoupling cap, and the OLED_CS/DC/RST/BL labels.
4. Set the Teensy 3V3 regulator pin to "Power output" and the other 3V3 pins to Passive.
