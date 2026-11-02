/**
 * @file heaterHandler.h
 *
 * @brief Boiler PID and the heater SSR drive (Teensy pin 2, via ULN2003)
 *
 * PHASE 3, STAGE 1. The PID runs and computes a duty cycle; the output stays
 * DISABLED IN SOFTWARE until deliberately enabled. That is not caution theatre
 * - it lets every interlock be proven before the element is ever connected,
 * which is the only order that makes sense for the one actuator on this machine
 * that can destroy a boiler.
 *
 * STRUCTURE, kept as upstream CleverCoffee has it
 *
 * Time-proportioning, not fast PWM: a mains SSR switches at zero crossings, so
 * the only sensible control is what fraction of a window it conducts for.
 * Upstream uses a 1000 ms window driven by a 10 ms timer tick, giving 100
 * steps, with the PID output scaled 0-1000 so it can be compared directly
 * against the tick counter. All of that is preserved - the ESP32 hardware timer
 * becomes an IntervalTimer and nothing else about the scheme changes.
 *
 * Tunings default to upstream's: Kp 62, Tn 52, Tv 11.5, integrator clamp 55,
 * input smoothing 0.6, setpoint 95 C. Ki and Kd are derived the way upstream
 * derives them (Ki = Kp/Tn, Kd = Kp*Tv) so the numbers in the web UI keep
 * meaning the same thing on both processors.
 *
 * HOW THE HEATER IS GATED
 *
 * Every pass of the main loop recomputes whether heating is permitted. The ISR
 * does NOT re-evaluate those conditions - it would have to call into half the
 * firmware from interrupt context to do so - but neither does it simply trust a
 * flag, because a flag set by a loop that has since hung is a lie.
 *
 * Instead the permit EXPIRES. The loop stamps it; the ISR refuses to energise
 * if the stamp is older than kPermitTimeoutMs. A hung main loop therefore drops
 * the heater within half a second, without the ISR needing to know why. This is
 * the same shape as the pump's command expiry, deliberately: the safe state is
 * what you get by DOING NOTHING, and staying on requires continuous proof of
 * life.
 *
 * The gates, all of which must hold:
 *
 *   1. the software master enable is on (default OFF, never persisted)
 *   2. no latched fault
 *   3. safety::boilerSensorHealthy - the independent gate that already exists
 *   4. the boiler RTD is healthy and its reading is fresh
 *   5. the boiler is below the over-temperature limit
 *   6. WATER IS PRESENT - the dry-fire interlock
 *   7. load shed: the pump is not running
 *   8. the permit is fresh (ISR-side, as above)
 *
 * ON GATE 6, WHICH IS CURRENTLY ALWAYS FALSE
 *
 * The water-level probe's opto-isolated circuit is not built, so there is no
 * way to know whether the boiler has water. The firmware therefore REFUSES TO
 * HEAT. That is the correct behaviour and must not be "fixed" by defaulting it
 * to true - running a 1500 W element on an unknown water level is precisely how
 * a boiler is destroyed, and an unbuilt sensor is an unknown level.
 *
 * There is a bench override for stage 2 (verifying the SSR drive electrically
 * with no element attached). It is deliberately awkward: it must be set every
 * boot, it is never persisted, and it says so loudly. See setBenchOverride().
 */

#pragma once

#include <stdint.h>

namespace heater {

    /**
     * Time-proportioning window, as upstream. One second is long enough that a
     * zero-crossing SSR has many cycles to work with and short enough that the
     * boiler's thermal mass does not notice the quantisation.
     */
    constexpr uint32_t kWindowMs = 1000;

    /** Timer tick. 10 ms into a 1000 ms window gives 100 steps of resolution. */
    constexpr uint32_t kTickMs = 10;

