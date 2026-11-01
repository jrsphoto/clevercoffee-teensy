/**
 * @file weightHandler.h
 *
 * @brief HX711 load cell: brew-by-weight (Teensy pins 38 DAT, 39 CLK, 40 DAT2)
 *
 * Phase 2 hardware bring-up. Reads the load cell, tares it, calibrates it and
 * reports a weight in grams. It does not stop a shot - the brew state machine
 * is phase 3 - but it provides the one thing that machine will need, which is an
 * honest answer to "do I currently know the weight".
 *
 * WHY THIS IS NOT CALLED scaleHandler
 *
 * Upstream's name is taken. src/scaleHandler.h is the ESP32-era header, still
 * included by brewHandler.h and the frozen reference build, and overwriting it
 * would break that build. It also could not be reused as-is: it reaches into
 * the config system, the display and the brew state machine, none of which are
 * ported yet. When the head is split out properly in phase 6 and the ESP32-era
 * file retires, this can take the upstream name.
 *
 * The hardware layer underneath IS upstream's: Scale and HX711Scale are used
 * unchanged, so the reading path and the dual-cell support match upstream and
 * stay cross-referenceable.
 *
 * WHAT IS DELIBERATELY DIFFERENT: STARTING UP WITHOUT BLOCKING
 *
 * HX711Scale::init() spins in `while (!loadCell->startMultiple(...))` for a
 * 5 second stabilisation plus a tare. On the ESP32 that is merely slow. Here it
 * is wrong for two reasons: it stalls the link and the console for five seconds,
 * and it is the kind of unbounded wait that this firmware should not contain at
 * all. So the stabilisation is driven from loop() as a state machine instead,
 * which costs nothing - the machine runs normally while the scale settles - and
 * is bounded by kStartTimeoutMs no matter how the hardware misbehaves.
 *
 * The library's own tare timeout is deliberately DISABLED and replaced with
 * that bound. Its implementation uses a function-level `static` for the deadline
 * (HX711_ADC.cpp, startMultiple), so it is computed once per program run rather
 * than once per attempt: any second attempt compares against a long-past
 * deadline and reports a tare timeout immediately. Disabling it and timing the
 * attempt here makes a retry actually work.
 *
 * SAFETY
 *
 * The scale owns no actuator and must never gate safety. One property matters
 * enough to state plainly, because getting it wrong is how a scale fault turns
 * into a flood:
 *
 *   BREW-BY-WEIGHT MUST HAVE A TIME BACKSTOP. Stopping a shot "when the weight
 *   reaches the target" means waiting for a number that a broken, unplugged or
 *   mis-tared scale will never produce - and waiting means the pump stays on.
 *   So phase 3 must run a maximum-time limit alongside the weight target,
 *   always, and must treat the weight as unusable when healthy() is false.
 *   That is what healthy() exists for. It is not a diagnostic nicety.
 *
 * A stale reading is treated as no reading rather than as the last good value,
 * for the same reason it is in tempHandler: a frozen number looks exactly like
 * a working one to anything downstream.
 *
 * TWO HARDWARE THINGS WORTH KNOWING
 *
 * 1. CONVERSION RATE. Most HX711 breakout boards tie the RATE pin low, giving
 *    10 samples per second. The library averages a dataset of up to 32 of them,
 *    so at the default that is over three seconds of lag - fine for a bench
 *    reading, useless for stopping a shot. kDefaultSamples is therefore well
 *    below the maximum, and is settable; see the note on it. If better
 *    resolution and speed are both wanted, the module's RATE pin has to be
 *    lifted to 3.3 V for 80 SPS, which is a hardware change.
 *
 * 2. INTERRUPTS DURING A READ. HX711Scale.h sets SCK_DISABLE_INTERRUPTS to 0,
 *    so interrupts stay enabled while the 24 bits are clocked out by hand. The
 *    HX711 powers itself down if its clock line is held high for more than
 *    about 60 microseconds, so a long interrupt in the wrong place corrupts a
 *    reading. The flow sensor's interrupt is the only one running and it is a
 *    few microseconds, so this should be fine - but if readings are ever
 *    erratic in a way that correlates with water flowing, that macro is the
 *    first thing to try.
 */

#pragma once

#include <stdint.h>

namespace weight {

    /**
     * Stabilisation time before taring, as upstream uses. Five seconds is the
     * top of the range the library's author recommends for tare accuracy, and
     * it is free here because nothing waits for it.
     */
    constexpr uint32_t kStabiliseMs = 5000;

    /**
     * Hard bound on the whole start-up attempt. Generous against kStabiliseMs
     * so a slow but healthy start is never mistaken for a failure, while an
     * absent or dead HX711 is still reported rather than waited on forever.
     */
    constexpr uint32_t kStartTimeoutMs = 12000;

    /**
     * Moving-average length.
     *
     * The trade-off is lag against noise, and at 10 conversions per second it
     * is a real one: 16 samples is 1.6 seconds of averaging. That is still slow
     * for a shot, and is a deliberate starting point rather than a final answer
     * - find the lowest value that is quiet enough on this machine, and raise
     * the conversion rate in hardware if 80 SPS is needed.
     */
    constexpr int kDefaultSamples = 16;

    /**
     * How long without a fresh conversion before the weight is called stale.
     *
     * At 10 SPS a conversion is due every 100 ms, so 1 second is many missed
     * ones - unambiguous, without chattering on a single slow sample.
     */
    constexpr uint32_t kStaleAfterMs = 1000;

    /**
     * @brief Create the scale and begin stabilising. Returns immediately.
     *
     * @return true if the hardware object was created. Whether an HX711 is
     *         actually attached is not known yet - that is decided during
     *         stabilisation, so watch ready() and present().
     */
    bool init();

    /** @brief Service stabilisation, taring and conversions. Call every pass. */
    void loop();

    /** @brief Is the HX711 responding at all? */
    bool present();

    /** @brief Has it stabilised and tared, so the weight means something? */
    bool ready();

    /**
     * @brief Is the weight usable right now?
     *
     * present AND ready AND the last conversion is recent. THIS IS THE ONE
     * PHASE 3 MUST CHECK before using a weight to decide anything - see the
     * header's note on the time backstop.
     */
    bool healthy();

    /** @brief Latest weight in grams. Meaningless unless healthy(). */
    float grams();

    /** @brief Milliseconds since the last successful conversion. */
    uint32_t msSinceLastReading();

    /** @brief Start a non-blocking tare. Watch tareInProgress(). */
    void startTare();

    /** @brief Is a tare still running? */
    bool tareInProgress();

    /**
     * @brief Calibrate from a known mass currently sitting on the cell.
     *
     * @param knownGrams the actual mass on the scale, which must be positive
     *                   and large enough to be meaningful
     * @return false if refused - not ready, or an implausible mass
     */
    bool calibrateTo(float knownGrams);

    /** @brief Current calibration factor. */
    float calFactor();

    /** @brief Set the calibration factor directly. */
    void setCalFactor(float factor);

    /** @brief Set the moving-average length. See kDefaultSamples. */
    void setSamples(int samples);

    /** @brief Current moving-average length. */
    int samples();

    /** @brief Retry start-up, for a scale plugged in after boot. */
    void restart();

    /** @brief Print scale state for the bring-up console. */
    void printStatus();

} // namespace weight
