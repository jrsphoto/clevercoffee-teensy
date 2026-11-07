/**
 * @file configStore.cpp
 *
 * @brief The Teensy's cache of head-held config. See configStore.h.
 */

#include "configStore.h"

#include <Arduino.h>
#include <EEPROM.h>
#include <string.h>

#include "heaterHandler.h"
#include "pumpHandler.h"
#include "shotHandler.h"
#include "switchHandler.h"
#include "weightHandler.h"

namespace configStore {

    namespace {

        /**
         * EEPROM MAP for the whole firmware, in one place so the next person
         * does not have to grep for it:
         *
         *   0   - 63    reserved. weightHandler's comment set this aside "for
         *               whatever the config system wants"; 64 bytes turned out
         *               to be too small, so it stays unused rather than being
         *               half-used.
         *   64  - 127   scale calibration (weightHandler, kEepromAddress 64)
         *   128 - ...   this store
         *
         * Teensy 4.1 emulates EEPROM in flash, so all of it survives a
         * firmware upload - but NOT the full erase described in BENCH.md.
         */
        constexpr int kEepromAddress = 128;
        constexpr uint32_t kStoreMagic = 0x5CF61002; // "CFG 1002"
        constexpr uint16_t kStoreVersion = 1;

        /** Plenty of headroom; the table is nowhere near this. */
        constexpr uint16_t kMaxKeys = 48;

        /** How long after the last change before EEPROM is written. */
        constexpr uint32_t kSaveDelayMs = 1500;

        /**
         * One settable value.
         *
         * `apply` takes the value already converted to a double and returns
         * whether the owning setter accepted it. Returning false here is a
         * REFUSAL, not an error: it means a validating setter did its job.
         *
         * `scale` converts the head's units to the setter's. Upstream stores
         * durations in SECONDS as doubles; nearly every setter here takes
         * milliseconds. Getting that wrong by 1000x is the single easiest
         * mistake to make in this file, so the conversion is data rather than
         * something each lambda remembers to do.
         */
        struct Entry {
                const char* key;
                double scale;
                bool (*apply)(double value);
        };

        // --- Adapters -----------------------------------------------------
        //
        // Thin, because the real work and all the range checking already lives
        // in the handlers. Anything that needs more than a cast belongs in the
        // handler, not here.

        bool applySetpoint(const double v) {
            const double before = heater::setpointC();
            heater::setSetpointC(v);

            // setSetpointC does its own range check and says nothing on
            // refusal, so "did it move" is how acceptance is detected. An
            // unchanged value being re-sent is also success.
            return heater::setpointC() == v || before == v;
        }

        // The PID setter takes all three at once, so each key updates its own
        // slot and re-sends the trio. Seeded from the live tunings so a single
        // key arriving on its own cannot zero the other two.
        double pidKp = 0.0;
        double pidTn = 0.0;
        double pidTv = 0.0;
        bool pidSeeded = false;

        void seedPid() {
            if (!pidSeeded) {
                heater::tunings(&pidKp, &pidTn, &pidTv);
                pidSeeded = true;
            }
        }

        bool applyKp(const double v) {
            seedPid();
            pidKp = v;
            heater::setTunings(pidKp, pidTn, pidTv);

            return true;
        }

        bool applyTn(const double v) {
            seedPid();
            pidTn = v;
            heater::setTunings(pidKp, pidTn, pidTv);

            return true;
        }

        bool applyTv(const double v) {
            seedPid();
            pidTv = v;
            heater::setTunings(pidKp, pidTn, pidTv);

            return true;
        }

        // Pre-infusion duration and its pump speed are one setter upstream has
        // no equivalent for, because upstream's pump is on/off. Passing the
        // existing percentage keeps a duration change from resetting it.
        bool applyPreInfuseMs(const double v) {
            shot::setPreInfuse(static_cast<uint32_t>(v), shot::profile().preInfusePercent);

            return true;
        }

        bool applyPreInfusePercent(const double v) {
            shot::setPreInfuse(shot::profile().preInfuseMs, v);

            return true;
        }

        bool applyPause(const double v) {
            shot::setPreinfusionPause(static_cast<uint32_t>(v));

            return true;
        }

        bool applyBrewPercent(const double v) {
            shot::setBrewPercent(v);

            return true;
        }

        bool applyTargetGrams(const double v) {
            shot::setTargetGrams(v);

            return true;
        }

        bool applyTargetMl(const double v) {
            shot::setTargetMl(v);

            return true;
        }

