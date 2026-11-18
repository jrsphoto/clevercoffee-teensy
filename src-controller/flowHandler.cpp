/**
 * @file flowHandler.cpp
 *
 * @brief Flow sensing on two inputs: pulse counting, volume and flow rate
 *
 * The two channels are identical in everything but their pin, their compiled
 * default calibration and their EEPROM address, so all the behaviour lives in
 * one Sensor struct and there are two of them. The only thing that cannot be
 * shared is the interrupt itself - attachInterrupt wants a plain function, so
 * there is one trivial shim per channel and both call the same handler.
 */

#include "flowHandler.h"

#include <Arduino.h>
#include <EEPROM.h>
#include <stdio.h>
#include <string.h>

#include "hardware/pinmapping.h"

namespace flow {

    namespace {

        /**
         * One input's complete state.
         *
         * Everything the ISR touches is volatile, and everything else is
         * derived outside it - the interrupt does the least work that is
         * correct and nothing more.
         */
        struct Sensor {
                // --- Fixed at construction, never written afterwards --------
                const char* name;
                uint8_t pin;
                uint32_t compiledPulsesPerLitre;
                int eepromAddress;

                // --- Written by the interrupt -------------------------------
                volatile uint32_t pulses;
                volatile uint32_t rejected;
                volatile uint32_t lastPulseUs;
                volatile uint32_t lastPulseMs;

                // --- Main-loop side only ------------------------------------
                uint32_t calibrationPulsesPerLitre;
                uint32_t lastRateCalcMs;
                uint32_t lastRatePulses;
                double currentRateMlPerSec;
        };

        /**
         * The EEPROM record for Flow1 sits at 528, in the gap between the
         * original flow record at 512-527 and the shot profiles at 576. Checked
         * against the map in configStore.cpp before it was placed there - that
         * map exists because two records were once put inside the config
         * store's 128-337 block and every config save silently erased them.
         */
        constexpr int kEepromAddressFlow2 = 512;
        constexpr int kEepromAddressFlow1 = 528;

        Sensor sensors[kChannelCount] = {
            // Flow1: net FLOW1, FLOW_SENS1, the Gicar
            {"FLOW1", PIN_FLOW_GICAR, kGicarPulsesPerLitre, kEepromAddressFlow1, 0, 0, 0, 0, kGicarPulsesPerLitre, 0, 0, 0.0},
            // Flow2: net FLOW2, DIGMESA1, the DM60 Nano - and PIN_FLOW, the
            // active channel
            {"FLOW2", PIN_FLOW_DIGMESA, kDefaultPulsesPerLitre, kEepromAddressFlow2, 0, 0, 0, 0, kDefaultPulsesPerLitre, 0, 0, 0.0},
        };

        Sensor& at(const Channel channel) {
            return sensors[static_cast<uint8_t>(channel)];
        }

        /**
         * @brief The shared pulse handler.
         *
         * Falling edge: both sensors are open-collector, so they pull the line
         * down once per unit volume.
         *
         * Deliberately tiny. No arithmetic, no floating point, no logging -
         * just a reject test and two counters. Volume and rate are derived in
         * the main loop where taking time is harmless. Marked always_inline so
         * splitting one ISR into two shims costs nothing at run time.
         */
        inline __attribute__((always_inline)) void onPulse(Sensor& s) {
            const uint32_t now = micros();

            // Reject implausibly close edges. Genuine pulses cannot arrive this
            // fast at any flow this pump can produce, so anything closer is
            // noise or bounce.
            if ((now - s.lastPulseUs) < kMinPulseIntervalUs) {
                s.rejected++;

                return;
            }

            s.lastPulseUs = now;
            s.lastPulseMs = millis();
            s.pulses++;
        }

        void onPulseFlow1() {
            onPulse(sensors[static_cast<uint8_t>(Channel::Flow1)]);
        }

