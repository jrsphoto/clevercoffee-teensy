/**
 * @file main.cpp
 *
 * @brief Teensy 4.1 control firmware for the Argenta SME espresso machine
 *
 * PHASE 1 SKELETON. This boots, proves the board is alive, holds everything in a
 * safe state and feeds the watchdog. It deliberately does not yet control
 * anything: the handlers are ported in later phases (see TEENSY-PORT-BRIEF.md).
 *
 * Division of labour, for anyone reading this cold:
 *   - This Teensy owns all real-time control, all physical I/O and ALL safety.
 *   - An ESP32-S3 module over Serial1 owns WiFi, the web UI and the colour LCD.
 *     It is a head only. It holds no actuators and no safety, and the machine
 *     runs and finishes shots normally with it unplugged.
 *
 * Structure follows upstream CleverCoffee on purpose: small hardware interfaces
 * (TempSensor / Relay / Switch / Scale), feature logic in xHandler modules, one
 * setup() and one loop(). The hardware layer underneath is swapped for Teensy
 * and a comms layer is added; the shape stays recognizable so upstream changes
 * remain cross-referenceable.
 */

#include <Arduino.h>

#include "hardware/pinmapping.h"
#include "safetyHandler.h"

// Version of the Teensy control firmware specifically. Independent of the
// upstream CleverCoffee version carried in VERSION.txt.
constexpr char kTeensyFirmwareVersion[] = "0.1.0-phase1";

namespace {

    /**
     * Serial console on the USB port. Used for bring-up and diagnostics; it is
     * NOT the ESP32 link (that is Serial1) and nothing about control depends on
     * anyone being connected to it.
     */
    constexpr uint32_t kConsoleBaud = 115200;

    /**
     * How long to wait for a USB console at boot before giving up and carrying
     * on. The machine must boot with nothing plugged in, so this is a short
     * courtesy pause for the bench, never a requirement.
     */
    constexpr uint32_t kConsoleWaitMs = 1500;

    /**
     * Main loop pacing. The real control cadence arrives with the PID in phase 3;
     * for now this just keeps the skeleton's heartbeat readable.
     */
    constexpr uint32_t kHeartbeatIntervalMs = 2000;

    uint32_t lastHeartbeatMs = 0;

    /**
     * @brief Power-on checks that must pass before the machine is allowed to do
     *        anything.
     *
     * Phase 1 only knows how to check the things that exist in phase 1. As
     * sensors land in phase 2 this grows: RTD fault flags, a plausible boiler
     * temperature, the water probe reading something valid, the I2C devices
     * acknowledging at their expected addresses.
     *
     * @return true if the machine is fit to run.
     */
    bool runBootSelfCheck() {
        bool ok = true;

        // A watchdog reset means the previous run stopped feeding the loop. That
        // is never normal, and it must be visible rather than silently absorbed
        // into what looks like a fresh power-on.
        if (safetyWasWatchdogReset()) {
            Serial.println("SELF-CHECK: previous run ended in a WATCHDOG RESET");
            safety::latchFault(FaultCode::WatchdogReset, "previous run stalled");
            ok = false;
        }

        return ok;
    }

    void printBanner() {
        Serial.println();
        Serial.println("========================================");
        Serial.print("CleverCoffee Teensy control firmware ");
        Serial.println(kTeensyFirmwareVersion);
        Serial.println("Argenta SME - Teensy 4.1 + ESP32-S3");
        Serial.println("========================================");
        Serial.print("Safe state asserted on pins: heater=");
        Serial.print(PIN_HEATER);
        Serial.print(" pump=");
        Serial.print(PIN_PUMP);
        Serial.print(" valve=");
        Serial.print(PIN_VALVE);
        Serial.print(" fill=");
        Serial.println(PIN_FILL);
    }

} // namespace

void setup() {
    // FIRST. Before the console, before config, before any object is built.
    // Until this runs, the outputs are held off only by the board's bias
    // resistors; this takes deliberate control of them.
    safetyInitOutputs();

    Serial.begin(kConsoleBaud);

    const uint32_t consoleWaitStart = millis();

    while (!Serial && (millis() - consoleWaitStart) < kConsoleWaitMs) {
        // Wait briefly for a bench console, but never block a real boot on it.
    }

    printBanner();

    if (!runBootSelfCheck()) {
        Serial.print("SELF-CHECK FAILED: ");
        Serial.print(safetyFaultName(safety::latchedFault));
        Serial.print(" (");
        Serial.print(safety::latchedDetail);
        Serial.println(")");
        Serial.println("Machine is LATCHED SAFE. Outputs stay off until the fault is cleared.");
    }
    else {
        Serial.println("SELF-CHECK PASSED");
    }

    // Started last, so a slow but legitimate boot cannot trip it, and before the
    // first loop() so the running machine is always covered.
    safety::noteControlLoopAlive();
    safetyWatchdogInit();

    Serial.println("Watchdog armed. Entering main loop.");
}

void loop() {
    // Tell the safety layer the control loop is alive and its data is fresh.
    // The heater drive (phase 3) refuses to energize if this goes stale, so a
    // hung loop drops the heater rather than leaving the SSR commanded on.
    safety::noteControlLoopAlive();

    // --- Phase 2 onwards, in roughly this order: -------------------------
    // loopWaterLevel();     // dry-fire interlock, gates the heater
    // loopTempSensors();    // both MAX31865 RTDs, fault flags checked
    // loopPressure();       // brew pressure
    // loopFlow();           // DM60 pulse counter -> volume
    // loopScale();          // HX711, plus BT weight arriving over the link
    // loopEncoder();        // Duppa gestures + RGB machine status
    // loopButtons();        // PCF8574 dose buttons
    // handleMachineState(); // brew / steam / hot water / standby
    // loopPid();            // heater PID
    // loopPump();           // PWM, closed loop on pressure + flow
    // loopComms();          // telemetry out, commands in, non-blocking

    if (safety::isFaulted()) {
        // Belt and braces. The fault path already forced everything off; this
        // re-asserts it every pass so no later code can quietly undo it.
        safetyHardOff();
    }

    // Phase 1 heartbeat so it is obvious on the bench that the loop is running.
    // Goes away once there is real telemetry.
    if (const uint32_t now = millis(); now - lastHeartbeatMs >= kHeartbeatIntervalMs) {
        lastHeartbeatMs = now;

        Serial.print("alive, uptime ");
        Serial.print(now / 1000);
        Serial.print("s, state ");
        Serial.println(safety::isFaulted() ? safetyFaultName(safety::latchedFault) : "safe-idle");
    }

    // Last statement in the loop on purpose: everything above has to have had a
    // chance to run for this pass to count as healthy.
    safetyWatchdogFeed();
}
