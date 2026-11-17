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
 * Resolution is 49,162 pulses per litre (datasheet, 93N-6211-1100x), and the
 * sensor's own ceiling is 3.0 l/min, so the pulse rate tops out around 2,460 Hz.
 * That is slow enough that a plain interrupt is ample and no hardware counter is
 * needed, though pin 10 is on QuadTimer1_0 and could use input capture later if
 * it ever proves necessary. The 200 us debounce below permits 5 kHz, leaving
 * roughly 2x headroom over the sensor's maximum.
 *
 * THE SIGNAL PIN NEEDS A REAL PULL-UP. The datasheet's own reference circuit
 * uses 4k7 to the supply plus 10 nF to ground, and the sensor does not work
 * without it: the Teensy's ~22k internal pull-up cannot lift the line between
 * pulses, so it sits low, there is no high state to fall from, and the
 * falling-edge interrupt almost never fires. With water flowing at a good rate
 * that produced FOUR pulses in thirty seconds. The internal pull-up is still
 * enabled below, but only as a backstop - it is not sufficient alone.
 *
 * FLOW DIRECTION IS NOT OPTIONAL. The DM60 drives its turbine with a jet
 * through a 1.1 mm nozzle, so plumbed backwards the jet geometry does not turn
 * the wheel at all - water passes straight through and the sensor reports
 * nothing. There is no arrow on the body; the datasheet's dimension drawing
 * marks one port "OUT" and shows an "INLET identification" feature instead,
 * which is easy to miss. Plumbed backwards this gave FOUR pulses in thirty
 * seconds with water flowing well; plumbed correctly, 10,561 pulses in twenty.
 *
 * The giveaway for a backwards sensor is the output pulling LOW and STAYING
 * low: the turbine is not rotating, so the Hall sensor sits wherever the magnet
 * parked. An electrically dead sensor looks different - it never actuates at
 * all and the line stays HIGH.
 *
 * Mounting matters too, from the datasheet's application notes: mount the sensor
 * VERTICALLY so air bubbles pass through instead of lodging in the turbine
 * chamber, keep at least 14 cm of tube between pump and sensor, and keep it away
 * from the pump's magnetic field - the turbine is magnet-driven, so a motor
 * alongside it can corrupt the reading.
 *
 * Do not blow compressed air through it. The datasheet states that drying or
 * operating the sensor with compressed air destroys it.
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
     * Pulses per litre, from the sensor's own datasheet.
     *
     * 49,162 is the stated resolution of the Digmesa Nano DM60, part number
     * 93N-6211-1100x, which is the sensor fitted to this machine. The previous
     * default of 42,000 was the middle of the range across DM60 variants - a
     * guess, and 15% low for this one.
     *
     * Quoted accuracy is +/-2.0% across the linear range of 0.05-3.0 l/min, so
     * the factory figure is good enough to use directly. A gravimetric
     * calibration with 'flowcal' against a weighed volume would still beat it
     * and should be done once the machine can dispense onto a working scale.
     */
    constexpr uint32_t kDefaultPulsesPerLitre = 49162;

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

    /**
     * @brief Watch the signal line directly and report every level change.
     *
     * Reads the PIN rather than the interrupt count, which is the distinction
     * that matters when nothing is being counted:
     *
     *   stuck HIGH    the internal pull-up is holding the line and nothing is
     *                 pulling it down - sensor not reaching this pin, sensor
     *                 unpowered, no shared ground, or genuinely no flow
     *   stuck LOW     something is holding the line at ground - a short, or a
     *                 swapped signal and ground
     *   changing      the signal IS arriving, so a zero pulse count would mean
     *                 the interrupt is at fault rather than the wiring
     *
     * Non-blocking, like the brew switch and encoder watches - a blocking one
     * would stop the watchdog being fed.
     */
    void startWatch(uint32_t durationMs);

    /** @brief Service an active watch. Call every main-loop pass. */
    void serviceWatch();

    /**
     * @brief Sample the signal pin in a tight loop and report what it does.
     *
     * BLOCKING, for a fixed short window well inside the watchdog's 4 s bite.
     * That is the point: polling from the main loop samples at a few kHz at
     * best and misses anything brief, so "no level changes" from serviceWatch()
     * proves nothing. This samples at megahertz and cannot miss an edge.
     *
     * It answers the question the pulse counter alone cannot: if this sees
     * transitions while pulseCount() stays at zero, the INTERRUPT is broken. If
     * it sees none, the signal genuinely is not moving and the fault is the
     * sensor or its wiring.
     *
     * Also reports the fraction of the window spent low, which distinguishes a
     * line held hard at ground from one pulsing with a very short duty.
     *
     * @param windowMs how long to sample for; kept short, it blocks
     */
    void sampleFast(uint32_t windowMs);

} // namespace flow
