/**
 * @file heaterHandler.cpp
 *
 * @brief Boiler PID and the heater SSR drive
 */

#include "heaterHandler.h"

#include <Arduino.h>
#include <PID_v1.h>

#include "hardware/outputs.h"
#include "hardware/pinmapping.h"
#include "pumpHandler.h"
#include "safetyHandler.h"
#include "tempHandler.h"

namespace heater {

    namespace {

        // --- PID state. The library takes pointers, so these must outlive it.
        double pidInput = 0.0;
        double pidOutput = 0.0;
        double pidSetpoint = kDefaultSetpointC;

        double tuneKp = kDefaultKp;
        double tuneTn = kDefaultTn;
        double tuneTv = kDefaultTv;

        double kiFromTn() {
            return (tuneTn == 0.0) ? 0.0 : (tuneKp / tuneTn);
        }

        double kdFromTv() {
            return tuneKp * tuneTv;
        }

        PID bPID(&pidInput, &pidOutput, &pidSetpoint, kDefaultKp, kDefaultKp / kDefaultTn, kDefaultKp * kDefaultTv, 1, DIRECT);

        IntervalTimer windowTimer;

        /**
         * The permit, and the stamp that makes it perishable.
         *
         * Both written by the main loop and read by the ISR. Volatile because
         * of that, and 32-bit so each is read atomically on this core - the ISR
         * can never see half of an update.
         */
        volatile bool permitGranted = false;
        volatile uint32_t permitStampMs = 0;

        /** Duty the ISR compares the tick counter against, 0-1000. */
        volatile uint32_t isrDutyPerMille = 0;

        volatile uint32_t isrTicks = 0;
        volatile uint32_t isrOnTicks = 0;
        volatile uint32_t isrCounter = 0;

        bool masterEnable = false;
        bool benchOverrideOn = false;
        const char* gateReason = "not enabled";

        /**
         * @brief Drive the heater pin directly. ISR-safe.
         *
         * Raw pin writes rather than the Relay object, for the same reason
         * safetyHardOff() uses them: this runs in interrupt context and must
         * not depend on any other part of the firmware being in a good state.
         * The channel is HIGH_TRIGGER through the ULN2003, so HIGH = conducting.
         */
        inline void driveHeater(const bool on) {
            digitalWriteFast(PIN_HEATER, on ? HIGH : LOW);
        }

        /**
         * @brief Window timer tick, every kTickMs.
         *
         * Deliberately tiny and free of anything that could block. It does not
         * evaluate the safety gates - that would mean calling into half the
         * firmware from an interrupt - but it does not blindly trust them
         * either: a permit older than kPermitTimeoutMs is treated as absent, so
         * a main loop that has stopped refreshing it drops the heater without
         * the ISR needing to know why.
         */
        void onTick() {
            ++isrTicks;

            const uint32_t now = millis();
            const bool permitFresh = (now - permitStampMs) <= kPermitTimeoutMs;

            if (!permitGranted || !permitFresh) {
                driveHeater(false);
                isrCounter = 0;

                return;
            }

            // Upstream's comparison, unchanged: the counter walks the window in
            // kTickMs steps and the output is scaled to the same units.
            const bool on = (isrDutyPerMille > isrCounter);

            driveHeater(on);

            if (on) {
                ++isrOnTicks;
            }

            isrCounter += kTickMs;

            if (isrCounter >= kWindowMs) {
                isrCounter = 0;
            }
        }

        /**
         * @brief Evaluate every gate. Main-loop context, so it may do real work.
         *
         * Order matters only for the message: the first failure is what gets
         * reported, so the most fundamental reasons are checked first.
         */
        bool evaluateGates() {
            if (!masterEnable) {
                gateReason = "not enabled ('heater on' to enable)";

                return false;
            }

            if (safety::isFaulted()) {
                gateReason = "fault latched";

                return false;
            }

            if (!safety::boilerSensorHealthy) {
                gateReason = "boiler sensor not healthy";

                return false;
            }

            if (!temps::boilerHealthy()) {
                gateReason = "boiler RTD faulted or stale";

                return false;
            }

            // Belt and braces against the over-temperature limit. tempHandler
            // already latches a fault on this, which gate 2 would then catch -
            // but the heater should not depend on another module having
            // reacted first.
            if (temps::boilerC() >= temps::kBoilerOverTempC) {
                gateReason = "boiler over temperature";

                return false;
            }

            // DRY-FIRE INTERLOCK. There is no water-level probe circuit yet, so
            // the honest answer to "is there water?" is "unknown", and unknown
            // must mean no. See the header - do not default this to true.
            if (!benchOverrideOn) {
                gateReason = "no water-level probe (dry-fire interlock)";

                return false;
            }

            // Load shed. The pump handler enforces the same rule from its side;
            // both directions are checked so whichever starts second loses.
            if (pump::isRunning() && pump::loadShed()) {
                gateReason = "pump running (load shed)";

                return false;
            }

            gateReason = "ok";

            return true;
        }

    } // namespace

