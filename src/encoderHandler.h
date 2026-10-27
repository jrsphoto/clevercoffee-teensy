/**
 * @file encoderHandler.h
 *
 * @brief DuPPa i2cEncoder V2: rotation, button gestures, and the RGB status LED
 *
 * Ported from the ESP32 build. Two things differ on the Teensy:
 *
 *  1. Config comes from constants here rather than the web UI, because the web
 *     UI now lives on the ESP32-S3 and reaches this side over the UART. Those
 *     values will arrive over the link in phase 6; until then they are plain
 *     constants in one place.
 *
 *  2. The address is 0x04, not the 0x66 the ESP32 board used. That is the
 *     factory state of a DuPPa with NO address jumpers soldered. It is in the
 *     I2C spec's reserved range, which is why a correct scanner skips it, which
 *     is exactly how this encoder managed to look dead while working perfectly.
 *     Soldering jumpers A1+A2+A5+A6 would move it to 0x66 and match the other
 *     board; until someone does, 0x04 is what is actually on the bus and the
 *     firmware says so plainly rather than pretending otherwise.
 *
 * The encoder is Teensy-owned on purpose: it carries the real control gestures
 * AND the machine-status RGB LED, both of which belong to the side that owns
 * the machine state. An ESP32 or display failure must never take the encoder
 * with it.
 */

#pragma once

#include <stdint.h>

namespace encoder {

    /**
     * I2C address, set by solder jumpers on the encoder board itself.
     * This unit has A2 soldered = 0x04. The ESP32 board's unit is 0x66.
     */
    constexpr uint8_t kAddress = 0x04;

    /** Is this the RGB-LED variant? */
    constexpr bool kHasRgbLed = true;

    /** Drive the RGB LED as a machine-status indicator. */
    constexpr bool kStatusColorsEnabled = true;

    // Status colours, matching the ESP32 build's defaults.
    constexpr uint32_t kColorIdle = 0x101010;    // dim white
    constexpr uint32_t kColorHeating = 0xFF4000; // amber
    constexpr uint32_t kColorReady = 0x00FF00;   // green
    constexpr uint32_t kColorActive = 0x0000FF;  // blue
    constexpr uint32_t kColorError = 0xFF0000;   // red

    /**
     * @brief Construct and configure the encoder. Call after i2c::init().
     *
     * @return true if the encoder acknowledged on the bus.
     */
    bool init();

    /**
     * @brief Poll for events and update the status LED. Call every loop pass.
     */
    void loop();

    /** @brief True if the encoder was found and initialised. */
    bool isPresent();

    /** @brief Net rotation since the last read, then cleared. */
    int32_t takeRotation();

    /** @brief Current absolute counter value. */
    int32_t counter();

    /** @brief Print encoder state for the console. */
    void printStatus();

    /**
     * @brief Force the status LED to a colour, ignoring machine state.
     *
     * Bring-up aid: proves the LED and the write path work without needing the
     * machine to be in a particular state.
     */
    void setLedColor(uint32_t rgb);

} // namespace encoder