        void onPulseFlow2() {
            onPulse(sensors[static_cast<uint8_t>(Channel::Flow2)]);
        }

        /**
         * @brief Read a pulse counter without tearing.
         *
         * A 32-bit read is a single instruction on this core, so it cannot tear
         * on its own - but reading several ISR-written values that must agree
         * with each other does need the interrupt held off briefly.
         */
        uint32_t snapshotPulses(const Sensor& s) {
            noInterrupts();
            const uint32_t value = s.pulses;
            interrupts();

            return value;
        }

        /**
         * @brief digitalReadFast on a pin chosen at run time.
         *
         * digitalReadFast only collapses to a single register read when the
         * pin is a compile-time constant, and a Sensor's pin is a field. This
         * branch gives the compiler two constant calls for the cost of one
         * compare, which keeps sampleFast() sampling at megahertz - the whole
         * reason that diagnostic exists is that it cannot miss a short edge.
         */
        inline int fastRead(const uint8_t pin) {
            return (pin == PIN_FLOW_DIGMESA) ? digitalReadFast(PIN_FLOW_DIGMESA) : digitalReadFast(PIN_FLOW_GICAR);
        }

        double mlFromPulses(const Sensor& s, const uint32_t count) {
            if (s.calibrationPulsesPerLitre == 0) {
                return 0.0;
            }

            return (static_cast<double>(count) * 1000.0) / static_cast<double>(s.calibrationPulsesPerLitre);
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
         * NOT inside 128-337, which is the config store - that was the first
         * attempt and every config save wiped it, which looked exactly like the
         * value never being saved at all. Guarded by magic, version and
         * checksum, and loaded ONLY if all three agree plus the value is sane.
         * Unprogrammed EEPROM reads as 0xFF everywhere, which would otherwise
         * look like a huge but plausible calibration.
         *
         * Both channels share the magic. They cannot be confused for each other
         * because each reads only its own address, and a record that somehow
         * landed at the wrong one would still have to pass the range check - at
         * which point it is a wrong calibration rather than a corrupt one, and
         * no magic number could tell the difference.
         */
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

        static_assert(kEepromAddressFlow1 + static_cast<int>(sizeof(StoredCalibration)) <= 576,
                      "the Flow1 calibration record overruns into the shot profiles at 576");
        static_assert(kEepromAddressFlow2 + static_cast<int>(sizeof(StoredCalibration)) <= kEepromAddressFlow1,
                      "the Flow2 calibration record overruns into the Flow1 record");

        uint32_t storeChecksum(const StoredCalibration& rec) {
            uint32_t sum = 0;
            const auto* bytes = reinterpret_cast<const uint8_t*>(&rec);

            for (size_t i = 0; i < offsetof(StoredCalibration, checksum); ++i) {
                sum = (sum << 1) ^ bytes[i];
            }

            return sum;
        }

        void saveCalibration(const Sensor& s) {
            StoredCalibration rec{};

            rec.magic = kStoreMagic;
            rec.version = kStoreVersion;
            rec.pulsesPerLitre = s.calibrationPulsesPerLitre;
            rec.checksum = storeChecksum(rec);

            EEPROM.put(s.eepromAddress, rec);

            Serial.print("flow ");
            Serial.print(s.name);
            Serial.print(": calibration saved (");
            Serial.print(s.calibrationPulsesPerLitre);
            Serial.println(" pulses/litre) - it will survive a reboot and a reflash");
        }

        /** @return true if a believable record was found and applied. */
        bool loadCalibration(Sensor& s) {
            StoredCalibration rec{};
            EEPROM.get(s.eepromAddress, rec);

            if (rec.magic != kStoreMagic || rec.version != kStoreVersion) {
                return false;
            }

            if (rec.checksum != storeChecksum(rec)) {
                Serial.print("flow ");
                Serial.print(s.name);
                Serial.println(": stored calibration failed its checksum - ignoring it");

                return false;
            }

            if (rec.pulsesPerLitre < kMinPulsesPerLitre || rec.pulsesPerLitre > kMaxPulsesPerLitre) {
                Serial.print("flow ");
                Serial.print(s.name);
                Serial.println(": stored calibration is out of range - ignoring it");

                return false;
            }

            s.calibrationPulsesPerLitre = rec.pulsesPerLitre;

            return true;
        }

        void initSensor(Sensor& s, void (*isr)()) {
            // INPUT_PULLUP covers both cases: the real board carries an external
            // pull-up to 3.3 V for the open-collector sensor, and on a bare Teensy
            // the internal one is enough for bench work or for a signal generator
            // driving the pin actively.
            pinMode(s.pin, INPUT_PULLUP);

            attachInterrupt(digitalPinToInterrupt(s.pin), isr, FALLING);

            if (loadCalibration(s)) {
                Serial.print("flow ");
                Serial.print(s.name);
                Serial.print(": calibration restored, ");
                Serial.print(s.calibrationPulsesPerLitre);
                Serial.println(" pulses/litre");
            }
            else {
                Serial.print("flow ");
                Serial.print(s.name);
                Serial.print(": no stored calibration, using the compiled ");
                Serial.print(s.calibrationPulsesPerLitre);
                Serial.println(" pulses/litre - run 'flowcal' against a measured volume");
            }
        }

        void resetSensor(Sensor& s) {
            noInterrupts();
            s.pulses = 0;
            s.rejected = 0;
            interrupts();

            s.lastRatePulses = 0;
            s.lastRateCalcMs = millis();
            s.currentRateMlPerSec = 0.0;
        }

        void loopSensor(Sensor& s) {
            const uint32_t now = millis();

            // Flow has stopped if nothing has arrived for a while. Checked
            // before the window below so the rate drops promptly at the end of
            // a shot rather than decaying over a whole window.
            if ((now - s.lastPulseMs) > kFlowTimeoutMs) {
                s.currentRateMlPerSec = 0.0;
                s.lastRatePulses = snapshotPulses(s);
                s.lastRateCalcMs = now;

                return;
            }

            if ((now - s.lastRateCalcMs) < kRateWindowMs) {
                return;
            }

            const uint32_t nowPulses = snapshotPulses(s);
            const uint32_t deltaPulses = nowPulses - s.lastRatePulses;
            const uint32_t deltaMs = now - s.lastRateCalcMs;

            if (deltaMs > 0) {
                s.currentRateMlPerSec = (mlFromPulses(s, deltaPulses) * 1000.0) / static_cast<double>(deltaMs);
            }

            s.lastRatePulses = nowPulses;
            s.lastRateCalcMs = now;
        }

    } // namespace

