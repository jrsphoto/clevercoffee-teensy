/**
 * @file tempHandler.h
 *
 * @brief Both MAX31865 PT100 RTDs, and the safety rules that depend on them
 *
 * Two probes on one shared SPI bus, differing only by chip-select:
 *
 *   BOILER (CS pin 15) - the PID control sensor, and the one safety depends on.
 *       A fault here is a hard fault: the heater is the thing that can do damage,
 *       and running it on a temperature you cannot trust is exactly how a boiler
 *       gets destroyed. Any fault, implausible value or stale reading latches the
 *       machine safe.
 *
 *   GROUP (CS pin 16) - mounted on the heat-exchanger outlet line, not the brass
 *       casting. Feeds flush/readiness logic only; it is NOT a control loop and
 *       NOT a safety input. A fault here is reported and nothing more. Treating it
 *       as critical would make the machine refuse to work over a sensor that
 *       cannot cause harm - and it also means the machine runs correctly with the
 *       group probe simply not fitted yet, which is the state during bring-up.
 *
 * That asymmetry is the main design decision in this file. It is deliberate.
 */

#pragma once

#include <stdint.h>

namespace temps {

    /**
     * Hard over-temperature cutoff on the boiler, in degrees C.
     *
     * The machine is a heat-exchanger design whose steam boiler normally sits
     * around 120-125 C, so this is set well above any legitimate operating point
     * but far below anything structurally dangerous. It is a last-resort firmware
     * backstop, not a control limit - the PID should never come close.
     *
     * PROVISIONAL: confirm against a real pressurised boiler reading once the
     * machine is reassembled, alongside the TCO thermal fuse grade (which is
     * tracked separately in parts-list.md and is deliberately NOT known to this
     * firmware).
     */
    constexpr double kBoilerOverTempC = 140.0;

    /**
     * @brief The over-temperature limit actually in force.
     *
     * Normally kBoilerOverTempC. It can be LOWERED at runtime, which exists for
     * one reason: a cutoff that has never fired is a cutoff nobody has tested,
     * and a bench boiler cannot be driven to 140 C to find out. Dropping the
     * limit below ambient makes the gate fire immediately and proves the whole
     * path - detection, fault latching, and the heater refusing to run.
     *
     * It can never be RAISED above kBoilerOverTempC. A settable safety limit
     * that can be moved upward is not a safety limit, so setOverTempLimitC()
     * refuses, and the ceiling is a compile-time constant rather than anything
     * a config file or the display could reach.
     *
     * Deliberately not persisted. A lowered limit is safe but surprising, and
     * it should not survive a reboot and quietly make the machine useless.
     */
    double overTempLimitC();

    /**
     * @brief Lower the over-temperature limit, for testing the cutoff.
     *
     * @return false if refused - above kBoilerOverTempC, or implausibly low
     */
    bool setOverTempLimitC(double celsius);

    /** @brief Restore the limit to kBoilerOverTempC. */
    void resetOverTempLimit();

    /**
     * Range outside which a reading is treated as physically impossible rather
     * than merely alarming. A PT100 reading -40 C or 300 C in a kitchen is a
     * broken sensor or broken wiring, not a temperature.
     */
    constexpr double kPlausibleMinC = -20.0;
    constexpr double kPlausibleMaxC = 200.0;

    /**
     * How long the boiler reading may go without a successful update before it
     * is treated as stale. The sensor layer refreshes every 400 ms, so this is
     * five missed cycles - enough to ride out a transient SPI glitch, short
     * enough that the heater is never driven on a stale number for long.
     *
     * Deliberately shorter than the sensor base class's own error threshold,
     * which needs ten consecutive failures (about four seconds) before it
     * admits to a problem.
     */
    constexpr uint32_t kStaleAfterMs = 2000;

    /**
     * @brief Start the SPI bus and both RTD sensors.
     *
     * Call after safetyInitOutputs() and outputs::init(). Does not itself latch
     * faults - selfCheck() decides whether the machine is fit to run.
     */
    void init();

    /**
     * @brief Boot self-check: is the boiler sensor present and believable?
     *
     * The group sensor is deliberately not part of this. It is monitoring only,
     * and may legitimately not be fitted.
     *
     * @return true if the boiler sensor can be trusted.
     */
    bool selfCheck();

    /**
     * @brief Read both sensors and enforce the boiler safety rules.
     *
     * Call every pass of the main loop. Latches a safety fault (which drops every
     * actuator) on a boiler sensor fault, an implausible boiler reading, or a
     * boiler reading that has gone stale.
     */
    void loop();

    /** @brief Last good boiler temperature, degrees C. */
    double boilerC();

    /** @brief Last good group temperature, degrees C. */
    double groupC();

    /** @brief True if the boiler sensor is currently trustworthy. */
    bool boilerHealthy();

    /** @brief True if the group sensor is currently reporting sensibly. */
    bool groupHealthy();

    /**
     * @brief Print a human-readable line for each sensor, for the console.
     */
    void printStatus();

    /**
     * @brief Bring-up aid: dump raw measured resistance for both probes.
     */
    void printDiagnostics();

    /**
     * @brief Bring-up aid: force the boiler sensor to fail, to test safety paths.
     */
    void injectBoilerFault(bool on);

    /** @brief Is boiler fault injection currently active? */
    bool boilerFaultInjected();

    /**
     * @brief Bench aid: override the group temperature with a fixed value.
     *
     * The cooling flush decides what to do from the group reading, and on a
     * bench at room temperature it will always and correctly decide to do
     * nothing. That leaves the whole path untested until there is a hot boiler,
     * which is the wrong time to find out it is wrong.
     *
     * So the group reading can be forced. This is SAFE to expose in a way that
     * forcing the boiler reading would not be: the group sensor drives the
     * flush and readiness hints only, never latches a fault, and never gates
     * the heater. Lying about it cannot energise anything that was not already
     * permitted.
     *
     * Overriding also marks the sensor healthy, because a forced value with
     * groupHealthy() false would be ignored by every caller and so test
     * nothing.
     *
     * @param degreesC temperature to report
     */
    void overrideGroupC(double degreesC);

    /** @brief Stop overriding the group temperature and read the probe again. */
    void clearGroupOverride();

    /** @brief Is the group temperature currently forced? */
    bool groupOverridden();

} // namespace temps
