/**
 * @file flowHandler.h
 *
 * @brief Flow sensing on TWO inputs: pulse counting, volume and flow rate
 *
 * NET-NEW CODE. CleverCoffee has no flow-sensor support anywhere - not in this
 * fork, not upstream master or develop. Its only shot-stop methods are by time
 * and by weight. So there is nothing to port here and nothing to cross-reference
 * against; this is written from the sensor's behaviour outwards.
 *
 * TWO INPUTS, NAMED AFTER THE NETS ON THE SCHEMATIC so the code and the KiCad
 * sheet use one vocabulary:
 *
 *   Channel::Flow1   net FLOW1, 7_RX2_OUT1A,  connector FLOW_SENS1 (Gicar)
 *   Channel::Flow2   net FLOW2, 10_CS_MQSR,   connector DIGMESA1   (DM60 Nano)
 *
 * Each has its own 4k7 pull-up and filter cap on the board, its own interrupt,
 * its own counters and its own stored calibration. They share nothing but this
 * file.
 *
 * ONE OF THEM IS THE ACTIVE ONE and it is Flow2, because that is PIN_FLOW - see
 * kActiveChannel below. Every function here that takes no Channel addresses the
 * active one, so brew-by-volume, telemetry and the console all behave exactly as
 * they did when this module knew about a single sensor. Pass a Channel only when
 * you specifically mean the other input.
 *
 * WHY THE SECOND ONE IS NOT CONNECTED TO ANYTHING YET. The board carries both
 * connectors deliberately, but no PCB has been fabricated and the bench is
 * hand-wired with the Gicar on pin 10 - physically the active channel. So Flow1
 * counts nothing today. Its pin sits at the internal pull-up with no sensor on
 * it, which produces no edges and costs nothing; if it ever starts counting on
 * an empty connector, something is driving that pin and that is worth knowing.
 *
 * THE CALIBRATIONS ARE 25x APART AND THAT IS THE REAL TRAP, not the pins. The
 * Gicar measures 1965 pulses/litre and the DM60 is 49162. Reading one sensor
 * through the other's constant gives a confident, plausible, completely wrong
 * volume - which is exactly what happened on 2026-11-16 and cost a session
 * chasing switching noise. Each channel therefore carries its OWN compiled
 * default and its OWN EEPROM record.
 *
 * BOTH sensors are NPN open-collector turbines: they pull the line to ground
 * once per fixed small volume. Pulled up to 3.3 V, never 5 V - neither chip here
 * is 5 V tolerant. Everything below about pull-ups, direction and mounting
 * applies to both, and the DM60 specifics are called out where they differ.
 *
 * The DM60's resolution is 49,162 pulses per litre (datasheet, 93N-6211-1100x)
 * and its ceiling is 3.0 l/min, so its pulse rate tops out around 2,460 Hz. The
 * Gicar is far coarser at 1,965 and was measured at 28.0 Hz with the pump at
 * 80%. Both are slow enough that a plain interrupt is ample and NEITHER channel
 * uses hardware counting today. Flow2 is on QuadTimer1_0 and could take input
 * capture later if per-pulse timing ever matters for a control loop; Flow1 is on
 * FlexPWM1_3_B and has no capture at all, which is why the high-resolution
 * sensor got Flow2. The 200 us debounce below permits 5 kHz, leaving roughly 2x
 * headroom over the faster sensor's maximum.
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
     * The two inputs, named after the nets on the schematic.
     *
     * Deliberately NOT named "primary"/"secondary" or after the sensors. The
     * net name is the one thing that is true in the firmware, the KiCad sheet
     * and the conversation at the bench all at once, and which sensor hangs off
     * which connector has already changed once.
     */
    enum class Channel : uint8_t {
        Flow1 = 0, ///< net FLOW1, 7_RX2_OUT1A,  FLOW_SENS1 (Gicar)
        Flow2 = 1, ///< net FLOW2, 10_CS_MQSR,   DIGMESA1   (DM60 Nano)
    };

    constexpr uint8_t kChannelCount = 2;

    /**
     * The channel every no-argument function below addresses.
     *
     * Flow2, because that is PIN_FLOW. On the finished board that is the
     * Digmesa, and on today's hand-wired bench it is where the Gicar is
     * physically plugged in - so it is the right answer in both worlds and
     * nothing that stops a shot has to know which board it is running on.
     */
    constexpr Channel kActiveChannel = Channel::Flow2;

    /**
     * Pulses per litre for Flow2, from the sensor's own datasheet.
     *
     * 49,162 is the stated resolution of the Digmesa Nano DM60, part number
     * 93N-6211-1100x. The previous default of 42,000 was the middle of the
     * range across DM60 variants - a guess, and 15% low for this one.
     *
     * Quoted accuracy is +/-2.0% across the linear range of 0.05-3.0 l/min, so
     * the factory figure is good enough to use directly. A gravimetric
     * calibration with 'flowcal' against a weighed volume would still beat it
     * and should be done once the machine can dispense onto a working scale.
     *
     * CAUTION ON TODAY'S BENCH. The Gicar, not a DM60, is what is physically on
     * Flow2 right now, and its real figure is 1,965. The stored record carries
     * that, so a reboot is safe - but a full EEPROM erase falls back to this
     * constant and silently over-reads by 25x. Recover with 'flowk 1965'.
     */
    constexpr uint32_t kDefaultPulsesPerLitre = 49162;

    /**
     * Pulses per litre for Flow1, MEASURED rather than from a datasheet.
     *
     * The Gicar 1/4" three-wire open collector, 1.15 mm jet, nominal 2,000.
     * 1,965 came from a 583 ml catch, the largest of three runs spanning 1,893
     * to 1,965 - the biggest catch is the one to trust, because the few
     * millilitres lost to splashing and hose are a smaller share of it.
     */
    constexpr uint32_t kGicarPulsesPerLitre = 1965;

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
     * @brief Configure BOTH input pins and attach both pulse interrupts.
     */
    void init();

    /**
     * @brief Update the derived rates on both channels. Call every main-loop pass.
     *
     * Counting happens in the interrupts; this only turns counts into a rate and
     * decides when flow has stopped.
     */
    void loop();

    /** @brief The net name of a channel, for console output and diagnostics. */
    const char* channelName(Channel channel);

    /** @brief The Teensy pin a channel counts on. */
    uint8_t channelPin(Channel channel);

    //
    // Per-channel accessors. Use these only when you specifically mean the
    // other input; everything in the control path wants the no-argument
    // versions below.
    //

    void reset(Channel channel);
    double totalMl(Channel channel);
    double rateMlPerSec(Channel channel);
    uint32_t pulseCount(Channel channel);
    uint32_t rejectedCount(Channel channel);
    bool isFlowing(Channel channel);
    uint32_t pulsesPerLitre(Channel channel);
    void setPulsesPerLitre(Channel channel, uint32_t pulses);
    bool calibrateTo(Channel channel, double actualMl);
    void printStatus(Channel channel);

    //
    // The active channel (kActiveChannel). These are what brew-by-volume,
    // telemetry and the console use, and they behave exactly as they did when
    // this module knew about one sensor.
    //

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
     * @brief Print pulses, volume, rate and calibration for BOTH channels.
     *
     * Both, deliberately. A console that showed only the active input would let
     * a sensor plugged into the wrong connector look like a dead sensor, which
     * is the single most likely mistake once two identical-looking three-pin
     * JST headers sit next to each other on the board.
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

    /** @brief As above, on a named channel. */
    void startWatch(Channel channel, uint32_t durationMs);

    /**
     * @brief Service an active watch. Call every main-loop pass.
     *
     * ONE watch runs at a time, on whichever channel started it. Two at once
     * would need two sets of state to answer a question nobody asks - the
     * point of a watch is to chase one suspect wire.
     */
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

    /** @brief As above, on a named channel. */
    void sampleFast(Channel channel, uint32_t windowMs);

} // namespace flow
