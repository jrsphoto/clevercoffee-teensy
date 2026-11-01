/**
 * @file weightHandler.cpp
 *
 * @brief HX711 load cell: brew-by-weight
 */

#include "weightHandler.h"

#include <Arduino.h>

#include "hardware/pinmapping.h"
#include "hardware/scales/HX711Scale.h"

namespace weight {

    namespace {

        /**
         * Upstream's HX711Scale, used unchanged. Only the start-up sequence is
         * driven differently here - see the header.
         */
        HX711Scale* scale = nullptr;

        /**
         * The underlying library object, needed directly because the
         * non-blocking start calls startMultiple() itself rather than going
         * through HX711Scale::init(), which blocks.
         */
        HX711_ADC* cell = nullptr;

        enum class State : uint8_t {
            NotCreated,  // init() has not run, or failed to allocate
            Stabilising, // conversions running, waiting to settle and tare
            Running,     // usable
            Failed,      // no HX711 responding, or start-up timed out
        };

        State state = State::NotCreated;

        /** Why we are in State::Failed, for the console. */
        const char* failureReason = "";

        uint32_t startBeganMs = 0;
        uint32_t lastGoodReadingMs = 0;
        bool haveEverRead = false;

        float lastWeight = 0.0f;
        int samplesInUse = kDefaultSamples;

        /**
         * Calibration factor. 1.0 means raw counts, which is useful for
         * bring-up - a visibly silly number is better than a plausible wrong
         * one, because a plausible wrong weight would silently mis-dose a shot.
         */
        float calibration = 1.0f;

        bool tareRunning = false;

        /**
         * How long to let the chip settle before believing a signal timeout.
         * The flag can trip while the HX711 is still powering up, which is not
         * a fault; after a second it means nothing is answering.
         */
        constexpr uint32_t kSignalGraceMs = 1000;

        void enterFailed(const char* reason) {
            state = State::Failed;
            failureReason = reason;

            Serial.print("scale: FAILED - ");
            Serial.println(reason);
            Serial.println("       Brew-by-weight must not be used. 'scalerestart' to retry.");
        }

    } // namespace

    bool init() {
        delete scale;

        // Single load cell. HX711Scale also supports two cells sharing a clock
        // (PIN_HXDAT2), which is how some machines weigh the cup and the drip
        // tray separately; add the dual constructor here if that is ever fitted.
        scale = new HX711Scale(PIN_HXDAT, PIN_HXCLK, calibration);

        if (scale == nullptr) {
            state = State::NotCreated;

            return false;
        }

        cell = scale->getLoadCell(1);

        if (cell == nullptr) {
            state = State::NotCreated;

            return false;
        }

        cell->begin();

        // The data line idles high when nothing is driving it, which is how an
        // absent HX711 is told apart from one holding the line low to say a
        // conversion is ready.
        pinMode(PIN_HXDAT, INPUT_PULLUP);

        // Disable the library's tare timeout and bound the attempt here
        // instead. Its deadline is a function-level static, computed once per
        // program run, so a second attempt would report an instant timeout.
        cell->disableTareTimeout();

        cell->setSamplesInUse(samplesInUse);

        startBeganMs = millis();
        haveEverRead = false;
        tareRunning = false;
        state = State::Stabilising;

        Serial.print("scale: HX711 on DAT ");
        Serial.print(PIN_HXDAT);
        Serial.print(" / CLK ");
        Serial.print(PIN_HXCLK);
        Serial.print(", stabilising for ");
        Serial.print(kStabiliseMs / 1000);
        Serial.println("s then taring (machine runs normally meanwhile)");

        return true;
    }

    void loop() {
        if (scale == nullptr || cell == nullptr) {
            return;
        }

        switch (state) {
            case State::Stabilising: {
                // Non-blocking equivalent of upstream's `while (!startMultiple
                // (...))`: called once per pass until it reports done.
                const int done = cell->startMultiple(kStabiliseMs, true);

                if (done != 0) {
                    if (cell->getSignalTimeoutFlag()) {
                        enterFailed("no response from the HX711 (check DAT/CLK and power)");

                        return;
                    }

                    cell->setCalFactor(calibration);
                    cell->setSamplesInUse(samplesInUse);

                    state = State::Running;
                    lastGoodReadingMs = millis();

                    Serial.println("scale: stabilised and tared - weight is now valid");

                    return;
                }

                const uint32_t elapsed = millis() - startBeganMs;

                // Fail fast on an absent chip. With the data line pulled up and
                // nothing driving it, a conversion is never ready and the
                // library raises its signal timeout - so there is no need to sit
                // through the full stabilisation to find out. The grace period
                // is because the flag can also trip transiently while the HX711
                // itself is powering up, which is not a failure.
                if (elapsed > kSignalGraceMs && cell->getSignalTimeoutFlag()) {
                    enterFailed("no response from the HX711 (check DAT/CLK, VCC and GND)");

                    return;
                }

                // Backstop. The library's own bound would not fire, having been
                // disabled above on purpose.
                if (elapsed > kStartTimeoutMs) {
                    enterFailed("start-up timed out - is an HX711 actually connected?");
                }

                break;
            }

            case State::Running: {
                if (scale->update()) {
                    lastWeight = scale->getWeight();
                    lastGoodReadingMs = millis();
                    haveEverRead = true;
                }

                // A signal timeout while running means the HX711 stopped
                // answering - unplugged mid-shot, most likely. Report it rather
                // than keep serving the last number, which would look exactly
                // like a working scale to anything downstream.
                if (cell->getSignalTimeoutFlag()) {
                    enterFailed("HX711 stopped responding while running");

                    return;
                }

                if (tareRunning && cell->getTareStatus()) {
                    tareRunning = false;

                    Serial.println("scale: tare complete");
                }

                break;
            }

            case State::NotCreated:
            case State::Failed:
                break;
        }
    }

