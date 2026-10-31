/**
 * @file audio.h
 *
 * @brief Startup sound on the display head's speaker
 *
 * The board carries an ES8311 codec, an FM8002E amplifier and a 2-pin speaker
 * socket, which makes a startup chime essentially free. This plays one.
 *
 * WHERE THIS SITS, AND WHY THAT IS THE POINT
 *
 * Entirely on the head. No actuator, no sensor and nothing safety-related is
 * within reach of this code, and the Teensy neither knows nor cares whether a
 * sound played. That is the right home for anything cosmetic.
 *
 * It also sets a boundary worth stating once, here, before anyone is tempted:
 * AUDIO MUST NEVER BE THE ONLY INDICATION OF ANYTHING THAT MATTERS. A speaker
 * can be unplugged, the volume can be at zero, the codec can fail to
 * initialise, and the whole display board can be disconnected while the machine
 * carries on making coffee. A beep is a convenience on top of something
 * visible; it is never the warning itself.
 *
 * NON-BLOCKING BY CONSTRUCTION
 *
 * The sound is 3.66 seconds long. Writing it to I2S in one go would stall the
 * head for 3.66 seconds, which would freeze the touch screen and stop the link
 * being serviced - telemetry would back up and the display would come up stale.
 * So playback is a small state machine fed from loop(): each pass pushes
 * however much the I2S DMA will accept right now and returns immediately.
 *
 * THE CLICK PROBLEM
 *
 * A class-D amplifier switched on or off with a live DAC behind it thumps. The
 * sequence here avoids that: amplifier shut down, codec configured, silence
 * clocked out until the DAC settles, THEN the amplifier on, play, more silence,
 * amplifier off. The trailing silence is not cosmetic either - the I2S DMA
 * holds about 90 ms of audio, so shutting the amplifier down the instant the
 * last sample is handed over would cut off the end of the sound.
 *
 * The amplifier is also shut down whenever nothing is playing, because an idle
 * class-D amp hisses, and a machine that hisses quietly all day is worse than
 * one that makes no sound at all.
 */

#pragma once

#include <stdint.h>

namespace audio {

    /**
     * @brief Bring up the codec and I2S. Safe to call with no speaker fitted.
     *
     * Must be called AFTER display::init(), because the codec shares the touch
     * panel's I2C bus and LovyanGFX is what opens it.
     *
     * @return true if the codec was found and configured. False means no
     *         sound; it is not an error worth stopping for.
     */
    bool init();

    /**
     * @brief Start the startup sound. Returns immediately.
     *
     * Does nothing if the codec is absent or something is already playing.
     *
     * @return true if playback started.
     */
    bool playStartupSound();

    /**
     * @brief Feed the I2S DMA. Call every loop pass.
     *
     * Does nothing when idle, and never blocks: it writes only what the DMA
     * will take without waiting.
     */
    void loop();

    /** @brief Is a sound currently playing (including its silent lead-out)? */
    bool isPlaying();

    /** @brief Stop immediately and shut the amplifier down. */
    void stop();

    /**
     * @brief Set playback volume, 0-100.
     *
     * 100 is 0 dB at the codec, 1 is about -50 dB, and 0 mutes. The range is
     * in half-decibel steps, so it sounds roughly linear to the ear rather
     * than being all in the top few percent.
     */
    void setVolumePercent(uint8_t percent);

    /** @brief Current volume, 0-100. */
    uint8_t volumePercent();

    /** @brief Print audio state for the serial console. */
    void printStatus();

} // namespace audio