    /**
     * How long a permit stays valid without being refreshed.
     *
     * Must be comfortably longer than a main-loop pass and comfortably shorter
     * than the watchdog's bite, so a stalled loop drops the heater BEFORE the
     * watchdog resets the board rather than after.
     */
    constexpr uint32_t kPermitTimeoutMs = 500;

    // --- Upstream's default tunings, unchanged -------------------------
    constexpr double kDefaultSetpointC = 95.0;
    constexpr double kDefaultKp = 62.0;
    constexpr double kDefaultTn = 52.0;
    constexpr double kDefaultTv = 11.5;
    constexpr double kDefaultIntegratorMax = 55.0;
    constexpr double kDefaultEmaFactor = 0.6;

    /**
     * @brief Set up the PID and start the window timer. Output stays off.
     *
     * Call after temps::init(), outputs::init() and safetyInitOutputs().
     */
    void init();

    /**
     * @brief Run the PID and refresh the permit. Call every main-loop pass.
     *
     * Refreshing the permit is the proof-of-life the ISR depends on, so this
     * MUST be called unconditionally - not skipped when the heater happens to
     * be disabled, or the expiry logic goes untested until the day it matters.
     */
    void loop();

    /**
     * @brief The software master enable.
     *
     * Defaults OFF and is never persisted, so every boot starts with the
     * heater incapable of running. Enabling is a deliberate act, and it still
     * only means the OTHER gates get a say.
     *
     * @return false if refused, with the reason available from blockedBy()
     */
    bool setEnabled(bool on);

    /** @brief Is the software master enable on? */
    bool enabled();

    /**
     * @brief Allow heating with no water-level probe fitted. BENCH ONLY.
     *
     * For stage 2: confirming the SSR drive switches correctly with NO HEATING
     * ELEMENT CONNECTED. Never persisted, must be re-set every boot, and
     * announced loudly when used.
     *
     * This exists because the alternative is worse. Without it, the only way to
     * test the drive would be to temporarily weaken the dry-fire interlock in
     * the source - an edit that works, gets forgotten, and ships.
     */
    void setBenchOverride(bool on);

    /** @brief Is the dry-fire interlock currently overridden for bench work? */
    bool benchOverride();

    /** @brief Duty the PID is asking for, 0-100%. Computed even when disabled. */
    double dutyPercent();

    /** @brief Raw PID output, 0-1000, as upstream scales it. */
    double rawOutput();

    /** @brief Target temperature in Celsius. */
    double setpointC();

    /** @brief Set the target temperature. Clamped to something sane. */
    void setSetpointC(double celsius);

    /** @brief Set PID tunings in upstream's Kp/Tn/Tv form. */
    void setTunings(double kp, double tn, double tv);

    /** @brief Current tunings, in the same form. */
    void tunings(double* kp, double* tn, double* tv);

    /** @brief Are all gates currently satisfied? */
    bool permitted();

    /** @brief The first gate that is failing, or "ok". */
    const char* blockedBy();

    /** @brief Timer ticks since boot. Proves the ISR is actually running. */
    uint32_t tickCount();

    /** @brief Ticks during which the output was actually energised. */
    uint32_t onTickCount();

    /** @brief Print PID and gate state for the bring-up console. */
    void printStatus();

    /**
     * @brief Deliberately stall the main loop and prove the heater drops.
     *
     * The permit expiry is the mechanism that protects against a hung control
     * loop, and it is the one piece of this that cannot be verified by reading
     * the code - an ISR that quietly keeps energising a stale permit looks
     * identical from outside until the day it matters.
     *
     * So this blocks for longer than kPermitTimeoutMs without refreshing the
     * permit, exactly as a hung loop would, and reports whether the ISR
     * actually stopped driving the output. Bounded well inside the watchdog's
     * bite, and only useful with the heater already running.
     *
     * Test code, deliberately kept in the firmware rather than done once by
     * hand: a safety mechanism that is only ever tested once is a safety
     * mechanism nobody will notice breaking.
     */
    void testPermitExpiry();

} // namespace heater
