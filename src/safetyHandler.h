/**
 * @file safetyHandler.h
 *
 * @brief Fail-safe state, boot self-check and hardware watchdog (Teensy)
 *
 * This machine has NO external fail-safe relays. The Teensy is the only active
 * line of defense. The single non-MCU backstop is a passive inline TCO thermal
 * fuse in the heater line, which this firmware neither knows about nor relies on.
 *
 * Design rules this file exists to enforce:
 *
 *  1. Outputs are driven to their safe state (heater off, pump off, valves
 *     de-energized) as the very first thing that happens at boot, before any
 *     object is constructed and before any config is read.
 *  2. Anything can call safetyHardOff() at any time, including from an ISR. It
 *     touches nothing but the output pins, so it cannot itself fail on a
 *     half-initialized system.
 *  3. A latched fault is sticky. It is cleared only by an explicit operator
 *     action, never by the condition going away on its own - a sensor that
 *     flickers back to plausible must not silently re-energize a heater.
 *  4. The watchdog resets the chip if the main loop stops feeding it. A reset
 *     lands in a safe state because the board's bias resistors (ULN2003 input
 *     pulldowns R15-R18, plus the PMP_PWM pulldown) hold every output off while
 *     the Teensy pins are floating inputs during reset.
 *
 * Safe state is defined by the hardware: all four ULN2003 channels are low-side
 * driven, so LOW on a Teensy pin = channel off = de-energized. The pump PWM path
 * is the same polarity: 0 duty = stopped.
 */

#pragma once

#include <Arduino.h>

#include "hardware/pinmapping.h"

/**
 * @brief Why the machine latched a fault.
 *
 * Ordered roughly by severity. Keep in sync with safetyFaultName().
 */
enum class FaultCode : uint8_t {
    None = 0,
    BootSelfCheck,   // a power-on check failed
    SensorFault,     // MAX31865 fault flag, open or short circuit
    SensorImplausible, // reading outside any physically sensible range
    SensorStale,     // sensor stopped updating
    OverTemperature, // boiler RTD above the hard limit
    OverPressure,    // brew pressure above the hard limit
    LowWater,        // dry-fire interlock: no water confirmed at the probe
    WatchdogReset,   // we booted because the watchdog fired
};

inline const char* safetyFaultName(const FaultCode code) {
    switch (code) {
        case FaultCode::None: return "none";
        case FaultCode::BootSelfCheck: return "boot self-check";
        case FaultCode::SensorFault: return "sensor fault";
        case FaultCode::SensorImplausible: return "sensor implausible";
        case FaultCode::SensorStale: return "sensor stale";
        case FaultCode::OverTemperature: return "over temperature";
        case FaultCode::OverPressure: return "over pressure";
        case FaultCode::LowWater: return "low water";
        case FaultCode::WatchdogReset: return "watchdog reset";
    }

    return "unknown";
}

/**
 * @brief Drive every actuator to its safe state. Safe to call from anywhere.
 *
 * Deliberately uses raw pin writes rather than the Relay/GPIOPin objects: this
 * must work before those objects exist and must not depend on any other part of
 * the firmware being healthy. Do not add logging, config lookups or allocations
 * here - it is called from fault paths and potentially from an ISR.
 */
inline void safetyHardOff() {
    // Re-assert the pin direction every time. If something (a brownout, a
    // peripheral reset, a stray pinMode) left a pin as an input, setting the
    // level alone would do nothing.
    pinMode(PIN_HEATER, OUTPUT);
    digitalWriteFast(PIN_HEATER, LOW);

    pinMode(PIN_PUMP, OUTPUT);
    digitalWriteFast(PIN_PUMP, LOW);

    pinMode(PIN_VALVE, OUTPUT);
    digitalWriteFast(PIN_VALVE, LOW);

    pinMode(PIN_FILL, OUTPUT);
    digitalWriteFast(PIN_FILL, LOW);
}

namespace safety {

    inline FaultCode latchedFault = FaultCode::None;
    inline const char* latchedDetail = "";

    /**
     * @brief True once a fault has been latched. Sticky until explicitly cleared.
     */
    inline bool isFaulted() {
        return latchedFault != FaultCode::None;
    }

    /**
     * @brief Latch a fault and immediately force the machine safe.
     *
     * The first fault wins: later faults are usually consequences of the first,
     * and the original cause is the one worth reporting.
     *
     * @param code   Which fault
     * @param detail Short static string for logs/telemetry. Must outlive the call
     *               (a string literal), since nothing here copies it.
     */
    inline void latchFault(const FaultCode code, const char* detail = "") {
        safetyHardOff();

        if (latchedFault == FaultCode::None) {
            latchedFault = code;
            latchedDetail = detail;
        }
    }

    /**
     * @brief Clear a latched fault. Only ever call this from a deliberate
     *        operator action, never automatically on the condition clearing.
     */
    inline void clearFault() {
        latchedFault = FaultCode::None;
        latchedDetail = "";
    }

    /**
     * @brief Heartbeat proving the control loop is still running and its sensor
     *        data is fresh.
     *
     * Set by the main loop each pass. The heater drive (phase 3) checks this
     * before it is allowed to energize, so a hung or starved main loop drops the
     * heater instead of leaving the SSR commanded on at whatever duty it last had.
     */
    inline volatile uint32_t lastControlLoopMs = 0;

    /**
     * @brief Is the boiler temperature reading currently fresh and believable?
     *
     * Published by the temperature handler each loop; consumed by the output
     * layer to gate the heater. Lives here rather than in tempHandler so the
     * output layer does not have to depend on the sensor layer - safety is the
     * common ground between them.
     *
     * Starts false: the heater is not permitted until something affirmatively
     * says the sensor is good, which is the correct default at boot.
     */
    inline bool boilerSensorHealthy = false;

    inline void noteControlLoopAlive() {
        lastControlLoopMs = millis();
    }

    /**
     * @brief How long since the control loop last reported in.
     */
    inline uint32_t controlLoopAgeMs() {
        return millis() - lastControlLoopMs;
    }

} // namespace safety

/**
 * @brief Put the machine in its safe state at power-on.
 *
 * Call this as the FIRST statement in setup(), before Serial, before config,
 * before anything. Between reset and this call the outputs are held off only by
 * the board's bias resistors; the job here is to take deliberate control of them
 * as early as possible.
 */
inline void safetyInitOutputs() {
    safetyHardOff();
}

/**
 * @brief Start the hardware watchdog.
 *
 * Deliberately started AFTER the slow parts of setup() so a long but legitimate
 * boot cannot trip it, and BEFORE the first loop() so the running machine is
 * always covered.
 */
void safetyWatchdogInit();

/**
 * @brief Feed the watchdog. Called once per main loop pass.
 *
 * Never call this from inside a wait loop or a retry, which would defeat the
 * point: a stuck operation must be allowed to trip the watchdog.
 */
void safetyWatchdogFeed();

/**
 * @brief True if this boot was caused by the watchdog resetting us.
 *
 * Checked during the boot self-check so a watchdog reset is visible rather than
 * looking like a normal power-on.
 */
bool safetyWasWatchdogReset();