    void init() {
        initSensor(at(Channel::Flow1), onPulseFlow1);
        initSensor(at(Channel::Flow2), onPulseFlow2);

        reset(Channel::Flow1);
        reset(Channel::Flow2);
    }

    void loop() {
        loopSensor(at(Channel::Flow1));
        loopSensor(at(Channel::Flow2));
    }

    const char* channelName(const Channel channel) {
        return at(channel).name;
    }

    uint8_t channelPin(const Channel channel) {
        return at(channel).pin;
    }

    //
    // Per-channel accessors.
    //

    void reset(const Channel channel) {
        resetSensor(at(channel));
    }

    double totalMl(const Channel channel) {
        Sensor& s = at(channel);

        return mlFromPulses(s, snapshotPulses(s));
    }

    double rateMlPerSec(const Channel channel) {
        return at(channel).currentRateMlPerSec;
    }

    uint32_t pulseCount(const Channel channel) {
        return snapshotPulses(at(channel));
    }

    uint32_t rejectedCount(const Channel channel) {
        noInterrupts();
        const uint32_t value = at(channel).rejected;
        interrupts();

        return value;
    }

    bool isFlowing(const Channel channel) {
        return (millis() - at(channel).lastPulseMs) <= kFlowTimeoutMs;
    }

    uint32_t pulsesPerLitre(const Channel channel) {
        return at(channel).calibrationPulsesPerLitre;
    }