    void init() {
        // The pin is already safe - safetyInitOutputs() ran first - but make
        // the ownership explicit before the timer can touch it.
        pinMode(PIN_HEATER, OUTPUT);
        digitalWriteFast(PIN_HEATER, LOW);

        permitGranted = false;
        permitStampMs = 0;
        isrDutyPerMille = 0;
        isrCounter = 0;

        // Upstream's configuration, unchanged. Output is scaled to the window
        // in milliseconds so the ISR can compare it straight against its tick
        // counter without converting anything in interrupt context.
        bPID.SetSampleTime(static_cast<int>(kWindowMs));
        bPID.SetOutputLimits(0, static_cast<double>(kWindowMs));
        bPID.SetIntegratorLimits(0, kDefaultIntegratorMax);
        bPID.SetSmoothingFactor(kDefaultEmaFactor);
        bPID.SetTunings(tuneKp, kiFromTn(), kdFromTv());
        bPID.SetMode(AUTOMATIC);

        windowTimer.begin(onTick, kTickMs * 1000);

        Serial.print("heater: PID ready, setpoint ");
        Serial.print(pidSetpoint, 1);
        Serial.print(" C, window ");
        Serial.print(kWindowMs);
        Serial.print(" ms / ");
        Serial.print(kTickMs);
        Serial.print(" ms tick, OUTPUT DISABLED (");
        Serial.print(gateReason);
        Serial.println(")");
    }

    void loop() {
        // Feed the PID the real temperature regardless of whether the output
        // is allowed to do anything. Running it continuously means the duty it
        // reports is meaningful the instant heating is permitted, and it keeps
        // the whole path exercised while the element is disconnected.
        pidInput = temps::boilerC();
        bPID.Compute();

        const bool allowed = evaluateGates();

        // Publish to the ISR. The duty goes out first so the ISR can never see
        // a fresh permit paired with a stale duty.
        isrDutyPerMille = allowed ? static_cast<uint32_t>(pidOutput) : 0;
        permitGranted = allowed;
        permitStampMs = millis();

        // Nothing re-asserts the output here. If this function stops being
        // called the stamp goes stale and the ISR drops the heater on its own,
        // which is the entire point of the expiry.
    }

    bool setEnabled(const bool on) {
        if (!on) {
            masterEnable = false;
            permitGranted = false;
            isrDutyPerMille = 0;
            driveHeater(false);

            Serial.println("heater: DISABLED");

            return true;
        }

        if (safety::isFaulted()) {
            gateReason = "fault latched";

            Serial.println("heater: REFUSED - clear the latched fault first");

            return false;
        }

        masterEnable = true;

        // Re-evaluate before reporting. gateReason is otherwise a pass behind,
        // so enabling the heater printed "blocked - not enabled" immediately
        // after enabling it. A status line that contradicts the thing you just
        // did teaches people to ignore status lines.
        evaluateGates();

        Serial.println("heater: master enable ON - the other gates still apply");
        printStatus();

        return true;
    }

    bool enabled() {
        return masterEnable;
    }

    void setBenchOverride(const bool on) {
        benchOverrideOn = on;

        if (on) {
            Serial.println();
            Serial.println("*** DRY-FIRE INTERLOCK OVERRIDDEN ***");
            Serial.println("The water-level probe is bypassed. This is for confirming the SSR");
            Serial.println("drive with NO HEATING ELEMENT CONNECTED. Do not leave it set, and");
            Serial.println("do not energise an element in a boiler whose water level is unknown.");
            Serial.println("Not persisted - a reboot clears it.");
            Serial.println();
        }
        else {
            Serial.println("heater: dry-fire interlock restored");
        }
    }

    bool benchOverride() {
        return benchOverrideOn;
    }

    double dutyPercent() {
        return (pidOutput * 100.0) / static_cast<double>(kWindowMs);
    }

    double rawOutput() {
        return pidOutput;
    }

    double setpointC() {
        return pidSetpoint;
    }

    void setSetpointC(const double celsius) {
        // Clamped well under the over-temperature limit. A setpoint above it
        // would mean the PID asking for heat the gates must then refuse -
        // the machine fighting itself, and confusing to debug.
        const double ceiling = temps::kBoilerOverTempC - 5.0;

        if (celsius < 20.0 || celsius > ceiling) {
            Serial.print("heater: REFUSED - setpoint must be 20 to ");
            Serial.print(ceiling, 1);
            Serial.println(" C");

            return;
        }

        pidSetpoint = celsius;

        Serial.print("heater: setpoint now ");
        Serial.print(pidSetpoint, 1);
        Serial.println(" C");
    }

