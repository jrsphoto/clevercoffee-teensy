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
 *    inputs, plus the pulldowns on PMP_IN1 and PMP_IN2. That
 *    hardware bias is part of the safety story, not an optional extra.
 */

#pragma once

#if defined(ARDUINO_TEENSY41) || defined(__IMXRT1062__)

/**
 * Teensy 4.1 - CleverCoffee_Teensy41_Dev_SMD
 */

// --- Inputs -------------------------------------------------------------
#define PIN_BREWSWITCH      24 // BPSW_SW1; external 10k pull-DOWN (R9) + 100nF (C5)
                               // on the signal, so the finished board is ACTIVE HIGH.
                               // JP2 is gone - see teensy-pinout-review.md.
#define PIN_WATERTANKSENSOR 41 // W_SENS1, net WATER, on 41_A17. Single boiler-level
                               // probe (AC-excited, opto-isolated), NOT an external
                               // reservoir float switch. Moved off 26_A12_MOSI1 for
                               // routing, 2026-11-24. Only ever digitalRead, so the
                               // analog capability of either pin is incidental.
                               // See documents/boiler-level-probe-sensing.md
// --- Flow: TWO sensors on the board, ONE counter in firmware so far -----
//
// FINAL PCB, 2026-11-24, read from a fresh kicad-cli netlist export:
//
//   FLOW1  FLOW_SENS1 (Gicar, 1/4" 3-wire open collector) -> 10_CS_MQSR
//          own pull-up R10 4k7, own filter C14
//   FLOW2  DIGMESA1   (Digmesa DM60 Nano, high resolution) -> 9_OUT1C
//          own pull-up R21 4k7, own filter C15
//
// Both connectors stay on the board deliberately - John, 2026-11-17. They
// were briefly on one shared net while the Digmesa was being added; they are
// not any more, and each sensor now has its own front end.
//
// THE TWO SWAPPED PINS on 2026-11-24, for PCB routing, and that CHANGED WHICH
// SENSOR IS THE ACTIVE ONE - see kActiveChannel in flowHandler.h, now Flow1.
//
// The old arrangement put the Digmesa on 10_CS_MQSR for its QuadTimer input
// capture, which only 10, 11, 12, 13, 14, 15, 18 and 19 have. That reasoning
// no longer applies and nothing is lost by it: NOTHING USES THE CAPTURE
// HARDWARE. flowHandler counts with attachInterrupt on a falling edge, which
// works on any Teensy 4.1 digital pin and is ample for either sensor - the
// DM60 is about 98 Hz at espresso flow and 740 Hz at a 15 ml/s flush, and the
// Gicar measured 28.0 Hz at 80% pump on the analyser. Capture would buy
// jitter-free per-pulse timing, which only matters if flow ever drives a
// control loop instead of totalising.
#define PIN_FLOW_GICAR   10 // FLOW1, and the ACTIVE channel
#define PIN_FLOW_DIGMESA  9 // FLOW2, counted but nothing reads it

// The pin the firmware actually counts, on the bench AND on the PCB.
//
// 10_CS_MQSR in both worlds, and after the swap it is the GICAR in both: the
// hand-wired bench has the Gicar physically on 10, and the finished board
// routes FLOW1 there. That is simpler than what came before, where this pin
// was the Gicar on the bench but the Digmesa on the board and only the
// calibration differed - by 25x.
//
// DOCUMENTATION ONLY. No code reads this define; flowHandler's sensor table
// uses the two named defines above. It is here because several comments name
// it, and because it records which pin carries the number that stops a shot.
#define PIN_FLOW            10
#define PIN_ENC_INT         25 // Duppa encoder INT, open-drain, R13 pull-up on board

// --- Outputs ------------------------------------------------------------
// Three relay/SSR channels, low-side driven through the ULN2003 (U2). Each has
// a 10k pulldown on its ULN input so outputs stay off at reset. The ULN channel
// numbers are NOT in pin order - verified against the schematic 2026-11-05,
// after a signal swap on the ULN inputs was found and fixed. outputs.h carries
// the full chain including which pulldown belongs to which net.
#define PIN_HEATER 2 // -> HEATER -> ULN I3/O3 -> HTR_RL  -> heater SSR     (R17)
#define PIN_FILL   3 // -> FILL   -> ULN I2/O2 -> FILL_RL -> fill solenoid  (R16)
#define PIN_VALVE  5 // -> GROUP  -> ULN I1/O1 -> GRP_RL  -> group solenoid (R15)
#define PIN_INLET  4 // -> INLET  -> ULN I4/O4 -> WTR_RL  -> cold inlet     (R18)
                     // Furon DVX2-144NCD2, 24 VDC coil, 2-way NORMALLY CLOSED,
                     // on the regulated cold feed. Added to the schematic
                     // 2026-11-15; read from relays.kicad_sch, where the label
                     // sits on the symbol pin whose name is 4_BCLK2, i.e. the
                     // pad printed 4. Normally closed means no power, no water.