    void setPulsesPerLitre(const Channel channel, const uint32_t pulsesPerL) {
        if (pulsesPerL > 0) {
            Sensor& s = at(channel);

            s.calibrationPulsesPerLitre = pulsesPerL;
            saveCalibration(s);
        }
    }

    bool calibrateTo(const Channel channel, const double actualMl) {
        Sensor& s = at(channel);
        const uint32_t counted = snapshotPulses(s);

        // Too little to calibrate against. A handful of pulses would produce a
        // confident-looking number with a huge error, which is worse than
        // refusing - so refuse.
        if (counted < 100 || actualMl <= 0.0) {
            return false;
        }

        s.calibrationPulsesPerLitre = static_cast<uint32_t>((static_cast<double>(counted) * 1000.0) / actualMl);
        saveCalibration(s);

        return true;
    }

    //
    // The active channel.
    //

    void reset() {
        reset(kActiveChannel);
    }

    double totalMl() {
        return totalMl(kActiveChannel);
    }

    double rateMlPerSec() {
        return rateMlPerSec(kActiveChannel);
    }

    uint32_t pulseCount() {
        return pulseCount(kActiveChannel);
    }

    uint32_t rejectedCount() {
        return rejectedCount(kActiveChannel);
    }

    bool isFlowing() {
        return isFlowing(kActiveChannel);
    }

    uint32_t pulsesPerLitre() {
        return pulsesPerLitre(kActiveChannel);
    }

    void setPulsesPerLitre(const uint32_t pulsesPerL) {
        setPulsesPerLitre(kActiveChannel, pulsesPerL);
    }

    bool calibrateTo(const double actualMl) {
        return calibrateTo(kActiveChannel, actualMl);
    }

    namespace {

        bool watching = false;
        Channel watchChannel = kActiveChannel;
        uint32_t watchUntilMs = 0;
        uint32_t watchChanges = 0;
        int watchLastLevel = -1;
        uint32_t watchStartPulses = 0;

    } // namespace

    void startWatch(const Channel channel, const uint32_t durationMs) {
        const Sensor& s = at(channel);

        watching = true;
        watchChannel = channel;
        watchUntilMs = millis() + durationMs;
        watchChanges = 0;
        watchLastLevel = -1;
        watchStartPulses = pulseCount(channel);

        Serial.print("watching ");
        Serial.print(s.name);
        Serial.print(" on pin ");
        Serial.print(s.pin);
        Serial.print(" for ");
        Serial.print(durationMs / 1000);
        Serial.println("s - run water, or blow through the sensor.");
        Serial.println("  stuck HIGH = nothing pulling the line down");
        Serial.println("  stuck LOW  = something holding it at ground");
        Serial.println("  changing   = signal is arriving");
    }

    void startWatch(const uint32_t durationMs) {
        startWatch(kActiveChannel, durationMs);
    }

