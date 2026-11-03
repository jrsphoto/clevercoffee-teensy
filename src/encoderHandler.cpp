/**
 * @file encoderHandler.cpp
 *
 * @brief DuPPa i2cEncoder V2: rotation, button gestures, and the RGB status LED
 */

#include "encoderHandler.h"

#include <Arduino.h>
#include <Wire.h>
#include <i2cEncoderLibV2.h>

#include "hardware/pinmapping.h"
#include "heaterHandler.h"
#include "safetyHandler.h"
#include "shotHandler.h"
#include "tempHandler.h"

namespace encoder {

    namespace {

        i2cEncoderLibV2* dev = nullptr;
        bool present = false;

        int32_t lastCounter = 0;

        /**
         * Button gesture tracking. The DuPPa reports press, release and double
         * press; long press and the single/double distinction are derived here.
         */
        constexpr uint32_t kLongPressMs = 600;
        constexpr uint32_t kDoublePressWindowMs = 550; // matches writeDoublePushPeriod(50)

        uint32_t pressedAtMs = 0;
        bool singlePending = false;
        uint32_t singleDeadlineMs = 0;
        Gesture pendingGesture = Gesture::None;
        int32_t pendingRotation = 0;

        // Sentinel so the first update always writes, even if the computed
        // colour happens to be 0x000000.
        uint32_t lastColor = 0xFFFFFFFF;

        /**
         * @brief Machine state -> status colour.
         *
         * Deliberately simple while the control core is still being ported. The
         * states that exist today are: faulted, heating toward setpoint, and
         * idle/ready. Brew and steam join this when those handlers land.
         */
        /**
         * Colour for the selected shot profile, shown only when nothing more
         * important needs saying.
         */
        uint32_t profileColor = kColorReady;

        uint32_t statusColor() {
            // Safety first, always. A profile colour must never be able to
            // mask a fault, so these come before anything cosmetic.
            if (safety::isFaulted()) {
                return kColorError;
            }

            if (!temps::boilerHealthy()) {
                return kColorError;
            }

            if (shot::isRunning()) {
                return kColorActive;
            }

            // Heating hard enough to be worth saying so. Below this the PID is
            // just trimming and the machine is effectively ready.
            if (heater::enabled() && heater::dutyPercent() > 10.0) {
                return kColorHeating;
            }

            // Nothing to report, so show which profile is selected.
            return profileColor;
        }

        void updateStatusColor() {
            if (!kHasRgbLed || !kStatusColorsEnabled || dev == nullptr) {
                return;
            }

            const uint32_t color = statusColor();

            // Only write on change. Pushing a colour every loop pass would
            // hammer the bus for no reason and crowd out real traffic.
            if (color != lastColor) {
                dev->writeRGBCode(color);
                lastColor = color;
            }
        }

    } // namespace

    bool init() {
        // Confirm something actually answers before constructing a driver around
        // it. The library will happily talk into the void otherwise, and a
        // missing encoder would look like a working one that never moves.
        Wire.beginTransmission(kAddress);

        if (kAddress > 0x07 && Wire.endTransmission() != 0) {
            present = false;

            return false;
        }

        dev = new i2cEncoderLibV2(kAddress);

        uint16_t flags = i2cEncoderLibV2::INT_DATA | i2cEncoderLibV2::WRAP_DISABLE | i2cEncoderLibV2::DIRE_RIGHT | i2cEncoderLibV2::IPUP_ENABLE | i2cEncoderLibV2::RMOD_X1;

        if (kHasRgbLed) {
            flags |= i2cEncoderLibV2::RGB_ENCODER;
        }

        dev->begin(flags);

        // Configure the counter. Without this the step size is zero and the
        // counter physically cannot move, so rotating the knob produces nothing
        // at all - the button still works, which makes it look like a wiring
        // problem rather than a configuration one.
        //
        // Wide limits because this is a free-running control input, not a bounded
        // menu selector: the handlers above consume deltas, not absolute position.
        dev->writeCounter(static_cast<int32_t>(0));
        dev->writeMax(static_cast<int32_t>(32767));
        dev->writeMin(static_cast<int32_t>(-32768));
        dev->writeStep(static_cast<int32_t>(1));

        // Debounce the pushbutton in the encoder itself (units of 10 ms).
        dev->writeDoublePushPeriod(50); // 500 ms window for a double-press
        dev->writeAntibouncingPeriod(20); // 200 ms

        if (kHasRgbLed) {
            const uint32_t boot = kStatusColorsEnabled ? kColorIdle : 0x000000;
            dev->writeRGBCode(boot);
            lastColor = boot;
        }

        lastCounter = dev->readCounterLong();
        pendingRotation = 0;
        present = true;

        return true;
    }

    void loop() {
        if (dev == nullptr) {
            return;
        }

        dev->updateStatus();

        if (dev->readStatus(i2cEncoderLibV2::RINC) || dev->readStatus(i2cEncoderLibV2::RDEC)) {
            const int32_t now = dev->readCounterLong();
            const int32_t delta = now - lastCounter;
            pendingRotation += delta;
            lastCounter = now;

            Serial.print("encoder: rotate ");
            Serial.print(delta > 0 ? "+" : "");
            Serial.print(delta);
            Serial.print("  counter=");
            Serial.println(now);
        }

        if (dev->readStatus(i2cEncoderLibV2::PUSHP)) {
            pressedAtMs = millis();
        }

        if (dev->readStatus(i2cEncoderLibV2::PUSHR)) {
            const uint32_t heldMs = millis() - pressedAtMs;

            if (heldMs >= kLongPressMs) {
                pendingGesture = Gesture::Long;
                singlePending = false;

                Serial.println("encoder: long press");
            }
            else {
                // Hold the single back until the double-press window closes.
                // The first press of a double is indistinguishable from a
                // single at this moment, so acting now would fire profile 1
                // every time somebody asked for profile 2.
                singlePending = true;
                singleDeadlineMs = millis() + kDoublePressWindowMs;
            }
        }

        if (dev->readStatus(i2cEncoderLibV2::PUSHD)) {
            // The double arrived, so the single that was waiting was its
            // first half. Discard it.
            singlePending = false;
            pendingGesture = Gesture::Double;

            Serial.println("encoder: double press");
        }

        if (singlePending && static_cast<int32_t>(millis() - singleDeadlineMs) >= 0) {
            singlePending = false;
            pendingGesture = Gesture::Short;

            Serial.println("encoder: short press");
        }

        updateStatusColor();
    }

    Gesture takeGesture() {
        const Gesture g = pendingGesture;
        pendingGesture = Gesture::None;

        return g;
    }

    bool isPresent() {
        return present;
    }

    int32_t takeRotation() {
        const int32_t value = pendingRotation;
        pendingRotation = 0;

        return value;
    }

    int32_t counter() {
        return lastCounter;
    }

    void setProfileColor(const uint32_t rgb) {
        profileColor = rgb;
    }

    void setLedColor(const uint32_t rgb) {
        if (dev == nullptr) {
            return;
        }

        dev->writeRGBCode(rgb);
        lastColor = rgb; // keep the change-detector honest
    }

    void printStatus() {
        Serial.print("  encoder (0x");
        Serial.print(kAddress, HEX);
        Serial.print(") : ");

        if (!present) {
            Serial.println("NOT FOUND");

            return;
        }

        Serial.print("counter ");
        Serial.print(lastCounter);
        Serial.print("   pending rotation ");
        Serial.print(pendingRotation);
        Serial.print("   LED 0x");
        Serial.println(lastColor, HEX);
    }

} // namespace encoder