        /**
         * Upstream's brew.by_time.target_time, which in this port IS the hard
         * limit rather than a separate optional stop.
         *
         * shotHandler.h explains the divergence: upstream's time stop can be
         * switched off, and here it cannot, because it is the only stop
         * condition with no sensor behind it. So the two merged into one
         * setting, and setMaxBrewMs still clamps it to 5-180 s and still
         * refuses to be disabled.
         */
        bool applyMaxBrewMs(const double v) {
            return shot::setMaxBrewMs(static_cast<uint32_t>(v));
        }

        bool applyBackflushCycles(const double v) {
            return shot::setBackflushCycles(static_cast<uint8_t>(v));
        }

        bool applyBackflushFill(const double v) {
            return shot::setBackflushFillMs(static_cast<uint32_t>(v));
        }

        bool applyBackflushFlush(const double v) {
            return shot::setBackflushFlushMs(static_cast<uint32_t>(v));
        }

        bool applyFlushPercent(const double v) {
            shot::setFlushPercent(v);

            return true;
        }

        bool applyMaxFlushMs(const double v) {
            return shot::setMaxFlushMs(static_cast<uint32_t>(v));
        }

        // --- Cooling flush. No upstream equivalent; see brief 9.5.
        bool applyCoolEnabled(const double v) {
            shot::setCoolingFlushEnabled(v != 0.0);

            return true;
        }

        bool applyCoolTarget(const double v) {
            return shot::setFlushTargetC(v);
        }

        bool applyCoolPerDeg(const double v) {
            return shot::setFlushMsPerDegree(static_cast<uint32_t>(v));
        }

        bool applyCoolCap(const double v) {
            return shot::setMaxCoolingFlushMs(static_cast<uint32_t>(v));
        }

        bool applyCoolRecovery(const double v) {
            shot::setRecoveryMs(static_cast<uint32_t>(v));

            return true;
        }

        bool applyCoolPercent(const double v) {
            shot::setCoolingFlushPercent(v);

            return true;
        }

        bool applyCoolMinIdle(const double v) {
            shot::setMinIdleForFlushMs(static_cast<uint32_t>(v));

            return true;
        }

        bool applyCoolCalibrated(const double v) {
            shot::setCoolingFlushCalibrated(v != 0.0);

            return true;
        }

        bool applyLoadShed(const double v) {
            pump::setLoadShed(v != 0.0);

            return true;
        }

        bool applyBrewSwitchType(const double v) {
            // Upstream's Switch::Type: MOMENTARY = 0, TOGGLE = 1.
            switches::setMomentary(static_cast<int>(v) == 0);

            return true;
        }

        bool applyScaleSamples(const double v) {
            weight::setSamples(static_cast<int>(v));

            return true;
        }

        /**
         * THE TABLE.
         *
         * Key names match upstream's config paths wherever an equivalent
         * setting exists, so the two stay cross-referenceable. Settings with no
         * upstream equivalent use the same dotted style under a path that makes
         * clear they are this fork's.
         *
         * `scale` converts the head's units to the setter's - 1000 wherever
         * upstream stores seconds and the setter wants milliseconds.
         */
        constexpr Entry kEntries[] = {
            // Heater and PID. Upstream key names exactly.
            {"brew.setpoint", 1.0, applySetpoint},
            {"pid.regular.kp", 1.0, applyKp},
            {"pid.regular.tn", 1.0, applyTn},
            {"pid.regular.tv", 1.0, applyTv},

            // Brew sequence. Upstream stores these in SECONDS.
            {"brew.pre_infusion.time", 1000.0, applyPreInfuseMs},
            {"brew.pre_infusion.pause", 1000.0, applyPause},
            {"brew.by_time.target_time", 1000.0, applyMaxBrewMs},
            {"brew.by_weight.target_weight", 1.0, applyTargetGrams},

            // This fork's additions to the shot: the pump has a speed.
            {"brew.pre_infusion.pump_percent", 1.0, applyPreInfusePercent},
            {"brew.pump_percent", 1.0, applyBrewPercent},
            {"brew.by_volume.target_volume", 1.0, applyTargetMl},

            // Backflush. Upstream key names; upstream stores SECONDS.
            {"backflush.cycles", 1.0, applyBackflushCycles},
            {"backflush.fill_time", 1000.0, applyBackflushFill},
            {"backflush.flush_time", 1000.0, applyBackflushFlush},

            // Manual flush. No upstream settings - upstream's manual flush has
            // nothing to configure, since it runs for as long as it is held.
            {"flush.pump_percent", 1.0, applyFlushPercent},
            {"flush.max_time", 1000.0, applyMaxFlushMs},

            // Cooling flush. Entirely this fork's; see brief 9.5.
            {"cooling_flush.enabled", 1.0, applyCoolEnabled},
            {"cooling_flush.target_temperature", 1.0, applyCoolTarget},
            {"cooling_flush.ms_per_degree", 1.0, applyCoolPerDeg},
            {"cooling_flush.max_time", 1000.0, applyCoolCap},
            {"cooling_flush.recovery_time", 1000.0, applyCoolRecovery},
            {"cooling_flush.pump_percent", 1.0, applyCoolPercent},
            {"cooling_flush.min_idle_time", 1000.0, applyCoolMinIdle},
            {"cooling_flush.calibrated", 1.0, applyCoolCalibrated},

            // Hardware. Upstream key names.
            {"hardware.switches.brew.type", 1.0, applyBrewSwitchType},
            {"hardware.sensors.scale.samples", 1.0, applyScaleSamples},

            // Load shed has no upstream equivalent - upstream has no 15 A
            // problem to solve.
            {"pump.load_shed", 1.0, applyLoadShed},
        };

