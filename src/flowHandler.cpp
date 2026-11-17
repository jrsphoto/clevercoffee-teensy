/**
 * @file flowHandler.cpp
 *
 * @brief Digmesa DM60 Nano flow sensor: pulse counting, volume and flow rate
 */

#include "flowHandler.h"

#include <Arduino.h>
#include <EEPROM.h>

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

    namespace {

        /**
         * The calibration survives a reboot, in EEPROM.
         *
         * Without this it reverted to the compiled default on every reset,
         * which on 2026-11-16 meant a measured 1899 pulses/litre was silently
         * replaced by the DM60's 49162 when the board was power-cycled during
         * wiring - a 25x error in delivered volume, reported with total
         * confidence. Exactly the failure the scale's own EEPROM record exists
         * to prevent, so this follows that file's pattern deliberately.
         *
         * Address 192, after the scale (64-127) and the config store (128+).
         * Guarded by magic, version and checksum, and loaded ONLY if all three
         * agree plus the value is sane. Unprogrammed EEPROM reads as 0xFF
         * everywhere, which would otherwise look like a huge but plausible
         * calibration.
         */
        constexpr int kEepromAddress = 192;
        constexpr uint32_t kStoreMagic = 0xF10C0001; // "FLOC 0001"
        constexpr uint16_t kStoreVersion = 1;

        /** Outside this, a stored record is treated as corrupt. */
        constexpr uint32_t kMinPulsesPerLitre = 100;
        constexpr uint32_t kMaxPulsesPerLitre = 200000;

        struct StoredCalibration {
                uint32_t magic;
                uint16_t version;
                uint16_t reserved;
                uint32_t pulsesPerLitre;
                uint32_t checksum;
        };

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
            rec.pulsesPerLitre = calibrationPulsesPerLitre;
            rec.checksum = storeChecksum(rec);

            EEPROM.put(kEepromAddress, rec);

            Serial.print("flow: calibration saved (");
            Serial.print(calibrationPulsesPerLitre);
            Serial.println(" pulses/litre) - it will survive a reboot and a reflash");
        }

        /** @return true if a believable record was found and applied. */
        bool loadCalibration() {
            StoredCalibration rec{};
            EEPROM.get(kEepromAddress, rec);

            if (rec.magic != kStoreMagic || rec.version != kStoreVersion) {
                return false;
            }

            if (rec.checksum != storeChecksum(rec)) {
                Serial.println("flow: stored calibration failed its checksum - ignoring it");

                return false;
            }

            if (rec.pulsesPerLitre < kMinPulsesPerLitre || rec.pulsesPerLitre > kMaxPulsesPerLitre) {
                Serial.println("flow: stored calibration is out of range - ignoring it");

                return false;
            }

            calibrationPulsesPerLitre = rec.pulsesPerLitre;

            return true;
        }

    } // namespace

    void init() {
        // INPUT_PULLUP covers both cases: the real board carries an external
        // pull-up to 3.3 V for the open-collector sensor, and on a bare Teensy
        // the internal one is enough for bench work or for a signal generator
        // driving the pin actively.
        pinMode(PIN_FLOW, INPUT_PULLUP);

        attachInterrupt(digitalPinToInterrupt(PIN_FLOW), onPulse, FALLING);

        if (loadCalibration()) {
            Serial.print("flow: calibration restored, ");
            Serial.print(calibrationPulsesPerLitre);
            Serial.println(" pulses/litre");
        }
        else {
            Serial.print("flow: no stored calibration, using the compiled ");
            Serial.print(calibrationPulsesPerLitre);
            Serial.println(" pulses/litre - run 'flowcal' against a measured volume");
        }

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
            saveCalibration();
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
        saveCalibration();

        return true;
    }

    namespace {

        bool watching = false;
        uint32_t watchUntilMs = 0;
        uint32_t watchChanges = 0;
        int watchLastLevel = -1;
        uint32_t watchStartPulses = 0;

    } // namespace

    void startWatch(const uint32_t durationMs) {
        watching = true;
        watchUntilMs = millis() + durationMs;
        watchChanges = 0;
        watchLastLevel = -1;
        watchStartPulses = pulseCount();

        Serial.print("watching the flow signal on pin ");
        Serial.print(PIN_FLOW);
        Serial.print(" for ");
        Serial.print(durationMs / 1000);
        Serial.println("s - run water, or blow through the sensor.");
        Serial.println("  stuck HIGH = nothing pulling the line down");
        Serial.println("  stuck LOW  = something holding it at ground");
        Serial.println("  changing   = signal is arriving");
    }

    void serviceWatch() {
        if (!watching) {
            return;
        }

        const int level = digitalRead(PIN_FLOW);

        if (level != watchLastLevel) {
            if (watchLastLevel >= 0) {
                ++watchChanges;
            }

            watchLastLevel = level;
        }

        if (static_cast<int32_t>(millis() - watchUntilMs) >= 0) {
            watching = false;

            Serial.print("flow watch finished: line is ");
            Serial.print(level == HIGH ? "HIGH" : "LOW");
            Serial.print(", ");
            Serial.print(watchChanges);
            Serial.print(" level changes, ");
            // Guard the subtraction. 'flowreset' can zero the counter while a
            // watch is running, which made this underflow and report
            // 4,294,967,279 pulses - a number so absurd it was obvious, but the
            // same wrap in any other arithmetic would not have been.
            const uint32_t nowPulses = pulseCount();
            const uint32_t pulsesSeen = (nowPulses >= watchStartPulses) ? (nowPulses - watchStartPulses) : nowPulses;

            Serial.print(pulsesSeen);
            Serial.println(" pulses counted during the watch");

            // The interrupt is the authority here, NOT the polled level. This
            // loop samples the pin once per main-loop pass, which is far too
            // slow to catch a pulse only microseconds wide - so "no level
            // changes" means this poll missed them, not that the line is dead.
            //
            // The first version of this reported "STUCK LOW - the signal never
            // moved" while 215 interrupts were being counted on the same pin.
            // A diagnostic that contradicts itself is worse than none, because
            // it sends you looking for a wiring fault that is not there.
            if (pulsesSeen > 0) {
                Serial.println("  Signal IS arriving - the interrupt counted pulses.");
                Serial.println("  Zero level changes above just means polling from the main loop");
                Serial.println("  is too slow to see them, which is expected and not a fault.");

                return;
            }

            if (watchChanges == 0) {
                Serial.print("  no pulses and no level change - line sat ");
                Serial.println(level == HIGH ? "HIGH" : "LOW");

                if (level == HIGH) {
                    Serial.println("  The internal pull-up is holding it, so nothing is driving the");
                    Serial.println("  pin. Check: signal wire actually on pin 10; the sensor has its");
                    Serial.println("  own supply (an open-collector output cannot switch unpowered);");
                    Serial.println("  and it shares a ground with the Teensy. Also check nothing");
                    Serial.println("  ELSE is connected to the pin - a second driver wins over a");
                    Serial.println("  pull-up and over an open-collector sensor both.");
                }
                else {
                    Serial.println("  Something is holding the line at ground and overriding the");
                    Serial.println("  pull-up. Check for a short, signal and ground swapped, or");
                    Serial.println("  another output driving this pin - a signal generator left");
                    Serial.println("  connected and idling low does exactly this.");
                }
            }
        }
    }

    void sampleFast(const uint32_t windowMs) {
        // Bounded hard. This blocks, and the watchdog bites at 4 s.
        const uint32_t window = windowMs > 500 ? 500 : windowMs;

        const uint32_t pulsesBefore = pulseCount();

        Serial.print("sampling pin ");
        Serial.print(PIN_FLOW);
        Serial.print(" as fast as possible for ");
        Serial.print(window);
        Serial.println(" ms...");

        uint32_t samples = 0;
        uint32_t lowSamples = 0;
        uint32_t transitions = 0;

        const uint32_t began = millis();
        int last = digitalReadFast(PIN_FLOW);

        while (millis() - began < window) {
            const int now = digitalReadFast(PIN_FLOW);

            ++samples;

            if (now == LOW) {
                ++lowSamples;
            }

            if (now != last) {
                ++transitions;
                last = now;
            }
        }

        const uint32_t pulsesDuring = pulseCount() - pulsesBefore;
        const float lowPct = samples ? (100.0f * static_cast<float>(lowSamples) / static_cast<float>(samples)) : 0.0f;
        const float rateMHz = samples ? (static_cast<float>(samples) / (static_cast<float>(window) * 1000.0f)) : 0.0f;

        Serial.print("  ");
        Serial.print(samples);
        Serial.print(" samples (");
        Serial.print(rateMHz, 2);
        Serial.print(" MHz), ");
        Serial.print(transitions);
        Serial.print(" transitions, line low ");
        Serial.print(lowPct, 1);
        Serial.print("% of the time, ");
        Serial.print(pulsesDuring);
        Serial.println(" interrupts in the same window");

        if (transitions == 0) {
            Serial.print("  The line did not move at all. Held ");
            Serial.print(lowPct > 50.0f ? "LOW" : "HIGH");
            Serial.println(" - this is not something the firmware can fix.");
        }
        else if (pulsesDuring == 0) {
            Serial.println("  TRANSITIONS BUT NO INTERRUPTS - the signal is arriving and the");
            Serial.println("  interrupt is not firing. That is a firmware fault, not wiring.");
        }
        else {
            Serial.println("  Signal moving and interrupts firing - the path works.");
        }
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
            // Not "uncalibrated". 49,162 is the DM60's datasheet resolution for
            // this exact part number, quoted to +/-2%, so it is a real figure
            // rather than a placeholder. Saying otherwise would invite someone
            // to distrust a number that is good enough to use.
            Serial.println("  (datasheet value, +/-2%; 'flowcal' to beat it gravimetrically)");
        }
        else {
            Serial.println();
        }
    }

} // namespace flow