    void setTunings(const double kp, const double tn, const double tv) {
        tuneKp = kp;
        tuneTn = tn;
        tuneTv = tv;

        bPID.SetTunings(tuneKp, kiFromTn(), kdFromTv());

        Serial.print("heater: tunings Kp ");
        Serial.print(tuneKp, 2);
        Serial.print(" Tn ");
        Serial.print(tuneTn, 2);
        Serial.print(" Tv ");
        Serial.print(tuneTv, 2);
        Serial.print("  (Ki ");
        Serial.print(kiFromTn(), 4);
        Serial.print(" Kd ");
        Serial.print(kdFromTv(), 2);
        Serial.println(")");
    }

    void tunings(double* kp, double* tn, double* tv) {
        *kp = tuneKp;
        *tn = tuneTn;
        *tv = tuneTv;
    }

    bool permitted() {
        return permitGranted;
    }

    const char* blockedBy() {
        return gateReason;
    }

    uint32_t tickCount() {
        return isrTicks;
    }

    uint32_t onTickCount() {
        return isrOnTicks;
    }

    void testPermitExpiry() {
        if (!permitGranted) {
            Serial.println("heater: not currently permitted - nothing to expire. Enable it first.");

            return;
        }

        const uint32_t onBefore = isrOnTicks;
        const uint32_t ticksBefore = isrTicks;

        Serial.print("heater: stalling the loop for ");
        Serial.print(kPermitTimeoutMs + 300);
        Serial.println(" ms WITHOUT refreshing the permit...");
        Serial.flush();

        // Spin, exactly as a hung loop would. Not delay() - this must look
        // like the loop stopped running, and the ISR must keep firing.
        const uint32_t began = millis();

        while (millis() - began < (kPermitTimeoutMs + 300)) {
            // Deliberately empty. The watchdog is fed in loop(), which is not
            // running - the stall is well inside its 4 s bite.
        }

        const uint32_t onAfter = isrOnTicks;
        const uint32_t ticksAfter = isrTicks;
        const uint32_t elapsedTicks = ticksAfter - ticksBefore;
        const uint32_t energisedTicks = onAfter - onBefore;

        Serial.print("  ISR ran ");
        Serial.print(elapsedTicks);
        Serial.print(" ticks during the stall and energised for ");
        Serial.print(energisedTicks);
        Serial.println(" of them.");

        // The permit was fresh for the first kPermitTimeoutMs, so some
        // energised ticks at the start are correct. What matters is that it
        // stopped well before the end.
        const uint32_t expectedMax = (kPermitTimeoutMs / kTickMs) + 5;

        if (energisedTicks <= expectedMax) {
            Serial.print("  PASS - stopped after about ");
            Serial.print(energisedTicks * kTickMs);
            Serial.print(" ms, within the ");
            Serial.print(kPermitTimeoutMs);
            Serial.println(" ms permit timeout.");
        }
        else {
            Serial.println("  *** FAIL - the ISR kept energising on a stale permit. ***");
            Serial.println("  This is the protection against a hung control loop. Do not");
            Serial.println("  connect a heating element until this passes.");
        }

        driveHeater(false);
    }

    void printStatus() {
        Serial.print("heater: ");
        Serial.print(masterEnable ? "ENABLED" : "disabled");
        Serial.print(", setpoint ");
        Serial.print(pidSetpoint, 1);
        Serial.print(" C, boiler ");

        if (temps::boilerHealthy()) {
            Serial.print(temps::boilerC(), 1);
            Serial.print(" C");
        }
        else {
            Serial.print("FAULT");
        }

        Serial.print(", PID asks ");
        Serial.print(dutyPercent(), 1);
        Serial.println("%");

        Serial.print("        gates: ");
        Serial.print(permitGranted ? "PERMITTED" : "blocked");
        Serial.print(" - ");
        Serial.println(gateReason);

        Serial.print("        timer: ");
        Serial.print(isrTicks);
        Serial.print(" ticks, ");
        Serial.print(isrOnTicks);
        Serial.print(" energised, Kp ");
        Serial.print(tuneKp, 1);
        Serial.print(" Tn ");
        Serial.print(tuneTn, 1);
        Serial.print(" Tv ");
        Serial.println(tuneTv, 1);

        if (benchOverrideOn) {
            Serial.println("        *** dry-fire interlock OVERRIDDEN (bench) ***");
        }
    }

} // namespace heater