        constexpr uint16_t kEntryCount = sizeof(kEntries) / sizeof(kEntries[0]);

        static_assert(kEntryCount <= kMaxKeys, "kMaxKeys too small for the table");

        // --- Persistence --------------------------------------------------

        struct Stored {
                uint32_t magic;
                uint16_t version;
                uint16_t count;

                /**
                 * Hash of every key name in the table.
                 *
                 * Values are stored by table INDEX to save space, which means a
                 * reordered or edited table would otherwise apply the wrong
                 * value to the wrong setting after a firmware update - exactly
                 * the sort of plausible-but-wrong behaviour this project
                 * refuses everywhere else. Hashing the names makes any table
                 * change invalidate the record automatically, so nobody has to
                 * remember to bump a version.
                 */
                uint32_t tableHash;

                float values[kMaxKeys];
                uint8_t present[(kMaxKeys + 7) / 8];
                uint32_t checksum;
        };

        float cached[kMaxKeys] = {};
        bool hasValue[kMaxKeys] = {};

        bool pendingSave = false;
        uint32_t lastChangeMs = 0;

        uint32_t applied = 0;
        uint32_t refused = 0;
        uint32_t unknown = 0;

        /**
         * Restored from EEPROM at boot, counted separately.
         *
         * Not folded into `applied`, because the two answer different
         * questions: `applied` is link activity, this is what was already in
         * force before any head connected. Showing only `applied` made the
         * status line read "applied=0" while two restored settings were
         * plainly in effect, which is the kind of diagnostic that gets
         * believed and then misleads.
         */
        uint16_t restoredAtBoot = 0;

        uint32_t tableHash() {
            uint32_t h = 2166136261u; // FNV-1a offset basis

            for (uint16_t i = 0; i < kEntryCount; ++i) {
                for (const char* p = kEntries[i].key; *p != '\0'; ++p) {
                    h ^= static_cast<uint8_t>(*p);
                    h *= 16777619u;
                }
            }

            return h;
        }

        /** Deliberately trivial - a corruption check, not security. */
        uint32_t recordChecksum(const Stored& rec) {
            uint32_t sum = 0;
            const auto* bytes = reinterpret_cast<const uint8_t*>(&rec);

            for (size_t i = 0; i < offsetof(Stored, checksum); ++i) {
                sum = (sum << 1) ^ bytes[i];
            }

            return sum;
        }

        int indexOf(const char* key) {
            for (uint16_t i = 0; i < kEntryCount; ++i) {
                if (strcmp(kEntries[i].key, key) == 0) {
                    return static_cast<int>(i);
                }
            }

            return -1;
        }

    } // namespace

    void begin() {
        Stored rec{};
        EEPROM.get(kEepromAddress, rec);

        if (rec.magic != kStoreMagic || rec.version != kStoreVersion) {
            // Blank or foreign EEPROM. Not an error - this is a new board, or
            // one that has had the full erase from BENCH.md. Defaults stand.
            Serial.println("config: no stored settings, using compiled defaults");

            return;
        }

        if (rec.checksum != recordChecksum(rec)) {
            Serial.println("config: stored settings FAILED checksum, ignoring them");

            return;
        }

        if (rec.tableHash != tableHash() || rec.count != kEntryCount) {
            // The table changed since this was written, so stored indices no
            // longer mean what they meant. Discard rather than misapply.
            Serial.println("config: stored settings are from a different firmware layout, ignoring them");

            return;
        }

        uint16_t restored = 0;
        uint16_t rejected = 0;

        for (uint16_t i = 0; i < kEntryCount; ++i) {
            if ((rec.present[i / 8] & (1 << (i % 8))) == 0) {
                continue;
            }

            const double scaled = static_cast<double>(rec.values[i]) * kEntries[i].scale;

            // Still routed through the validating setter. A stored value is no
            // more trusted than one off the wire: EEPROM can rot, and a limit
            // that tightened in a firmware update must win over what was saved
            // under the old one.
            if (kEntries[i].apply(scaled)) {
                cached[i] = rec.values[i];
                hasValue[i] = true;
                restored++;
            }
            else {
                rejected++;
            }
        }

        restoredAtBoot = restored;

        Serial.print("config: restored ");
        Serial.print(restored);
        Serial.print(" of ");
        Serial.print(kEntryCount);
        Serial.print(" settings from EEPROM");

        if (rejected > 0) {
            Serial.print(", ");
            Serial.print(rejected);
            Serial.print(" REFUSED by their setter (limits may have tightened)");
        }

        Serial.println();
    }

