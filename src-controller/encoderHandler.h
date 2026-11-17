/**
 * @file encoderHandler.h
 *
 * @brief DuPPa i2cEncoder V2: rotation, button gestures, and the RGB status LED
 *
 * Ported from the ESP32 build. Two things differ on the Teensy:
 *
 *  1. Config arrives OVER THE LINK from the web UI on the ESP32-S3, which is
 *     where the settings pages now live. The values below are the defaults
 *     used until the head pushes something else, and they must match the
 *     head's registered defaults or the head will silently overwrite them at
 *     the next handshake - see the warning in CLAUDE.md.
 *
 *     ADDRESS, FITTED AND RGB-VARIANT ONLY TAKE EFFECT AT BOOT, because the
 *     driver is constructed in init() around a fixed address. The web UI marks
 *     them as needing a restart. The status colours apply immediately.
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
     * DEFAULTS. Every one of these is also a key the head can push.
     *
     * Keep them identical to the head's registered defaults in
     * headConfig.cpp. The head holds the master config and wins at every
     * handshake, so a mismatch here is not a disagreement - it is this side
     * being silently overwritten.
     */

    /**
     * I2C address, set by the A0-A6 solder jumpers on the encoder board.
     * 0x04 is the factory default (A2 fitted). The ESP32 board's unit is 0x66,
     * and THAT is the number in the shared Config.h - which is exactly the
     * mismatch described above, so the head deliberately registers 0x04.
     */
    constexpr uint8_t kDefaultAddress = 0x04;

    /** Is this the RGB-LED variant? */
    constexpr bool kDefaultHasRgbLed = true;

    /** Drive the RGB LED as a machine-status indicator. */
    constexpr bool kDefaultStatusColors = true;

    // Status colours, matching the ESP32 build's defaults.
    constexpr uint32_t kDefaultColorIdle = 0x101010;    // dim white
    constexpr uint32_t kDefaultColorHeating = 0xFF4000; // amber
    constexpr uint32_t kDefaultColorReady = 0x00FF00;   // green
    constexpr uint32_t kDefaultColorActive = 0x0000FF;  // blue
    constexpr uint32_t kDefaultColorError = 0xFF0000;   // red

    /**
     * @brief Is the encoder fitted at all? Takes effect at the next boot.
     *
     * Disabled means init() does not even probe the bus. Nothing in the
     * control path depends on the encoder, so running without it is a
     * supported configuration rather than a degraded one.
     */
    void setFitted(bool fitted);
    bool fitted();

    /** @brief I2C address. Takes effect at the next boot. */
    void setAddress(uint8_t address);
    uint8_t address();

    /** @brief Is this the RGB variant? Takes effect at the next boot. */
    void setHasRgbLed(bool has);

    /** @brief Drive the LED from machine status. Applies immediately. */
    void setStatusColorsEnabled(bool on);

    /**
     * @brief One of the five status colours. Applies immediately.
     *
     * @param which 0 idle, 1 heating, 2 ready, 3 active, 4 error
     * @return false if `which` is out of range
     */
    bool setStatusColor(int which, uint32_t rgb);

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

    /**
     * @brief Set the colour shown when the machine is idle and well.
     *
     * The shot profile owns this, so the knob says which profile is selected
     * without anyone reading a screen. SAFETY COLOURS STILL WIN: a fault or an
     * unhealthy sensor shows red regardless, and heating shows amber. A profile
     * colour must never be able to hide a fault.
     */
    void setProfileColor(uint32_t rgb);

    /**
     * @enum Gesture
     * @brief A completed button gesture, consumed once.
     */
    enum class Gesture : uint8_t {
        None,
        Short,  // one press
        Double, // two within the double-press window
        Long,   // held
    };

    /**
     * @brief Take the pending gesture, clearing it.
     *
     * A SHORT press is deliberately reported late - only once the double-press
     * window has passed without a second press. It cannot be otherwise: the
     * first press of a double looks exactly like a single press at the moment
     * it happens, so acting immediately would start a profile-1 shot every
     * time somebody asked for profile 2.
     */
    Gesture takeGesture();

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
