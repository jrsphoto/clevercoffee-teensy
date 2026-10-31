/**
 * @file updateMode.cpp
 *
 * @brief Groundwork for flashing the Teensy over the link from the ESP32-S3
 */

#include "updateMode.h"

#include <Arduino.h>

#include "hardware/outputs.h"
#include "safetyHandler.h"

namespace updateMode {

    namespace {

        /**
         * Arbitrary but distinctive. A random-looking value matters: SNVS_LPGPR0
         * is uninitialised at cold boot, and a pattern like 0x00000001 could
         * plausibly appear by accident. This one will not.
         */
        constexpr uint32_t kMagic = 0x5FDA7E01; // "UPDATE 01"

    } // namespace

    /**
     * The flag itself.
     *
     * DMAMEM puts this in OCRAM, which the Teensy 4 startup code deliberately
     * does NOT zero - so it survives a software reset while still being lost on
     * a power cycle. That combination is exactly what is wanted: the reboot
     * carries the request across, and pulling the plug always returns the
     * machine to normal firmware.
     *
     * SNVS_LPGPR0 was the first choice and is the textbook answer, but on this
     * Teensy it is locked: writing it reads back zero. Verified on hardware
     * rather than assumed, which is the only reason it was not a silent failure
     * that rebooted straight back into normal firmware.
     */
    DMAMEM __attribute__((aligned(32))) uint32_t updateFlag;

    bool isSafe(const char** reason) {
        // Every actuator must be off. The control loop stops during an update,
        // so anything left energised stays energised, unsupervised, for the
        // whole transfer - on this machine that includes a ~1500 W element.
        if (outputs::isOn(outputs::Channel::Heater)) {
            *reason = "heater is on";

            return false;
        }

        if (outputs::isOn(outputs::Channel::Pump)) {
            *reason = "pump is on";

            return false;
        }

        if (outputs::isOn(outputs::Channel::Valve)) {
            *reason = "group valve is open";

            return false;
        }

        if (outputs::isOn(outputs::Channel::Fill)) {
            *reason = "fill valve is open";

            return false;
        }

        // A latched fault is deliberately NOT a blocker - see the header. It
        // already forces every output off, which is the condition that matters,
        // and you may be updating in order to fix it.

        *reason = "ok";

        return true;
    }

    bool requestAndReboot(const char** reason) {
        if (!isSafe(reason)) {
            return false;
        }

        // Belt and braces: force everything off before the reset, so the pins
        // are already safe rather than relying on the reset to make them so.
        safetyHardOff();
        outputs::allOff();

        Serial.println("UPDATE MODE: requested, rebooting into the flash receiver.");
        Serial.println("             Power-cycle at any point to return to normal firmware.");
        Serial.flush();
        delay(50);

        // Write the flag and VERIFY it stuck. SNVS low-power registers can be
        // locked, and a silent failure here would reboot straight back into
        // normal firmware - which is exactly what happened the first time this
        // was tested, and looked like the reset not working at all.
        updateFlag = kMagic;

        // FLUSH THE CACHE. RAM2 is cached, so without this the write sits in the
        // data cache, the reset discards it, and the next boot reads stale RAM -
        // which is exactly what happened here: the flag verified fine before the
        // reset (it was read back out of the same cache) and was gone after it.
        //
        // The whole 32-byte cache line is flushed and the variable is aligned to
        // one, so nothing shares a line with it.
        arm_dcache_flush_delete(&updateFlag, 32);

        // Verify the flag actually stuck. Note this alone was NOT enough to
        // catch the cache problem - a cached read returns the cached value.
        if (updateFlag != kMagic) {
            Serial.print("UPDATE MODE: FAILED - flag did not hold (read back 0x");
            Serial.print(updateFlag, HEX);
            Serial.println("). Not rebooting.");
            Serial.flush();

            *reason = "flag storage not writable";

            return false;
        }

        // Software reset via the ARM core. SCB_AIRCR requires the VECTKEY.
        SCB_AIRCR = 0x05FA0004;

        // Not reached.
        while (true) {
        }
    }

    bool consumeRequest() {
        // Invalidate the cache line first, so this reads actual RAM rather than
        // whatever the cache happens to hold.
        arm_dcache_delete(&updateFlag, 32);

        const bool requested = (updateFlag == kMagic);

        // Clear unconditionally, whether or not it matched. One-shot: a crash
        // inside update mode must not produce a boot loop that stays there.
        updateFlag = 0;
        arm_dcache_flush_delete(&updateFlag, 32);

        return requested;
    }

    void runLoop() {
        // Outputs are already safe. Re-assert anyway: this path must not depend
        // on anything that ran before it.
        safetyHardOff();

        Serial.println();
        Serial.println("========================================");
        Serial.println("UPDATE MODE");
        Serial.println("========================================");
        Serial.println("All outputs forced off. No watchdog armed.");
        Serial.println("No sensors, no control loop - nothing is being controlled.");
        Serial.println();
        Serial.println("The flash receiver is NOT implemented yet. This mode exists");
        Serial.println("so the safety gating and mode entry can be proven before");
        Serial.println("anything is able to write flash.");
        Serial.println();
        Serial.println("Power-cycle to return to normal firmware.");

        uint32_t lastBeat = 0;

        while (true) {
            // Deliberately no watchdog feed: the watchdog is not armed in this
            // mode, which is the entire reason for rebooting into it.
            //
            // FlasherX lands here: receive Intel HEX over Serial1, verify a CRC
            // over the whole image before committing, write to unused flash,
            // then copy over the running program and reset.

            if (const uint32_t now = millis(); now - lastBeat >= 2000) {
                lastBeat = now;

                Serial.print("update mode, idle, uptime ");
                Serial.print(now / 1000);
                Serial.println("s  (outputs off, not controlling)");
            }
        }
    }

} // namespace updateMode