    bool present() {
        return state == State::Stabilising || state == State::Running;
    }

    bool ready() {
        return state == State::Running;
    }

    bool healthy() {
        // Staleness is checked against the last SUCCESSFUL conversion, not
        // against the last time this was called, so a scale that has quietly
        // stopped converting cannot keep refreshing its own freshness.
        return state == State::Running && haveEverRead && !tareRunning && msSinceLastReading() <= kStaleAfterMs;
    }

    float grams() {
        return lastWeight;
    }

    uint32_t msSinceLastReading() {
        if (!haveEverRead) {
            // Never read: report an age that is unambiguously stale rather
            // than zero, which would read as perfectly fresh.
            return kStaleAfterMs + 1;
        }

        return millis() - lastGoodReadingMs;
    }

    void startTare() {
        if (state != State::Running) {
            Serial.println("scale: not ready - cannot tare yet");

            return;
        }

        // The non-blocking tare. The blocking tare() would stall the loop for
        // the length of the whole dataset, which at 10 SPS is seconds.
        cell->tareNoDelay();
        tareRunning = true;

        Serial.println("scale: taring...");
    }

    bool tareInProgress() {
        return tareRunning;
    }

    bool calibrateTo(const float knownGrams) {
        if (state != State::Running) {
            Serial.println("scale: not ready - cannot calibrate yet");

            return false;
        }

        if (tareRunning) {
            Serial.println("scale: tare still running - wait for it to finish");

            return false;
        }

        // A tiny mass gives a calibration factor dominated by noise and drift,
        // and a negative one means the cell is wired backwards or was tared
        // with the mass already on it. Either way, refuse rather than store a
        // number that would quietly mis-dose every shot afterwards.
        if (knownGrams < 10.0f) {
            Serial.println("scale: REFUSED - use a known mass of at least 10 g");

            return false;
        }

        calibration = cell->getNewCalibration(knownGrams);

        Serial.print("scale: calibrated to ");
        Serial.print(knownGrams, 2);
        Serial.print(" g, factor is now ");
        Serial.println(calibration, 4);

        return true;
    }

    float calFactor() {
        return calibration;
    }

    void setCalFactor(const float factor) {
        calibration = factor;

        if (cell != nullptr) {
            cell->setCalFactor(factor);
        }
    }

    void setSamples(const int newSamples) {
        samplesInUse = newSamples;

        if (scale != nullptr) {
            scale->setSamples(newSamples);
        }
    }

    int samples() {
        return samplesInUse;
    }

    void restart() {
        Serial.println("scale: restarting start-up sequence");
        init();
    }

    void printStatus() {
        Serial.print("scale: ");

        switch (state) {
            case State::NotCreated:
                Serial.println("not initialised");

                return;

            case State::Failed:
                Serial.print("FAILED - ");
                Serial.println(failureReason);
                Serial.println("       brew-by-weight unusable; 'scalerestart' to retry");

                return;

            case State::Stabilising:
                Serial.print("stabilising, ");
                Serial.print((millis() - startBeganMs) / 1000);
                Serial.print("s of ");
                Serial.print(kStabiliseMs / 1000);
                Serial.println("s");

                return;

            case State::Running:
                break;
        }

        Serial.print(lastWeight, 2);
        Serial.print(" g (factor ");
        Serial.print(calibration, 4);

        if (calibration == 1.0f) {
            Serial.print(", UNCALIBRATED - this is raw counts, not grams");
        }

        Serial.print("), ");
        Serial.print(samplesInUse);
        Serial.print(" samples, last reading ");
        Serial.print(msSinceLastReading());
        Serial.print(" ms ago, ");
        Serial.println(healthy() ? "healthy" : "NOT HEALTHY - weight must not be used");

        if (tareRunning) {
            Serial.println("scale: tare in progress");
        }
    }

} // namespace weight