    void serviceWatch() {
        if (!watching) {
            return;
        }

        const Sensor& s = at(watchChannel);
        const int level = digitalRead(s.pin);

        if (level != watchLastLevel) {
            if (watchLastLevel >= 0) {
                ++watchChanges;
            }

            watchLastLevel = level;
        }

        if (static_cast<int32_t>(millis() - watchUntilMs) >= 0) {
            watching = false;

            Serial.print("flow watch finished on ");
            Serial.print(s.name);
            Serial.print(": line is ");
            Serial.print(level == HIGH ? "HIGH" : "LOW");
            Serial.print(", ");
            Serial.print(watchChanges);
            Serial.print(" level changes, ");
            // Guard the subtraction. 'flowreset' can zero the counter while a
            // watch is running, which made this underflow and report
            // 4,294,967,279 pulses - a number so absurd it was obvious, but the
            // same wrap in any other arithmetic would not have been.
            const uint32_t nowPulses = pulseCount(watchChannel);
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
                    Serial.print("  The internal pull-up is holding it, so nothing is driving pin ");
                    Serial.print(s.pin);
                    Serial.println(".");
                    Serial.print("  Check: the sensor is on the ");
                    Serial.print(s.name);
                    Serial.println(" connector and not the other one - there are TWO;");
                    Serial.println("  the sensor has its own supply (an open-collector output cannot");
                    Serial.println("  switch unpowered); and it shares a ground with the Teensy. Also");
                    Serial.println("  check nothing ELSE is connected to the pin - a second driver");
                    Serial.println("  wins over a pull-up and over an open-collector sensor both.");
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

    void sampleFast(const Channel channel, const uint32_t windowMs) {
        // Bounded hard. This blocks, and the watchdog bites at 4 s.
        const uint32_t window = windowMs > 500 ? 500 : windowMs;

        const Sensor& s = at(channel);
        const uint8_t pin = s.pin;
        const uint32_t pulsesBefore = pulseCount(channel);

        Serial.print("sampling ");
        Serial.print(s.name);
        Serial.print(" on pin ");
        Serial.print(pin);
        Serial.print(" as fast as possible for ");
        Serial.print(window);
        Serial.println(" ms...");

        uint32_t samples = 0;
        uint32_t lowSamples = 0;
        uint32_t transitions = 0;

        const uint32_t began = millis();
        int last = fastRead(pin);

        while (millis() - began < window) {
            const int now = fastRead(pin);

            ++samples;

            if (now == LOW) {
                ++lowSamples;
            }

            if (now != last) {
                ++transitions;
                last = now;
            }
        }

        const uint32_t pulsesDuring = pulseCount(channel) - pulsesBefore;
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

    void sampleFast(const uint32_t windowMs) {
        sampleFast(kActiveChannel, windowMs);
    }

    void printStatus(const Channel channel) {
        Sensor& s = at(channel);
        const uint32_t counted = snapshotPulses(s);

        // Padded to a fixed width so the two channels line up under each
        // other. Ragged columns on a two-line readout are exactly where a
        // misread happens at the bench.
        char head[48];
        snprintf(head, sizeof(head), "  flow %s (pin %u)%s", s.name, static_cast<unsigned>(s.pin),
                 channel == kActiveChannel ? " ACTIVE" : "");
        Serial.print(head);

        for (size_t i = strlen(head); i < 30; ++i) {
            Serial.print(' ');
        }

        Serial.print(": ");
        Serial.print(mlFromPulses(s, counted), 2);
        Serial.print(" ml   rate ");
        Serial.print(s.currentRateMlPerSec, 2);
        Serial.print(" ml/s   ");
        Serial.println(isFlowing(channel) ? "FLOWING" : "idle");

        Serial.print("    pulses=");
        Serial.print(counted);
        Serial.print("  rejected=");
        Serial.print(rejectedCount(channel));
        Serial.print("  calibration=");
        Serial.print(s.calibrationPulsesPerLitre);
        Serial.print(" pulses/litre");

        if (s.calibrationPulsesPerLitre == s.compiledPulsesPerLitre) {
            // Not "uncalibrated" for Flow2. 49,162 is the DM60's datasheet
            // resolution for this exact part number, quoted to +/-2%, so it is
            // a real figure rather than a placeholder. Saying otherwise would
            // invite someone to distrust a number that is good enough to use.
            // Flow1's 1,965 is better still - it was measured here.
            Serial.println("  (compiled default; 'flowcal' to beat it gravimetrically)");
        }
        else {
            Serial.println();
        }
    }

    void printStatus() {
        printStatus(Channel::Flow1);
        printStatus(Channel::Flow2);
    }

} // namespace flow
