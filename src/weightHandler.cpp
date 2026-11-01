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

        /**
         * How long to wait for DOUT to go low before calling the chip absent.
         *
         * At the usual 10 conversions per second one is due every 100 ms, so
         * 600 ms is several conversion periods - long enough to be certain,
         * short enough to report at boot rather than after a long wait. Covers
         * a board strapped for 80 SPS too, which is faster still.
         */
        constexpr uint32_t kDoutWaitMs = 600;

        /**
         * @brief Wait for the HX711 to pull DOUT low. Blocking, bounded.
         *
         * Only called from init() and the console probe, never from loop(). The
         * HX711 holds DOUT low once a conversion is ready and nothing has
         * clocked it out, so this is a definitive presence test rather than a
         * sampled guess.
         *
         * @param wentLowAfterMs set to how long it took, when it did
         * @return true if DOUT went low within kDoutWaitMs
         */
        bool waitForDoutLow(uint32_t* wentLowAfterMs) {
            // Clock idle low. If SCK were left high the HX711 would be in
            // power-down and would never produce a conversion, which would
            // look exactly like an absent chip.
            pinMode(PIN_HXCLK, OUTPUT);
            digitalWrite(PIN_HXCLK, LOW);

            pinMode(PIN_HXDAT, INPUT_PULLUP);

            const uint32_t began = millis();

            while (millis() - began < kDoutWaitMs) {
                if (digitalRead(PIN_HXDAT) == LOW) {
                    *wentLowAfterMs = millis() - began;

                    return true;
                }
            }

            *wentLowAfterMs = kDoutWaitMs;

            return false;
        }

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

        // Check the chip is actually there BEFORE handing over to the library,
        // which has no reliable way to tell us. See waitForDoutLow().
        uint32_t doutLowAfterMs = 0;

        if (!waitForDoutLow(&doutLowAfterMs)) {
            enterFailed("DOUT never went low - no HX711 on pin 38 (check DAT, VCC, GND)");

            return true;
        }

        Serial.print("scale: DOUT went low after ");
        Serial.print(doutLowAfterMs);
        Serial.println(" ms - a chip is present and converting");

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

                // NOT checking getSignalTimeoutFlag() here, which looks like
                // the obvious thing to do and does not work: startMultiple()
                // resets the timestamp that flag is measured from on every
                // call, and this calls it every loop pass, so the timeout can
                // never elapse. Presence was established directly in init().
                if (millis() - startBeganMs > kStartTimeoutMs) {
                    enterFailed("stabilise/tare did not finish - see 'scalediag'");
                }

                break;
            }

            case State::Running: {
                if (scale->update()) {
                    lastWeight = scale->getWeight();
                    lastGoodReadingMs = millis();
                    haveEverRead = true;
                }

                // Losing the chip mid-shot is caught by staleness in healthy()
                // rather than by the library's flag, for the reason given above.
                // update() is called once per pass here, which does NOT reset
                // the flag's timestamp, but it is simpler and more honest to
                // measure the thing we actually care about: whether a fresh
                // conversion arrived recently.

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

    void probeDataLine() {
        uint32_t wentLowAfterMs = 0;
        const bool low = waitForDoutLow(&wentLowAfterMs);

        Serial.print("scale diag: DAT pin ");
        Serial.print(PIN_HXDAT);
        Serial.print(", CLK pin ");
        Serial.print(PIN_HXCLK);
        Serial.println(", clock held idle low for this test");

        if (low) {
            Serial.print("  DOUT went LOW after ");
            Serial.print(wentLowAfterMs);
            Serial.println(" ms -> a chip is present, powered and converting");
        }
        else {
            Serial.print("  DOUT stayed HIGH for ");
            Serial.print(kDoutWaitMs);
            Serial.println(" ms -> nothing is driving it");
            Serial.println("  Check: DAT to pin 38, CLK to pin 39, VCC to 3.3V, GND common.");
            Serial.println("  Do NOT power it from 5V - DOUT would then swing to 5V and the");
            Serial.println("  Teensy is not 5V tolerant.");
        }

        if (cell != nullptr) {
            Serial.print("  library: ");
            Serial.print(cell->getSPS(), 2);
            Serial.print(" conversions/s, conversion time ");
            Serial.print(cell->getConversionTime(), 1);
            Serial.print(" us, read index ");
            Serial.print(cell->getReadIndex());
            Serial.print(", settling ");
            Serial.print(cell->getSettlingTime());
            Serial.println(" ms");
            Serial.print("  flags: tare timeout ");
            Serial.print(cell->getTareTimeoutFlag() ? "SET" : "clear");
            Serial.print(", signal timeout ");
            Serial.print(cell->getSignalTimeoutFlag() ? "SET" : "clear");
            Serial.println("  (signal timeout is unreliable here - see the header)");
            Serial.print("  tare offset ");
            Serial.println(cell->getTareOffset());
        }
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
        else if (calibration < 0.0f) {
            // Not a mistake and not worth "fixing" in hardware: this load cell
            // reads negative under load, and calibrating against a positive
            // known mass produces a negative factor that corrects the sign for
            // free. The library's setReverseOutput() would do the same thing
            // but cannot be undone, so the factor is the better place for it.
            Serial.print(", negative factor - load cell reads inverted, corrected here");
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
