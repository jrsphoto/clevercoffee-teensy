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
     * Range outside which a reading is treated as physically impossible rather
     * than merely alarming. A PT100 reading -40 C or 300 C in a kitchen is a
     * broken sensor or broken wiring, not a temperature.
     */
    constexpr double kPlausibleMinC = -20.0;
    constexpr double kPlausibleMaxC = 200.0;

    /**
     * How long the boiler reading may go without a successful update before it
     * is treated as stale. The sensor layer refreshes every 400 ms, so this is
     * several missed cycles, not a hair trigger.
     */
    constexpr uint32_t kStaleAfterMs = 3000;

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

} // namespace temps
