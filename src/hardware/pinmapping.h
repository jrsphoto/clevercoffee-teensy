/**
 * @file pinmapping.h
 *
 * @brief GPIO pin mapping for the Argenta SME-JR custom boards
 *
 * Two boards live here during the Teensy port:
 *
 *  - TEENSY 4.1 (the next board, CleverCoffee_Teensy41_Dev_SMD): all real-time
 *    control and all physical I/O. Pin numbers below are the reviewed, ERC-clean
 *    assignments from teensy-pinout-review.md - that document is the source of
 *    truth and was verified against the schematic netlist and the Teensy 4.1
 *    peripheral map. Do not change a pin here without changing it there.
 *
 *  - ESP32 (the current, built Rev 1.2 board): kept so the existing firmware
 *    still builds from this repo as the porting reference. Frozen - no new work.
 *
 * Teensy notes that matter:
 *  - Pins 0-54 are 3.3V and NOT 5V tolerant. 24V appears only on the ULN2003
 *    output/COM side and never reaches a Teensy pin.
 *  - There are no boot/strapping-sensitive pins in 0-54, and the onboard SD card
 *    and QSPI flash use dedicated pads that do not collide with anything here.
 *  - Teensy pins power up as floating inputs. Outputs are only guaranteed safe at
 *    power-on by the board's bias resistors: pulldowns R15-R18 on the ULN2003
 *    inputs, plus the pulldown on PMP_PWM added during the pinout review. That
 *    hardware bias is part of the safety story, not an optional extra.
 */

#pragma once

#if defined(ARDUINO_TEENSY41) || defined(__IMXRT1062__)

/**
 * Teensy 4.1 - CleverCoffee_Teensy41_Dev_SMD
 */

// --- Inputs -------------------------------------------------------------
#define PIN_BREWSWITCH      24 // BPSW_SW1; pull set by R9 + JP2
#define PIN_WATERTANKSENSOR 26 // single boiler-level probe (AC-excited, opto-isolated),
                               // NOT an external reservoir float switch.
                               // See documents/boiler-level-probe-sensing.md
#define PIN_FLOW            10 // Digmesa DM60 Nano pulse. QuadTimer1_0: supports hardware
                               // input capture, not just attachInterrupt
#define PIN_ENC_INT         25 // Duppa encoder INT, open-drain, R13 pull-up on board

// --- Outputs ------------------------------------------------------------
// All four relay/SSR channels are low-side driven through the ULN2003 (U2).
// Each ULN input has a 10k pulldown (R15-R18) so outputs stay off at reset.
#define PIN_HEATER 2 // -> ULN I1 -> heater SSR
#define PIN_FILL   3 // -> ULN I4 -> inlet fill solenoid
#define PIN_VALVE  5 // -> ULN I3 -> 3-way group solenoid

// Pump speed. FlexPWM2_2_B, and it owns that submodule outright (the only other
// pin on it, D6, was freed when the OLED came off this board), so the pump's PWM
// frequency is independent of everything else.
// The Pmode jumper routes this either to the PWM header (John's brushed-motor
// driver, variable speed) or to ULN I2 (plain on/off pump relay).
#define PIN_PUMP 9

// --- Shared SPI: both MAX31865 RTD boards -------------------------------
// The Teensy core's own pins_arduino.h already defines these three, with exactly
// the same values - which is a useful independent confirmation that the board
// uses the native SPI pins rather than something that merely happens to work.
// Guarded rather than redefined so the core stays the single source of truth and
// the build has no redefinition warnings.
#ifndef PIN_SPI_SCK
#define PIN_SPI_SCK 13 // also the onboard LED; it flickers with SPI traffic, harmless
#endif
#ifndef PIN_SPI_MOSI
#define PIN_SPI_MOSI 11
#endif
#ifndef PIN_SPI_MISO
#define PIN_SPI_MISO 12
#endif

#define PIN_RTD_CS_BOILER 15 // BLR_TEMP1 - the PID control sensor
#define PIN_RTD_CS_GROUP  16 // GRP_TEMP1 - exchanger outlet; flush/readiness only, not a control loop

// --- Shared I2C: Duppa encoder, PCF8574 buttons, brew pressure ----------
#define PIN_I2CSDA 18 // Wire (I2C0)
#define PIN_I2CSCL 19

// --- HX711 scale (two load cells sharing one clock) ---------------------
#define PIN_HXDAT  38
#define PIN_HXDAT2 40
#define PIN_HXCLK  39

// --- Link to the ESP32-S3 head ------------------------------------------
// Serial1. Teensy TX1 (D1) -> ESP32 RX, Teensy RX1 (D0) <- ESP32 TX.
// 3.3V both ends, no level shifter. Use Serial1 directly; these are for reference.
#define PIN_LINK_RX 0
#define PIN_LINK_TX 1

// Spare after the review: D4, D6, D7, D8 (D7/D8 = Serial2 if a second UART is
// ever wanted), plus D14, D17, D20-D23, D27-D37, D41 and the analog-only pins.

#else

/**
 * ESP32 Rev 1.2 - the current built board. FROZEN: porting reference only.
 * Matches pin-map.md (Rev 1.6 schematic).
 */

// Switches/Buttons
#define PIN_BREWSWITCH 34 // BPSW_SW1

// Sensors
#define PIN_WATERTANKSENSOR 36 // single boiler-level probe, not a reservoir float switch
#define PIN_FLOW            23 // flowmeter pulse (Phase 2)
#define PIN_HXDAT           32 // Brew scale data pin 1
#define PIN_HXDAT2          25 // Brew scale data pin 2
#define PIN_HXCLK           33 // Brew scale clock pin

// Relays (all four low-side driven through the ULN2003 buffer)
#define PIN_VALVE  17 // group valve (GRP_SOL1)
#define PIN_PUMP   19 // moved from stock IO27 for PCB routing
#define PIN_HEATER 2  // heater SSR (HT_RL1)
#define PIN_FILL   4  // inlet fill solenoid relay (BLR_FILL1)

// RTD (MAX31865) chip-selects
#define PIN_RTD_CS_BOILER 26
#define PIN_RTD_CS_GROUP  27

// SPI display (DISPLAY1)
#define PIN_OLED_CS 16 // OLED_RST ties to the ESP32 EN/RST pin
#define PIN_OLED_DC 18
#define PIN_OLED_BL 5

// Shared buses
#define PIN_I2CSCL 22
#define PIN_I2CSDA 21

#define PIN_SPI_SCK  14
#define PIN_SPI_MOSI 13
#define PIN_SPI_MISO 35 // input-only, fine as an input

#define PIN_ENC_INT 39 // input-only, R13 pull-up fitted

#endif
