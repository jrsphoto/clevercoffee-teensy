/**
 * @file flowHandler.h
 *
 * @brief Digmesa DM60 Nano flow sensor: pulse counting, volume and flow rate
 *
 * NET-NEW CODE. CleverCoffee has no flow-sensor support anywhere - not in this
 * fork, not upstream master or develop. Its only shot-stop methods are by time
 * and by weight. So there is nothing to port here and nothing to cross-reference
 * against; this is written from the sensor's behaviour outwards.
 *
 * The sensor is an NPN open-collector turbine: it pulls the line to ground once
 * per fixed small volume. Pulled up to 3.3 V, never 5 V - neither chip here is
 * 5 V tolerant.
 *
 * Resolution is high, roughly 36,000-49,000 pulses per litre depending on the
 * variant, so at the pump's ~17 ml/s ceiling the pulse rate tops out under 1 kHz.
 * That is slow enough that a plain interrupt is ample and no hardware counter is
 * needed, though pin 10 is on QuadTimer1_0 and could use input capture later if
 * it ever proves necessary.
 *
 * What this feeds, eventually:
 *   - a brew-stop condition by volume, parallel to the existing by-weight one
 *   - live flow for the profiled-pump control loop
 *   - the flow-derived weight estimate that covers a Bluetooth scale dropping
 *     mid-shot (see TEENSY-PORT-BRIEF.md section 9.1)
 */

#pragma once

#include <stdint.h>

namespace flow {

    /**
     * Pulses per litre. THIS MUST BE CALIBRATED - the figure varies by variant
     * across roughly 36,000-49,000, and the only honest value is one measured on
     * this machine with this sensor.
     *
     * The default is deliberately mid-range so that an uncalibrated system is
     * obviously approximate rather than plausibly wrong. Use the console's
     * 'flowcal' against a measured volume to replace it.
     */
    constexpr uint32_t kDefaultPulsesPerLitre = 42000;

    /**
     * How long without a pulse before flow is reported as zero.
     *
     * At the lowest useful espresso flow (~0.5 ml/s) pulses still arrive every
     * ~50 ms, so 400 ms is many missed pulses - slow enough not to chatter at
     * the end of a shot, fast enough that "the pump stopped" is prompt.
     */
    constexpr uint32_t kFlowTimeoutMs = 400;

    /**
     * Window over which the flow rate is averaged. Short enough to be useful in
     * a control loop, long enough that it is not dominated by the quantisation
     * of individual pulses.
     */
    constexpr uint32_t kRateWindowMs = 200;

    /**
     * Minimum time between pulses that will be accepted, in microseconds.
     *
     * A turbine on an open-collector output can produce contact-bounce-like
     * doubles, and a long unshielded run next to a pump is not a quiet place.
     * At the physical maximum (~17 ml/s, ~830 Hz) real pulses are ~1200 us
     * apart, so rejecting anything closer than 200 us discards noise with a
     * comfortable margin and cannot discard a genuine pulse.
     */
    constexpr uint32_t kMinPulseIntervalUs = 200;

    /**
     * @brief Configure the input pin and attach the pulse interrupt.
     */
    void init();

    /**
     * @brief Update the derived rate. Call every pass of the main loop.
     *
     * Counting happens in the interrupt; this only turns counts into a rate and
     * decides when flow has stopped.
     */
    void loop();

    /**
     * @brief Zero the volume accumulator. Call at the start of a shot.
     */
    void reset();

    /** @brief Volume since the last reset(), in millilitres. */
    double totalMl();

    /** @brief Current flow rate in millilitres per second. */
    double rateMlPerSec();

    /** @brief Raw pulses since the last reset(). Diagnostics and calibration. */
    uint32_t pulseCount();

    /** @brief Pulses rejected as too close together. A health indicator: a
     *         count that climbs during normal flow means electrical noise. */
    uint32_t rejectedCount();

    /** @brief True if pulses are arriving now. */
    bool isFlowing();

    /** @brief Current calibration, pulses per litre. */
    uint32_t pulsesPerLitre();

    /**
     * @brief Set the calibration directly.
     */
    void setPulsesPerLitre(uint32_t pulses);

    /**
     * @brief Calibrate from a measured volume.
     *
     * Run water into a measuring vessel, then pass the real millilitres that
     * came out. Derives pulses-per-litre from the pulses counted since the last
     * reset(). The more volume used, the better the result.
     *
     * @return false if there are too few pulses to calibrate against.
     */
    bool calibrateTo(double actualMl);

    /**
     * @brief Print pulses, volume, rate and calibration for the console.
     */
    void printStatus();

} // namespace flow