// --- Pump, a DRV8876 H-bridge: BOTH inputs are driven -------------------
//
// The driver became a DRV8876 board on 2026-11-23, the DRV8871 having died
// with the second Teensy. In PWM mode its truth table is IDENTICAL - IN1=1/
// IN2=0 forward, 1/1 brake, 0/1 reverse, 0/0 coast - so none of the drive
// logic changed. PUMP1 grew from four pins to six; see CLAUDE.md for the
// connector and NOTES-HISTORY.md for why.
//
// IN1 is held HIGH while running and IN2 carries the PWM, so the bridge
// alternates between DRIVING and BRAKING rather than driving and coasting.
// That is slow-decay drive, and it is what the datasheet asks for when the
// point is speed control: coasting between pulses gives poor torque at low
// duty, which on the bench meant nothing moved below about 56%.
//
// The duty on IN2 is INVERTED - it is the braking fraction, so 0 on IN2 is
// full speed. pumpHandler does that conversion.
//
// ORDER MATTERS. IN1 low with IN2 high is REVERSE on this chip too, so IN2 is
// always taken down before IN1, and IN1 is always raised before IN2 is
// pulsed. Both low is coast, which is the safe state and what the board's
// pulldowns give while the Teensy pins float at reset.
#define PIN_PUMP_IN1  7 // -> PMP_IN1 -> EN/IN1, held high while running.
                        // 7_RX2_OUT1A. Moved off 9_OUT1C for routing 2026-11-24.
#define PIN_PUMP_IN2  6 // -> PMP_IN2 -> PH/IN2, carries the 20 kHz PWM.
                        // 6_OUT1D, unmoved. FlexPWM2_2_A.

// --- Pump feedback: NEITHER IS READ BY ANY FIRMWARE YET -----------------
//
// Both are new with the DRV8876 and exist only on the PCB, which has not been
// fabricated. Nothing initialises or samples them; they are here so the pin
// map is complete and so nobody allocates these pins to something else.
//
// PMP_FAULT is the driver's own nFAULT: open drain, pulled up to 3V3 on our
// board, LOW when the chip has shut the bridge down for overcurrent, supply
// undervoltage, charge-pump undervoltage or over-temperature. Unambiguous
// only because R2 on the module is bridged to 0R - at its shipped 20k it
// would also pulse on every current chop.
//
// PMP_CS is the driver's IPROPI current output into a resistor on the module:
// 2.49 V per amp, with the chip's own hardware limit at 1.33 A, so the ADC's
// full scale and the trip point coincide. Analog, hence 27_A13_SCK1 - most of
// this board's free pins cannot read analog at all.
#define PIN_PUMP_FAULT 28 // 28_RX7,      digital in, active LOW
#define PIN_PUMP_CS    27 // 27_A13_SCK1, analog in, 2.49 V/A

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
//
// Serial1. Teensy TX1 (D1) -> ESP32 RX (IO43), Teensy RX1 (D0) <- ESP32 TX
// (IO44). 3.3V both ends, no level shifter.
//
// MOVED TO Serial2/D7/D8 on 2026-11-10 and moved straight back, which is
// worth recording so nobody repeats the investigation:
//
// The first Teensy (serial 8314800) developed a dead short on D0 - 4.8 ohms
// pin-to-ground with everything disconnected, against 335k on D1 beside it.
// The firmware found the same fault independently: `rxpin` showed the pad
// held LOW against both internal pulls, and the head's 67,620 driven edges
// were swallowed. That board then lost its 3.3V rail entirely while out of
// circuit with 5V present, so the regulator failed too. A dead part, not a
// design fault and nothing to do with these pins - so the replacement goes
// back on D0/D1 and the schematic stands unchanged.
#define PIN_LINK_RX 0
#define PIN_LINK_TX 1

/**
 * The link's UART, named once.
 *
 * Kept from the brief move to Serial2: every reference in commsHandler goes
 * through this, so relocating the link is editing this file and nothing else.
 * Scattered `Serial1` references were the only reason that move touched more
 * than one place.
 */
#define LINK_SERIAL Serial1

// SPARE, read from a kicad-cli netlist export of the FINAL layout, 2026-11-24:
//
//   8_TX2_IN1, 14_A0_TX3_SPDIF_OUT, 17_A3_TX4_SDA1, 20_A6_TX5_LRCLK1,
//   21_A7_RX5_BCLK1, 23_A9_CRX1_MCLK1, 26_A12_MOSI1, 29_TX7, 30_CRX3,
//   31_CTX3
//
// plus the GPIO1 header, which already breaks out 32_OUT1B, 33_MCLK2,
// 34_RX8, 35_TX8, 36_CS and 37_CS.
//
// Two of those are not as free as they look:
//   14_A0_TX3_SPDIF_OUT is unconnected on the board but SPOKEN FOR in
//       firmware as kPinHtrRlMonitor (heaterHandler.h), the wire fitted by
//       hand for ulntest and the heater duty measurement. Taking it costs
//       that test. See BENCH.md.
//   8_TX2_IN1 came free when net LED moved off it on 2026-11-24, and Serial2
//       with it, since 7_RX2_OUT1A/8_TX2_IN1 are its pair - but 7_RX2_OUT1A
//       is now PMP_IN1, so Serial2 is NOT usable as a UART.
//
// ANYTHING ANALOG MUST LAND ON 14-27 OR 38-41. Of the spares that means
// 14, 17, 20, 21, 23 and 26 only; 29-37 are digital-only, the GPIO1 header
// included. Drawing PMP_CS on 28_RX7 cost a round trip on 2026-11-23.

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
