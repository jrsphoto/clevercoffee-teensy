/**
 * @file pumpHandler.cpp
 *
 * @brief Variable-speed pump, hardware PWM on Teensy pin 9 (FlexPWM2_2_B)
 */

#include "pumpHandler.h"

#include <Arduino.h>

#include "hardware/pinmapping.h"
#include "safetyHandler.h"

namespace pump {

    namespace {

        /** Full-scale duty for the configured resolution. */
        constexpr uint32_t kDutyMax = (1u << kPwmResolutionBits) - 1u;

        /** Requested speed, 0-100. What the caller asked for. */
        float requested = 0.0f;

        /** Applied speed, 0-100. Follows `requested` at the ramp rate. */
        float applied = 0.0f;

        uint32_t lastCommandMs = 0;
        uint32_t lastRampMs = 0;

        bool initialised = false;

        const char* refusal = "ok";

        /** So the expiry is reported once rather than every pass. */
        bool expiryReported = false;

        /**
         * @brief Put the duty on the pin.
         *
         * Zero is written as a plain GPIO low rather than a 0% PWM. A 0% duty
         * should be a steady low, but "should" is not good enough for the pin
         * that runs the pump: taking the pad back to GPIO makes off mean off at
         * the hardware level, independent of the PWM peripheral's state.
         */
        void writeDuty(const float percent) {
            if (percent <= 0.0f) {
                pinMode(PIN_PUMP, OUTPUT);
                digitalWriteFast(PIN_PUMP, LOW);

                return;
            }

            const float clamped = percent > 100.0f ? 100.0f : percent;
            const auto duty = static_cast<uint32_t>((clamped / 100.0f) * static_cast<float>(kDutyMax) + 0.5f);

            analogWrite(PIN_PUMP, static_cast<int>(duty > kDutyMax ? kDutyMax : duty));
        }

    } // namespace

    void init() {
        // Off first, and by the same route safetyHardOff() uses.
        pinMode(PIN_PUMP, OUTPUT);
        digitalWriteFast(PIN_PUMP, LOW);

        // Configure the PWM peripheral but do NOT mux the pin to it yet - that
        // happens on the first non-zero duty. Setting frequency and resolution
        // here is harmless and keeps the first real command cheap.
        analogWriteResolution(kPwmResolutionBits);
        analogWriteFrequency(PIN_PUMP, static_cast<float>(kPwmFrequencyHz));

        requested = 0.0f;
        applied = 0.0f;
        lastCommandMs = 0;
        lastRampMs = millis();
        initialised = true;
        refusal = "ok";

        Serial.print("pump: PWM on pin ");
        Serial.print(PIN_PUMP);
        Serial.print(" at ");
        Serial.print(kPwmFrequencyHz);
        Serial.print(" Hz, ");
        Serial.print(kPwmResolutionBits);
        Serial.println("-bit, stopped");
    }

    bool setSpeed(const float percent) {
        if (!initialised) {
            refusal = "pump not initialised";

            return false;
        }

        // Zero is always allowed, and must be: a stop request can never be
        // refused, including while faulted.
        if (percent <= 0.0f) {
            requested = 0.0f;
            lastCommandMs = millis();
            refusal = "ok";

            return true;
        }

        if (safety::isFaulted()) {
            refusal = "fault latched";

            return false;
        }

        requested = percent > 100.0f ? 100.0f : percent;
        lastCommandMs = millis();
        expiryReported = false;
        refusal = "ok";

        return true;
    }

    void stop() {
        // Immediate, not ramped. A ramp down is right for a shot ending
        // gracefully; it is wrong for everything that calls stop(), which is
        // faults, expiry and a person asking for it to stop now.
        requested = 0.0f;
        applied = 0.0f;

        writeDuty(0.0f);
    }

    void loop() {
        if (!initialised) {
            return;
        }

        const uint32_t now = millis();

        // A latched fault stops the pump regardless of what was asked for, and
        // without waiting for the ramp.
        if (safety::isFaulted() && (requested > 0.0f || applied > 0.0f)) {
            stop();

            Serial.println("pump: stopped - fault latched");

            return;
        }

        // Command expiry. The speed must be continuously re-asserted; a caller
        // that stops asking gets a stopped pump, not an abandoned running one.
        if (requested > 0.0f && (now - lastCommandMs) > kCommandTimeoutMs) {
            stop();

            if (!expiryReported) {
                expiryReported = true;

                Serial.print("pump: stopped - no speed command for ");
                Serial.print(kCommandTimeoutMs);
                Serial.println(" ms (command expired)");
            }

            return;
        }

        // Ramp toward the request.
        const uint32_t elapsed = now - lastRampMs;

        if (elapsed == 0) {
            return;
        }

        lastRampMs = now;

        if (applied != requested) {
            const float maxStep = kMaxRampPercentPerSec * (static_cast<float>(elapsed) / 1000.0f);
            const float error = requested - applied;

            if (fabsf(error) <= maxStep) {
                applied = requested;
            }
            else {
                applied += (error > 0.0f) ? maxStep : -maxStep;
            }

            writeDuty(applied);
        }
    }

    float requestedSpeed() {
        return requested;
    }

    float actualSpeed() {
        return applied;
    }

    bool isRunning() {
        return applied > 0.0f;
    }

    const char* lastRefusal() {
        return refusal;
    }

    void printStatus() {
        Serial.print("pump: ");

        if (!initialised) {
            Serial.println("not initialised");

            return;
        }

        Serial.print(applied, 1);
        Serial.print("% applied");

        if (applied != requested) {
            Serial.print(" (ramping to ");
            Serial.print(requested, 1);
            Serial.print("%)");
        }

        Serial.print(", ");
        Serial.print(kPwmFrequencyHz);
        Serial.print(" Hz on pin ");
        Serial.print(PIN_PUMP);

        if (requested > 0.0f) {
            Serial.print(", command age ");
            Serial.print(millis() - lastCommandMs);
            Serial.print("/");
            Serial.print(kCommandTimeoutMs);
            Serial.print(" ms");
        }

        Serial.println();

        if (strcmp(refusal, "ok") != 0) {
            Serial.print("pump: last request REFUSED - ");
            Serial.println(refusal);
        }
    }

} // namespace pump
