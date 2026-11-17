/**
 * @file weightHandler.cpp
 *
 * @brief HX711 load cell: brew-by-weight
 */

#include "weightHandler.h"

#include <Arduino.h>
#include <EEPROM.h>

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

        /**
         * The second load cell, when fitted. On this machine two cells sit
         * about an inch apart under one 3D-printed plate, so BOTH must be read:
         * the plate shares the load between them by position, and only the sum
         * is independent of where the mass lands. Reading one alone measures an
         * unknown fraction and looks like a faulty cell.
         */
        HX711_ADC* cell2 = nullptr;

        /** Two cells under a shared plate is this machine's arrangement. */
        bool useDualCell = true;

        /**
         * Is a scale fitted at all? Pushed from the head's Hardware page.
         *
         * Default TRUE here because this machine has one and it is verified
         * working. Must match the head's registered default - see the warning
         * in CLAUDE.md about the head winning every disagreement.
         */
        bool scaleEnabled = true;

        /**
         * Mass of the calibration reference, in grams, used by 'scalecal' and
         * by the web UI's calibration flow. Upstream's default is the drip
         * tray's weight.
         */
        float knownWeightG = 267.0f;

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

        /**
         * Cell 2's factor, SEPARATE from cell 1's and with its own sign.
         *
         * This is not over-engineering, it is what the hardware demands. The
         * two cells on this machine are mounted mirrored, so one reads negative
         * under load while the other reads positive: measured at 411.7 g,
         * cell 1 gave -258585 counts and cell 2 gave +195133. Summing those raw
         * destroys about three quarters of the signal, and how much it destroys
         * depends on where the load sits - which reintroduces exactly the
         * position dependence that reading both cells was meant to remove.
         *
         * Giving each cell a factor carrying its own sign makes both channels
         * contribute POSITIVELY, so the total is the real load wherever it
         * lands. Gaggiuino documents the same thing for the same reason: with
         * mirrored housings one scale factor comes out negative.
         *
         * Alternatively the A+/A- pair on one cell could be swapped to make
         * both read the same way in hardware. The firmware handling it costs
         * nothing and leaves the wiring alone.
         */
        float calibration2 = 1.0f;

        bool tareRunning = false;
        bool tare1Done = false;
        bool tare2Done = false;

        /**
         * Pending calibration. Taking a calibration point means averaging a
         * dataset that contains ONLY post-load conversions, which upstream does
         * with the library's refreshDataSet(). That function blocks until the
         * set refills - seconds at 10 conversions per second - and the watchdog
         * here is armed with a 4 second bite, so calling it would reset the
         * board mid-calibration. This counts the conversions from loop()
         * instead and computes the factor when enough have arrived.
         */
        bool calibrationPending = false;
        float calibrationTargetGrams = 0.0f;
        int calibrationSamplesLeft = 0;
        uint32_t calibrationStartedMs = 0;

        /** Bound on collecting a fresh dataset, in case conversions dry up. */
        constexpr uint32_t kCalibrationTimeoutMs = 15000;

        /**
         * Saturation threshold in raw counts.
         *
         * The HX711 is 24-bit signed, so full scale is +/-8,388,607. At 90% of
         * that the cell is effectively at the stop: the converter still returns
         * a number but it no longer tracks the load, so the reading is fiction.
         * Left short of the absolute limit because the last tenth of the range
         * is already badly nonlinear.
         */
        constexpr float kSaturationCounts = 7550000.0f;

        bool cellSaturated = false;
        int saturatedCell = 0;

        /**
         * Calibration survives a reboot, in EEPROM.
         *
         * Without this the factor reverts to 1.0 on every reset and every
         * reflash, which during development means recalibrating against a
         * known mass several times an hour. Teensy 4.1 emulates EEPROM in
         * flash, so this also survives a firmware upload.
         *
         * Guarded by a magic number, a version and a checksum, and loaded ONLY
         * if all three agree. Unprogrammed EEPROM reads as 0xFF everywhere,
         * which would otherwise be interpreted as a calibration factor - and a
         * garbage factor that happens to look plausible is precisely the
         * failure this code refuses to have anywhere else. A rejected record
         * leaves the scale uncalibrated, reporting raw counts, which is
         * obvious.
         *
         * Address 64 rather than 0, leaving the start of EEPROM free for
         * whatever the config system wants when it arrives in phase 6.
         */
        constexpr int kEepromAddress = 64;
        constexpr uint32_t kStoreMagic = 0x5CA1E001; // "SCALE 001"
        constexpr uint16_t kStoreVersion = 1;

        struct StoredCalibration {
                uint32_t magic;
                uint16_t version;
                uint16_t samples;
                float cal1;
                float cal2;
                uint8_t dual;
                uint8_t reserved[3];
                uint32_t checksum;
        };

        /** Deliberately trivial - this is a corruption check, not security. */
        uint32_t storeChecksum(const StoredCalibration& rec) {
            uint32_t sum = 0;
            const auto* bytes = reinterpret_cast<const uint8_t*>(&rec);

            for (size_t i = 0; i < offsetof(StoredCalibration, checksum); ++i) {
                sum = (sum << 1) ^ bytes[i];
            }

            return sum;
        }

        void saveCalibration() {
            StoredCalibration rec{};

            rec.magic = kStoreMagic;
            rec.version = kStoreVersion;
            rec.samples = static_cast<uint16_t>(samplesInUse);
            rec.cal1 = calibration;
            rec.cal2 = calibration2;
            rec.dual = useDualCell ? 1 : 0;
            rec.checksum = storeChecksum(rec);

            EEPROM.put(kEepromAddress, rec);

            Serial.println("scale: calibration saved - it will survive a reboot and a reflash");
        }

        /**
         * @brief Load a stored calibration, if there is a believable one.
         *
         * @return true if a valid record was applied.
         */
        bool loadCalibration() {
            StoredCalibration rec{};
            EEPROM.get(kEepromAddress, rec);

            if (rec.magic != kStoreMagic || rec.version != kStoreVersion) {
                return false;
            }

            if (rec.checksum != storeChecksum(rec)) {
                Serial.println("scale: stored calibration failed its checksum - ignoring it");

                return false;
            }

            // Sanity-check the values themselves. A record can pass its
            // checksum and still be nonsense if it was written by a different
            // build, and a wrong factor silently mis-doses every shot.
            if (rec.cal1 == 0.0f || isnan(rec.cal1) || isinf(rec.cal1) || rec.cal2 == 0.0f || isnan(rec.cal2) || isinf(rec.cal2)) {
                Serial.println("scale: stored calibration holds impossible factors - ignoring it");

                return false;
            }

            calibration = rec.cal1;
            calibration2 = rec.cal2;
            useDualCell = (rec.dual != 0);

            if (rec.samples >= 1 && rec.samples <= 32) {
                samplesInUse = rec.samples;
            }

            Serial.print("scale: restored calibration ");
            Serial.print(calibration, 2);
            Serial.print("/");
            Serial.print(calibration2, 2);
            Serial.print(", ");
            Serial.print(useDualCell ? "2 cells, " : "1 cell, ");
            Serial.print(samplesInUse);
            Serial.println(" samples");

            return true;
        }

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
        bool waitForDoutLow(int dataPin, uint32_t* wentLowAfterMs) {
            // Clock idle low. If SCK were left high the HX711 would be in
            // power-down and would never produce a conversion, which would
            // look exactly like an absent chip.
            pinMode(PIN_HXCLK, OUTPUT);
            digitalWrite(PIN_HXCLK, LOW);

            pinMode(dataPin, INPUT_PULLUP);

            const uint32_t began = millis();

            while (millis() - began < kDoutWaitMs) {
                if (digitalRead(dataPin) == LOW) {
                    *wentLowAfterMs = millis() - began;

                    return true;
                }
            }

            *wentLowAfterMs = kDoutWaitMs;

            return false;
        }

        /**
         * @brief Finish a pending calibration once the dataset is fresh.
         *
         * Called from loop() while running. Counts conversions rather than
         * blocking, then takes the factor from a set that holds only post-load
         * readings - which is the whole point, and what was wrong with the
         * first attempt at calibrating this.
         */
        void serviceCalibration() {
            if (!calibrationPending) {
                return;
            }

            if (millis() - calibrationStartedMs > kCalibrationTimeoutMs) {
                calibrationPending = false;

                Serial.println("scale: calibration ABANDONED - conversions stopped arriving");

                return;
            }

            if (calibrationSamplesLeft > 0) {
                return; // loop() decrements this as conversions land
            }

            calibrationPending = false;

            // Recover the RAW counts behind each cell's reading: getData()
            // returns (smoothed - tare) / calFactor, so multiplying back by the
            // factor in force gives the raw count again.
            const float raw1 = cell->getData() * cell->getCalFactor();
            float raw2 = 0.0f;
            float rawMagnitudeTotal = fabsf(raw1);

            if (cell2 != nullptr) {
                raw2 = cell2->getData() * cell2->getCalFactor();
                rawMagnitudeTotal += fabsf(raw2);
            }

            // Sum the MAGNITUDES, not the signed values. The cells are
            // mirrored, so one goes negative and one positive under the same
            // load; adding them signed cancels most of the signal, and the
            // amount cancelled depends on where the mass sits.
            //
            // Each cell then gets that one magnitude carrying ITS OWN sign, so
            // both channels read positive under load and the total is the real
            // weight wherever it lands on the plate.
            const float magnitude = rawMagnitudeTotal / calibrationTargetGrams;

            if (magnitude == 0.0f || isnan(magnitude) || isinf(magnitude)) {
                Serial.println("scale: calibration REFUSED - the computed factor is not a number.");
                Serial.println("       Almost always means no load was actually on the cell.");

                return;
            }

            calibration = (raw1 < 0.0f) ? -magnitude : magnitude;
            cell->setCalFactor(calibration);

            if (cell2 != nullptr) {
                calibration2 = (raw2 < 0.0f) ? -magnitude : magnitude;
                cell2->setCalFactor(calibration2);

                Serial.print("scale: cell 1 factor ");
                Serial.print(calibration, 2);
                Serial.print(", cell 2 factor ");
                Serial.println(calibration2, 2);

                if ((raw1 < 0.0f) != (raw2 < 0.0f)) {
                    Serial.println("       Cells read OPPOSITE ways (mirrored mounting) - the signs");
                    Serial.println("       above are what make both count toward the total.");
                }

                // How the plate shared the load. Any split works now, but a
                // very lopsided one wastes range on the lightly loaded cell.
                const float share1 = (rawMagnitudeTotal != 0.0f) ? (fabsf(raw1) / rawMagnitudeTotal) * 100.0f : 0.0f;

                Serial.print("       load split cell1/cell2 = ");
                Serial.print(share1, 0);
                Serial.print("/");
                Serial.print(100.0f - share1, 0);
                Serial.println(" %");
            }

            Serial.print("scale: calibrated to ");
            Serial.print(calibrationTargetGrams, 2);
            Serial.print(" g, factor is now ");
            Serial.println(calibration, 4);

            saveCalibration();
            Serial.println("       Verify it: lift the mass and put it back. If the two readings");
            Serial.println("       differ by more than ~0.1 g the cell is not mounted rigidly");
            Serial.println("       enough and the factor is meaningless.");
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
        // Not fitted means do not create anything and do not report a failure.
        // A machine with no scale is a supported configuration - brewing by
        // time and by volume both still work - and it must not look like
        // broken hardware on the panel or in the log.
        if (!scaleEnabled) {
            delete scale;
            scale = nullptr;
            cell = nullptr;
            cell2 = nullptr;
            state = State::NotCreated;
            failureReason = "";

            Serial.println("scale: disabled by configuration");

            return false;
        }

        // Only on the very first call: a restart() must not quietly undo a
        // factor set by hand with 'scalefactor' since boot.
        static bool triedLoad = false;

        if (!triedLoad) {
            triedLoad = true;
            loadCalibration();
        }

        delete scale;
        cell = nullptr;
        cell2 = nullptr;

        // Two cells share the clock pin and have their own data pins, each with
        // its own HX711. That is what the board's pin map was laid out for.
        if (useDualCell) {
            scale = new HX711Scale(PIN_HXDAT, PIN_HXDAT2, PIN_HXCLK, calibration, calibration2);
        }
        else {
            scale = new HX711Scale(PIN_HXDAT, PIN_HXCLK, calibration);
        }

        if (scale == nullptr) {
            state = State::NotCreated;

            return false;
        }

        cell = scale->getLoadCell(1);
        cell2 = useDualCell ? scale->getLoadCell(2) : nullptr;

        if (cell == nullptr || (useDualCell && cell2 == nullptr)) {
            state = State::NotCreated;

            return false;
        }

        // Check the chips are actually there BEFORE handing over to the
        // library, which has no reliable way to tell us. See waitForDoutLow().
        uint32_t doutLowAfterMs = 0;

        if (!waitForDoutLow(PIN_HXDAT, &doutLowAfterMs)) {
            enterFailed("cell 1 DOUT never went low - no HX711 on pin 38");

            return true;
        }

        Serial.print("scale: cell 1 DOUT low after ");
        Serial.print(doutLowAfterMs);
        Serial.println(" ms");

        if (useDualCell) {
            if (!waitForDoutLow(PIN_HXDAT2, &doutLowAfterMs)) {
                // Deliberately a hard failure rather than a quiet fall back to
                // one cell. With a shared plate, one cell is not a degraded
                // reading, it is a meaningless one - and silently reporting a
                // fraction of the weight as if it were the weight is exactly
                // how brew-by-weight would mis-dose every shot.
                enterFailed("cell 2 DOUT never went low - no HX711 on pin 40 ('scaledual 0' for one cell)");

                return true;
            }

            Serial.print("scale: cell 2 DOUT low after ");
            Serial.print(doutLowAfterMs);
            Serial.println(" ms");
        }

        cell->begin();

        if (cell2 != nullptr) {
            cell2->begin();
        }

        pinMode(PIN_HXDAT, INPUT_PULLUP);

        if (useDualCell) {
            pinMode(PIN_HXDAT2, INPUT_PULLUP);
        }

        // Disable the library's tare timeout and bound the attempt here
        // instead. Its deadline is a function-level static, computed once per
        // program run, so a second attempt would report an instant timeout.
        cell->disableTareTimeout();
        cell->setSamplesInUse(samplesInUse);

        if (cell2 != nullptr) {
            cell2->disableTareTimeout();
            cell2->setSamplesInUse(samplesInUse);
        }

        startBeganMs = millis();
        haveEverRead = false;
        tareRunning = false;
        calibrationPending = false;
        state = State::Stabilising;

        Serial.print("scale: HX711 ");
        Serial.print(useDualCell ? "DUAL cell (DAT 38 + 40" : "single cell (DAT 38");
        Serial.print(", shared CLK ");
        Serial.print(PIN_HXCLK);
        Serial.print("), stabilising ");
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
                // (...))`: called once per pass until it reports done. Both
                // cells are driven, exactly as upstream's dual-cell branch
                // does - startMultiple exists precisely so several HX711s can
                // stabilise and tare at the same time.
                int done = cell->startMultiple(kStabiliseMs, true);

                if (cell2 != nullptr) {
                    const int done2 = cell2->startMultiple(kStabiliseMs, true);

                    // Both must be finished before the reading means anything.
                    done = (done != 0 && done2 != 0) ? 1 : 0;
                }

                if (done != 0) {
                    if (cell->getSignalTimeoutFlag()) {
                        enterFailed("no response from the HX711 (check DAT/CLK and power)");

                        return;
                    }

                    cell->setCalFactor(calibration);
                    cell->setSamplesInUse(samplesInUse);

                    if (cell2 != nullptr) {
                        cell2->setCalFactor(calibration2);
                        cell2->setSamplesInUse(samplesInUse);
                    }

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

                    // Check each cell against the end of its range. One
                    // saturated cell corrupts the total, because the total is
                    // a sum - so this has to be per-cell, not a sanity check
                    // on the final number.
                    const float raw1 = cell->getData() * cell->getCalFactor();
                    const float raw2 = (cell2 != nullptr) ? cell2->getData() * cell2->getCalFactor() : 0.0f;

                    const bool wasSaturated = cellSaturated;

                    if (fabsf(raw1) > kSaturationCounts) {
                        cellSaturated = true;
                        saturatedCell = 1;
                    }
                    else if (cell2 != nullptr && fabsf(raw2) > kSaturationCounts) {
                        cellSaturated = true;
                        saturatedCell = 2;
                    }
                    else {
                        cellSaturated = false;
                        saturatedCell = 0;
                    }

                    if (cellSaturated && !wasSaturated) {
                        Serial.print("scale: CELL ");
                        Serial.print(saturatedCell);
                        Serial.println(" SATURATED - reading is not usable.");
                        Serial.println("       A mass set down outside the span between the cells");
                        Serial.println("       levers the plate and pries the far cell upward.");
                    }

                    if (calibrationPending && calibrationSamplesLeft > 0) {
                        --calibrationSamplesLeft;
                    }
                }

                // Losing the chip mid-shot is caught by staleness in healthy()
                // rather than by the library's flag, for the reason given above.
                // update() is called once per pass here, which does NOT reset
                // the flag's timestamp, but it is simpler and more honest to
                // measure the thing we actually care about: whether a fresh
                // conversion arrived recently.

                if (tareRunning) {
                    // getTareStatus() is read-and-clear, so each cell's flag
                    // must be collected the pass it appears rather than
                    // re-tested later.
                    if (cell->getTareStatus()) {
                        tare1Done = true;
                    }

                    if (cell2 != nullptr && cell2->getTareStatus()) {
                        tare2Done = true;
                    }

                    if (tare1Done && (cell2 == nullptr || tare2Done)) {
                        tareRunning = false;

                        Serial.println("scale: tare complete");
                    }
                }

                serviceCalibration();

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
        return state == State::Running && haveEverRead && !tareRunning && !cellSaturated && msSinceLastReading() <= kStaleAfterMs;
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
        //
        // BOTH cells, or the zero is only half corrected and every reading
        // carries the other cell's offset.
        tare1Done = false;
        tare2Done = false;

        cell->tareNoDelay();

        if (cell2 != nullptr) {
            cell2->tareNoDelay();
        }

        tareRunning = true;

        Serial.println("scale: taring...");
    }

    bool saturated() {
        return cellSaturated;
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

        if (calibrationPending) {
            Serial.println("scale: a calibration is already in progress");

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

        // Warn rather than refuse. Gaggiuino's guidance for the same class of
        // load cell is a 200-400 g reference; well below that, the cell is
        // working in the few percent of its range where its own nonlinearity
        // and the mounting both matter most.
        if (knownGrams < 150.0f) {
            Serial.print("scale: NOTE - ");
            Serial.print(knownGrams, 1);
            Serial.println(" g is a light reference for this cell.");
            Serial.println("       200-400 g gives a far more trustworthy factor.");
        }

        // Discard the whole averaging window and wait for it to refill with
        // post-load conversions only. Upstream does this with the library's
        // blocking refreshDataSet(); here it is counted from loop(), because
        // blocking for seconds would trip the watchdog.
        cell->resetSamplesIndex();

        if (cell2 != nullptr) {
            cell2->resetSamplesIndex();
        }

        calibrationTargetGrams = knownGrams;

        // +2 for the ignored high/low samples, doubled with two cells because
        // HX711Scale::update() alternates between them, so one successful
        // update refreshes only one cell's window.
        calibrationSamplesLeft = (samplesInUse + 2) * (cell2 != nullptr ? 2 : 1);
        calibrationStartedMs = millis();
        calibrationPending = true;

        Serial.print("scale: collecting ");
        Serial.print(calibrationSamplesLeft);
        Serial.println(" fresh conversions - keep the mass still and do not touch the cell");

        return true;
    }

    float calFactor() {
        return calibration;
    }

    void setCalFactor(const float factor) {
        calibration = factor;
        calibration2 = factor;

        saveCalibration();

        if (cell != nullptr) {
            cell->setCalFactor(factor);
        }

        if (cell2 != nullptr) {
            cell2->setCalFactor(factor);
        }
    }

    float calFactor2() {
        return calibration2;
    }

    void setCalFactor2(const float factor) {
        // Cell 2 only. setCalFactor() writes BOTH, which is right for a single
        // cell and wrong for this machine's mirrored pair - they differ in sign
        // and slightly in magnitude, so each needs its own number.
        calibration2 = factor;

        saveCalibration();

        if (cell2 != nullptr) {
            cell2->setCalFactor(factor);
        }
    }

    void setEnabled(const bool on) {
        if (on == scaleEnabled) {
            return;
        }

        scaleEnabled = on;

        Serial.print("scale: ");
        Serial.println(on ? "enabled" : "disabled");

        // Recreate or tear down now rather than at the next boot, so the
        // setting visibly does something.
        init();
    }

    bool enabled() {
        return scaleEnabled;
    }

    void setKnownWeight(const float grams) {
        knownWeightG = grams;
    }

    float knownWeight() {
        return knownWeightG;
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
        Serial.print("scale diag: ");
        Serial.print(useDualCell ? "DUAL cell" : "single cell");
        Serial.print(", DAT ");
        Serial.print(PIN_HXDAT);

        if (useDualCell) {
            Serial.print(" + ");
            Serial.print(PIN_HXDAT2);
        }

        Serial.print(", shared CLK ");
        Serial.print(PIN_HXCLK);
        Serial.println(", clock held idle low for this test");

        const int pins[2] = {PIN_HXDAT, PIN_HXDAT2};
        const int count = useDualCell ? 2 : 1;

        for (int i = 0; i < count; ++i) {
            uint32_t wentLowAfterMs = 0;
            const bool low = waitForDoutLow(pins[i], &wentLowAfterMs);

            Serial.print("  cell ");
            Serial.print(i + 1);
            Serial.print(" (pin ");
            Serial.print(pins[i]);
            Serial.print("): ");

            if (low) {
                Serial.print("DOUT LOW after ");
                Serial.print(wentLowAfterMs);
                Serial.println(" ms -> present, powered, converting");
            }
            else {
                Serial.print("DOUT stayed HIGH for ");
                Serial.print(kDoutWaitMs);
                Serial.println(" ms -> nothing driving it");
                Serial.println("    Check DAT, CLK, VCC to 3.3V, GND common. Do NOT use 5V -");
                Serial.println("    DOUT would swing to 5V and the Teensy is not 5V tolerant.");
            }
        }

        // Per-cell readings. With a shared plate BOTH should move when a mass
        // is placed anywhere on it. One cell dead while the other reads is
        // precisely the condition that makes the total depend on position.
        if (cell != nullptr && state == State::Running) {
            Serial.print("  cell 1 reads ");
            Serial.print(cell->getData(), 1);
            Serial.print(" (factor ");
            Serial.print(cell->getCalFactor(), 2);
            Serial.println(")");

            if (cell2 != nullptr) {
                Serial.print("  cell 2 reads ");
                Serial.print(cell2->getData(), 1);
                Serial.print(" (factor ");
                Serial.print(cell2->getCalFactor(), 2);
                Serial.println(")");
            }

            Serial.print("  ");
            Serial.print(cell->getSPS(), 2);
            Serial.print(" conversions/s, cell 1 tare offset ");
            Serial.println(cell->getTareOffset());
        }
    }

    void setDualCell(const bool dual) {
        useDualCell = dual;

        Serial.print("scale: now configured for ");
        Serial.println(dual ? "TWO load cells (DAT 38 + 40)" : "ONE load cell (DAT 38)");

        init();
    }

    bool dualCell() {
        return useDualCell;
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

        if (useDualCell) {
            Serial.print("/");
            Serial.print(calibration2, 4);
        }

        if (calibration == 1.0f) {
            Serial.print(", UNCALIBRATED - this is raw counts, not grams");
        }
        else if (useDualCell && ((calibration < 0.0f) != (calibration2 < 0.0f))) {
            // Not a fault. The cells are mounted mirrored, so one reads
            // negative under load; the opposing signs are what make both
            // contribute to the total instead of cancelling.
            Serial.print(", opposite signs - mirrored cells, both counted");
        }
        else if (calibration < 0.0f) {
            Serial.print(", negative factor - cell reads inverted, corrected here");
        }

        Serial.print("), ");
        Serial.print(useDualCell ? "2 cells, " : "1 cell, ");
        Serial.print(samplesInUse);
        Serial.print(" samples, last reading ");
        Serial.print(msSinceLastReading());
        Serial.print(" ms ago, ");
        Serial.println(healthy() ? "healthy" : "NOT HEALTHY - weight must not be used");

        if (tareRunning) {
            Serial.println("scale: tare in progress");
        }

        if (cellSaturated) {
            Serial.print("scale: CELL ");
            Serial.print(saturatedCell);
            Serial.println(" IS SATURATED - the weight above is meaningless.");
            Serial.println("       Move the load between the two cells, or move the cells");
            Serial.println("       further apart so a load cannot lever the plate.");
        }
    }

} // namespace weight