    bool apply(const char* key, const char* value, const char** why) {
        const int idx = indexOf(key);

        if (idx < 0) {
            // Not ours. The head pushes its whole config, which legitimately
            // includes WiFi, MQTT and display settings. Counted, not refused.
            unknown++;
            *why = "notmine";

            return false;
        }

        const double raw = atof(value);
        const double scaled = raw * kEntries[idx].scale;

        if (!kEntries[idx].apply(scaled)) {
            refused++;
            *why = "refused";

            return false;
        }

        cached[idx] = static_cast<float>(raw);
        hasValue[idx] = true;
        applied++;

        pendingSave = true;
        lastChangeMs = millis();

        *why = "ok";

        return true;
    }

    void save() {
        if (!pendingSave) {
            return;
        }

        Stored rec{};
        rec.magic = kStoreMagic;
        rec.version = kStoreVersion;
        rec.count = kEntryCount;
        rec.tableHash = tableHash();

        for (uint16_t i = 0; i < kEntryCount; ++i) {
            rec.values[i] = cached[i];

            if (hasValue[i]) {
                rec.present[i / 8] |= (1 << (i % 8));
            }
        }

        rec.checksum = recordChecksum(rec);

        // EEPROM.put only rewrites bytes that differ, so an unchanged record
        // costs no flash wear.
        EEPROM.put(kEepromAddress, rec);
        pendingSave = false;

        Serial.println("config: settings written to EEPROM");
    }

    bool dirty() {
        return pendingSave;
    }

    void loop() {
        // Coalesce a burst into one write. The head pushes its whole config on
        // handshake, so without this a single reconnect would mean dozens of
        // EEPROM writes. Same idea as upstream's ParameterRegistry autosave.
        if (pendingSave && (millis() - lastChangeMs) >= kSaveDelayMs) {
            save();
        }
    }

    uint16_t ownedKeyCount() {
        return kEntryCount;
    }

    uint32_t appliedCount() {
        return applied;
    }

    uint32_t refusedCount() {
        return refused;
    }

    uint32_t unknownCount() {
        return unknown;
    }

    void printStatus() {
        Serial.print("config: ");
        Serial.print(kEntryCount);
        Serial.print(" keys owned, restored at boot=");
        Serial.print(restoredAtBoot);
        Serial.print(" applied=");
        Serial.print(applied);
        Serial.print(" refused=");
        Serial.print(refused);
        Serial.print(" notmine=");
        Serial.print(unknown);
        Serial.println(pendingSave ? "  (unsaved changes)" : "");

        Serial.println("  key                                  stored    scale");

        for (uint16_t i = 0; i < kEntryCount; ++i) {
            Serial.print("  ");
            Serial.print(kEntries[i].key);

            for (size_t pad = strlen(kEntries[i].key); pad < 36; ++pad) {
                Serial.print(' ');
            }

            if (hasValue[i]) {
                Serial.print(cached[i], 3);
            }
            else {
                Serial.print("-");
            }

            Serial.print("     x");
            Serial.println(kEntries[i].scale, 0);
        }

        Serial.println("  'stored' is what the head last sent, in the head's units.");
        Serial.println("  A dash means never set - the compiled default is in force.");
    }

    void clearStored() {
        Stored rec{};
        rec.magic = 0; // anything but kStoreMagic
        EEPROM.put(kEepromAddress, rec);

        for (uint16_t i = 0; i < kEntryCount; ++i) {
            hasValue[i] = false;
            cached[i] = 0.0f;
        }

        pendingSave = false;

        Serial.println("config: stored settings cleared - next boot uses compiled defaults");
        Serial.println("        (values currently in force are unchanged until reboot)");
    }

} // namespace configStore
