/**
 * @file pinmapping.h
 *
 * @brief GPIO pin mapping for the Argenta SME-JR custom board
 *
 * Matches the authoritative pin-map.md in the project root (Rev 1.6 schematic).
 * This is NOT the stock CleverCoffee pin map - the rotary encoder moved to I2C
 * (Duppa i2cEncoder V2), freeing enough GPIOs for an SPI display and the two
 * MAX31865 RTD chip-selects. Dropped entirely vs. stock: the GPIO rotary
 * encoder, the three standalone status/brew/steam LEDs (status now lives on
 * the encoder's RGB LED), the dimmer (nothing to dim on a rotary/gear pump),
 * and the power/steam/hot-water physical switches (their old pins are now
 * claimed by the encoder interrupt, SPI MISO, and the boiler-level probe).
 */

#pragma once

/**
 * Input Pins
 */

// Switches/Buttons
#define PIN_BREWSWITCH 34 // kept as-is (BPSW_SW1)

// Sensors
#define PIN_WATERTANKSENSOR 36 // NOTE: despite the name, this is now the single boiler-level probe
                                // read (AC-excited, opto-isolated - see documents/boiler-level-probe-sensing.md),
                                // NOT an external reservoir float switch. Renaming this symbol is tracked
                                // as firmware-plan.md item 5; not done yet to keep this change small.
#define PIN_FLOW   23 // flowmeter pulse (Phase 2, GICAR FM57RT)
#define PIN_HXDAT  32 // Brew scale data pin 1
#define PIN_HXDAT2 25 // Brew scale data pin 2
#define PIN_HXCLK  33 // Brew scale clock pin

/**
 * Output pins
 */

// Relays (all four low-side driven through the ULN2003 buffer, +5V required on each header)
#define PIN_VALVE  17 // group valve (GRP_SOL1)
#define PIN_PUMP   19 // moved from stock IO27 for PCB routing; IO27 is now PIN_RTD_CS_GROUP
#define PIN_HEATER 2  // heater SSR (HT_RL1)
#define PIN_FILL   4  // inlet fill solenoid relay (BLR_FILL1) - new

// RTD (MAX31865) chip-selects - both share PIN_SPI_SCK/MOSI/MISO below
#define PIN_RTD_CS_BOILER 26 // BLR_TEMP1 - PID control sensor
#define PIN_RTD_CS_GROUP  27 // GRP_TEMP1 - exchanger-outlet, flush/readiness only, not a control loop

// SPI display (DISPLAY1) - shares PIN_SPI_SCK/MOSI with the RTDs, own chip-select
#define PIN_OLED_CS 16 // OLED_RST ties to the ESP32 EN/RST pin - no dedicated reset GPIO
#define PIN_OLED_DC 18
#define PIN_OLED_BL 5 // unused for an OLED (no backlight); PWM-capable if a color TFT is fitted later.
                       // IO5 is also a boot-strapping pin - it floats high briefly at power-up, harmless here.

/**
 * Bidirectional / shared bus pins
 */
#define PIN_I2CSCL 22 // carries the Duppa encoder, ABP2 pressure sensor, OLED alt bus, PCF8574
#define PIN_I2CSDA 21

// Shared SPI bus (both RTDs + the display)
#define PIN_SPI_SCK  14 // reclaimed from JTAG TMS
#define PIN_SPI_MOSI 13 // reclaimed from JTAG TCK
#define PIN_SPI_MISO 35 // reclaimed from the old steam switch; input-only, fine since this is an input

// I2C encoder (Duppa i2cEncoder V2) - SDA/SCL ride PIN_I2CSDA/SCL above
#define PIN_ENC_INT 39 // reclaimed from the old power switch; input-only, R13 pull-up fitted on the board
