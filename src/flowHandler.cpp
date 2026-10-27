/**
 * @file flowHandler.cpp
 *
 * @brief Digmesa DM60 Nano flow sensor: pulse counting, volume and flow rate
 */

#include "flowHandler.h"

#include <Arduino.h>

#include "hardware/pinmapping.h"

namespace flow {

    namespace {

        /**
         * Written by the interrupt, read by the main loop. Everything the ISR
         * touches is volatile, and everything else is derived outside it - the
         * interrupt does the least work that is correct and nothing more.
         */
        volatile uint32_t pulses = 0;
        volatile uint32_t rejected = 0;
        volatile uint32_t lastPulseUs = 0;
        volatile uint32_t lastPulseMs = 0;

        uint32_t calibrationPulsesPerLitre = kDefaultPulsesPerLitre;

        // Rate derivation, main-loop side only.
        uint32_t lastRateCalcMs = 0;
        uint32_t lastRatePulses = 0;
        double currentRateMlPerSec = 0.0;

        /**
         * @brief Pulse interrupt.
         *
         * Falling edge: the sensor is open-collector, so it pulls the line down
         * once per unit volume.
         *
         * Deliberately tiny. No arithmetic, no floating point, no logging - just
         * a reject test and two counters. Volume and rate are derived in the main
         * loop where taking time is harmless.
         */
        void onPulse() {
            const uint32_t now = micros();

            // Reject implausibly close edges. Genuine pulses cannot arrive this
            // fast at any flow this pump can produce, so anything closer is
            // noise or bounce.
            if ((now - lastPulseUs) < kMinPulseIntervalUs) {
                rejected++;

                return;
            }

            lastPulseUs = now;
            lastPulseMs = millis();
            pulses++;
        }

        /**
         * @brief Read the pulse counter without tearing.
         *
         * A 32-bit read is a single instruction on this core, so it cannot tear
         * on its own - but reading several ISR-written values that must agree
         * with each other does need the interrupt held off briefly.
         */
        uint32_t snapshotPulses() {
            noInterrupts();
            const uint32_t value = pulses;
            interrupts();

            return value;
        }

        double mlFromPulses(const uint32_t count) {
            if (calibrationPulsesPerLitre == 0) {
                return 0.0;
            }

            return (static_cast<double>(count) * 1000.0) / static_cast<double>(calibrationPulsesPerLitre);
        }

    } // namespace

    void init() {
        // INPUT_PULLUP covers both cases: the real board carries an external
        // pull-up to 3.3 V for the open-collector sensor, and on a bare Teensy
        // the internal one is enough for bench work or for a signal generator
        // driving the pin actively.
        pinMode(PIN_FLOW, INPUT_PULLUP);

        attachInterrupt(digitalPinToInterrupt(PIN_FLOW), onPulse, FALLING);

        reset();
    }

    void reset() {
        noInterrupts();
        pulses = 0;
        rejected = 0;
        interrupts();

        lastRatePulses = 0;
        lastRateCalcMs = millis();
        currentRateMlPerSec = 0.0;
    }

    void loop() {
        const uint32_t now = millis();

        // Flow has stopped if nothing has arrived for a while. Checked before
        // the window below so the rate drops promptly at the end of a shot
        // rather than decaying over a whole window.
        if ((now - lastPulseMs) > kFlowTimeoutMs) {
            currentRateMlPerSec = 0.0;
            lastRatePulses = snapshotPulses();
            lastRateCalcMs = now;

            return;
        }

        if ((now - lastRateCalcMs) < kRateWindowMs) {
            return;
        }

        const uint32_t nowPulses = snapshotPulses();
        const uint32_t deltaPulses = nowPulses - lastRatePulses;
        const uint32_t deltaMs = now - lastRateCalcMs;

        if (deltaMs > 0) {
            currentRateMlPerSec = (mlFromPulses(deltaPulses) * 1000.0) / static_cast<double>(deltaMs);
        }

        lastRatePulses = nowPulses;
        lastRateCalcMs = now;
    }

    double totalMl() {
        return mlFromPulses(snapshotPulses());
    }

    double rateMlPerSec() {
        return currentRateMlPerSec;
    }

    uint32_t pulseCount() {
        return snapshotPulses();
    }

    uint32_t rejectedCount() {
        noInterrupts();
        const uint32_t value = rejected;
        interrupts();

        return value;
    }

    bool isFlowing() {
        return (millis() - lastPulseMs) <= kFlowTimeoutMs;
    }

    uint32_t pulsesPerLitre() {
        return calibrationPulsesPerLitre;
    }

    void setPulsesPerLitre(const uint32_t pulsesPerL) {
        if (pulsesPerL > 0) {
            calibrationPulsesPerLitre = pulsesPerL;
        }
    }

    bool calibrateTo(const double actualMl) {
        const uint32_t counted = snapshotPulses();

        // Too little to calibrate against. A handful of pulses would produce a
        // confident-looking number with a huge error, which is worse than
        // refusing - so refuse.
        if (counted < 100 || actualMl <= 0.0) {
            return false;
        }

        calibrationPulsesPerLitre = static_cast<uint32_t>((static_cast<double>(counted) * 1000.0) / actualMl);

        return true;
    }

    void printStatus() {
        const uint32_t counted = snapshotPulses();

        Serial.print("  flow (pin ");
        Serial.print(PIN_FLOW);
        Serial.print(") : ");
        Serial.print(totalMl(), 2);
        Serial.print(" ml   rate ");
        Serial.print(currentRateMlPerSec, 2);
        Serial.print(" ml/s   ");
        Serial.println(isFlowing() ? "FLOWING" : "idle");

        Serial.print("    pulses=");
        Serial.print(counted);
        Serial.print("  rejected=");
        Serial.print(rejectedCount());
        Serial.print("  calibration=");
        Serial.print(calibrationPulsesPerLitre);
        Serial.print(" pulses/litre");

        if (calibrationPulsesPerLitre == kDefaultPulsesPerLitre) {
            Serial.println("  (DEFAULT - not yet calibrated)");
        }
        else {
            Serial.println();
        }
    }

} // namespace flow
