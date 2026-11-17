/**
 * @file es8311.h
 *
 * @brief Minimal ES8311 codec driver for the LCDwiki 4.0" ESP32-S3 board
 *
 * The board's audio path is ES8311 codec -> FM8002E class-D amplifier -> the
 * 2-pin speaker socket. The FM8002E is a dumb analogue amplifier with one
 * shutdown pin, so everything configurable lives in the ES8311, and the ES8311
 * is useless until its registers are set up over I2C. There is no "just write
 * to I2S and hear something" path on this board.
 *
 * WHY THIS IS HAND-ROLLED RATHER THAN A LIBRARY
 *
 * Every ES8311 Arduino library drives the codec through Wire on I2C port 0.
 * LovyanGFX has already claimed port 0 for the touch panel, using the ESP-IDF
 * driver directly rather than Wire, and the codec sits on those same two pins
 * (IO16/IO15). Two drivers on one bus is how you get intermittent, maddening
 * faults - so this talks to the codec through LovyanGFX's own I2C helper and
 * the board keeps exactly one I2C driver.
 *
 * The register sequence is not invented. It is the Espressif esp_codec_dev
 * ES8311 driver (components/esp_codec_dev/device/es8311), evaluated by hand for
 * the single configuration this board needs, with the clock coefficients taken
 * from that driver's own table:
 *
 *   slave mode (the ESP32-S3 is I2S master and generates MCLK, BCLK and LRCK)
 *   MCLK supplied on IO4 at 256 x the sample rate
 *   16-bit, 44.1 kHz, standard I2S (Philips) framing
 *   DAC only - the board's microphone is wired but unused here
 *
 * Fixing the configuration is what makes this small enough to read. The general
 * driver spends most of its size choosing between rates and clock sources; we
 * play one file at one rate, so those branches collapse to constants. The
 * comments give the derivation so the numbers can be checked rather than
 * trusted.
 *
 * Board facts, from the LCDwiki page for this exact module:
 *   IO1  amplifier enable, LOW = enabled, HIGH = shut down
 *   IO4  MCLK   (ESP32-S3 -> codec)
 *   IO5  BCLK   (ESP32-S3 -> codec)
 *   IO6  SDOUT  (ESP32-S3 -> codec, playback data)
 *   IO7  LRCK   (ESP32-S3 -> codec)
 *   IO8  SDIN   (codec -> ESP32-S3, microphone; unused)
 *   speaker socket is 1.25 mm 2-pin, max 1.5 W into 8 ohm or 2 W into 4 ohm
 *
 * NOTHING HERE IS SAFETY-RELEVANT. This is the display head; it owns no
 * actuators. A codec that fails to initialise means no sound, and the firmware
 * says so and carries on. Audio must never be the only indication of anything
 * that matters - a speaker can be unplugged, and the head can be unplugged
 * entirely while the machine keeps running.
 */

#pragma once

#include <stdint.h>

namespace es8311 {

    /**
     * The ES8311's I2C address is set by its CE pin: 0x18 with CE low, 0x19
     * with CE high. This board does not document which, so both are probed.
     */
    constexpr uint8_t kAddrLow = 0x18;
    constexpr uint8_t kAddrHigh = 0x19;

    /** Amplifier shutdown pin. LOW enables the FM8002E, HIGH shuts it down. */
    constexpr int kPinAmpEnable = 1;

    /**
     * DAC volume register value for 0 dB.
     *
     * Register 0x32 is 0.5 dB per step from -95.5 dB at 0x00, so 0xBF is unity
     * and 0xFF is +32 dB. Going above unity on a 1.5 W speaker buys distortion,
     * not loudness; make the source file louder instead.
     */
    constexpr uint8_t kVolume0dB = 0xBF;

    /**
     * @brief Find the codec on the I2C bus.
     *
     * Call only after the display is up, because LovyanGFX is what initialises
     * I2C port 0 and this deliberately borrows that one bus instance instead of
     * starting a second driver on the same pins.
     *
     * @return true if a codec acknowledged at 0x18 or 0x19.
     */
    bool probe();

    /** @brief The address the codec actually answered on, or 0 if not found. */
    uint8_t address();

    /**
     * @brief Configure the codec for 16-bit 44.1 kHz playback and unmute it.
     *
     * Does NOT enable the amplifier - see ampEnable(). Keeping those apart is
     * what allows the amplifier to stay shut down until the I2S clocks are
     * running, which is the difference between a clean start and a loud click.
     *
     * @return true if every register write was acknowledged.
     */
    bool begin();

    /**
     * @brief Set DAC volume. 0x00 is silent, 0xBF is 0 dB, 0xFF is +32 dB.
     */
    bool setVolume(uint8_t reg);

    /** @brief Mute or unmute the DAC. */
    bool setMute(bool mute);

    /**
     * @brief Enable or shut down the FM8002E amplifier.
     *
     * Shut down when idle. The amplifier is noisy with no signal, and a 1.5 W
     * class-D amp hissing quietly behind an espresso machine all day is both
     * irritating and a waste of the speaker.
     */
    void ampEnable(bool enable);

    /** @brief Put the codec back to sleep. Also shuts the amplifier down. */
    void suspend();

    /** @brief Print codec state for the serial console. */
    void printStatus();

    /**
     * @brief Read back and print the registers that begin() wrote.
     *
     * Writes are acknowledged by the bus, not by the codec agreeing with them,
     * so an acknowledged write is not proof the value stuck. This reads them
     * back and flags any that differ from what was asked for, which is the
     * difference between "the codec is configured" and "the codec was sent a
     * configuration".
     */
    void dumpRegisters();

} // namespace es8311
