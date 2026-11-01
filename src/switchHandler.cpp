/**
 * @file switchHandler.cpp
 *
 * @brief The hardwired brew switch on the machine's panel (Teensy pin 24)
 */

#include "switchHandler.h"

#include <Arduino.h>

#include "hardware/IOSwitch.h"
#include "hardware/pinmapping.h"

namespace switches {

    namespace {

        /**
         * The switch itself. Heap-allocated and rebuilt when the sense or type
         * changes, matching how upstream CleverCoffee constructs its switches -
         * IOSwitch takes its mode at construction and has no setter.
         */
        IOSwitch* brewSwitch = nullptr;

        /**
         * Where the idle level comes from. Defaults to the Teensy's internal
         * pull-up, because that is the only option that works on a breadboard
         * with no components fitted - R9 and JP2 are on a PCB that has not been
         * made yet. The finished board will want Pull::External.
         */
        Pull pullMode = Pull::InternalUp;

        /**
         * Which physical level means pressed. Defaults to active LOW to match
         * the internal pull-up above: the pin idles high and the switch pulls
         * it down to GND. Kept separate from the pull rather than derived from
         * it, because on the real board the sense also depends on how JP2 is
         * fitted and which rail the switch goes to.
         */
        bool senseActiveHigh = false;

        /** Spring-return rather than latching. */
        bool isMomentary = true;

        /**
         * Has the switch been seen released since boot?
         *
         * Until it has, brewRequested() answers false no matter what the switch
         * says. A panel switch that is already closed at power-on must not start
         * a shot the moment the machine is plugged in - see the header.
         */
        bool seenRelease = false;

        /** Last debounced state, for counting transitions. */
        bool lastPressed = false;
        uint32_t transitionCount = 0;

        /** Cached debounced reading, refreshed by loop(). */
        bool pressedNow = false;
        bool longNow = false;

        /** Bring-up watch window. */
        bool watching = false;
        uint32_t watchUntilMs = 0;
        uint8_t watchLastRaw = 0xFF;
        bool watchLastPressed = false;
        bool watchLastLong = false;

        void build() {
            delete brewSwitch;

            // IN_HARDWARE is a plain input with the internal pull OFF, for the
            // finished board where R9 does the pulling and an internal pull
            // would fight it. On the breadboard there is no R9, so an internal
            // pull is the only thing holding the pin at a defined level.
            //
            // NORMALLY_OPEN leaves the reading as-is; NORMALLY_CLOSED inverts
            // it, which is upstream's way of expressing an active-low switch.
            GPIOPin::Type pinType = GPIOPin::IN_HARDWARE;

            switch (pullMode) {
                case Pull::InternalUp:
                    pinType = GPIOPin::IN_PULLUP;
                    break;
                case Pull::InternalDown:
                    pinType = GPIOPin::IN_PULLDOWN;
                    break;
                case Pull::External:
                    pinType = GPIOPin::IN_HARDWARE;
                    break;
            }

            brewSwitch = new IOSwitch(PIN_BREWSWITCH,
                                      pinType,
                                      isMomentary ? Switch::MOMENTARY : Switch::TOGGLE,
                                      senseActiveHigh ? Switch::NORMALLY_OPEN : Switch::NORMALLY_CLOSED);

            // Everything learned under the previous interpretation is now
            // meaningless, including the release latch - re-earn it.
            seenRelease = false;
            lastPressed = false;
            pressedNow = false;
            longNow = false;
        }

    } // namespace

    bool init() {
        build();

        Serial.print("brew switch: pin ");
        Serial.print(PIN_BREWSWITCH);
        Serial.print(", ");
        Serial.print(pullName());
        Serial.print(", active ");
        Serial.print(senseActiveHigh ? "HIGH" : "LOW");
        Serial.print(", ");
        Serial.print(isMomentary ? "momentary" : "toggle");
        Serial.print(", raw level reads ");
        Serial.println(rawLevel() == HIGH ? "HIGH" : "LOW");

        return brewSwitch != nullptr;
    }

    void loop() {
        if (brewSwitch == nullptr) {
            return;
        }

        // isPressed() is not a pure read: it advances the debounce and
        // long-press state machines, so it must be called every pass and
        // exactly once per pass.
        pressedNow = brewSwitch->isPressed();
        longNow = brewSwitch->longPressDetected();

        if (pressedNow != lastPressed) {
            lastPressed = pressedNow;
            ++transitionCount;
        }

        // Arm on OBSERVING the switch released, not on a transition to
        // released. Waiting for a transition looks equivalent and is not: a
        // switch that is already released at boot - the normal case - never
        // transitions, so the latch would never arm and the brew switch would
        // be dead for the entire run. The property wanted is only ever "we have
        // seen this switch not-pressed at some point since boot", which this is.
        if (!pressedNow && !seenRelease) {
            seenRelease = true;

            Serial.println("brew switch: seen released - presses will now be acted on");
        }
    }

    bool brewRequested() {
        // The release latch is checked here rather than in loop() so that the
        // raw and debounced readings stay honest for diagnostics while the
        // request itself is suppressed.
        return pressedNow && seenRelease;
    }

    bool longPressed() {
        return longNow && seenRelease;
    }

    uint8_t rawLevel() {
        // Read the pin directly, bypassing debounce and sense correction. This
        // is what tells you which way JP2 is fitted.
        return digitalRead(PIN_BREWSWITCH);
    }

    bool releaseSeen() {
        return seenRelease;
    }

    uint32_t transitions() {
        return transitionCount;
    }

    void setPull(Pull newPull) {
        pullMode = newPull;
        build();
    }

    Pull pull() {
        return pullMode;
    }

    const char* pullName() {
        switch (pullMode) {
            case Pull::InternalUp:
                return "internal pull-up (switch to GND)";
            case Pull::InternalDown:
                return "internal pull-down (switch to +3.3V)";
            case Pull::External:
                return "no internal pull - external R9/JP2 (PCB only)";
        }

        return "?";
    }

    void setActiveHigh(bool activeHigh) {
        senseActiveHigh = activeHigh;
        build();
    }

    bool activeHigh() {
        return senseActiveHigh;
    }

    void setMomentary(bool momentary) {
        isMomentary = momentary;
        build();
    }

    bool momentary() {
        return isMomentary;
    }

    void startWatch(uint32_t durationMs) {
        watching = true;
        watchUntilMs = millis() + durationMs;

        // 0xFF is not a valid pin level, so the first service pass always
        // reports the starting state rather than waiting for a change.
        watchLastRaw = 0xFF;
        watchLastPressed = pressedNow;
        watchLastLong = longNow;

        Serial.print("watching the brew switch for ");
        Serial.print(durationMs / 1000);
        Serial.println("s - operate it now.");
        Serial.print("  pull is currently: ");
        Serial.println(pullName());
        Serial.println("  Short pin 24 to the rail OPPOSITE the pull to simulate a press.");
        Serial.println("  Nothing happening at all usually means the wrong 'brewpull' setting.");
    }

    void serviceWatch() {
        if (!watching) {
            return;
        }

        if (static_cast<int32_t>(millis() - watchUntilMs) >= 0) {
            watching = false;

            Serial.print("brew switch watch finished - ");
            Serial.print(transitionCount);
            Serial.println(" debounced transitions in total");
            printStatus();

            return;
        }

        const uint8_t raw = rawLevel();

        // Long-press is included in the change test on purpose: it is the one
        // transition that happens with no change in the pin level at all, so a
        // watch that only looked at raw/debounced would never show it.
        if (raw != watchLastRaw || pressedNow != watchLastPressed || longNow != watchLastLong) {
            watchLastRaw = raw;
            watchLastPressed = pressedNow;
            watchLastLong = longNow;

            Serial.print("  raw=");
            Serial.print(raw == HIGH ? "HIGH" : "LOW ");
            Serial.print("  debounced=");
            Serial.print(pressedNow ? "PRESSED " : "released");

            if (longNow) {
                Serial.print("  LONG PRESS");
            }

            if (!seenRelease) {
                Serial.print("  (ignored: no release seen yet)");
            }

            Serial.println();
        }
    }

    void printStatus() {
        Serial.print("brew switch: pin ");
        Serial.print(PIN_BREWSWITCH);
        Serial.print(" raw=");
        Serial.print(rawLevel() == HIGH ? "HIGH" : "LOW");
        Serial.print(", ");
        Serial.print(pullName());
        Serial.print(", active ");
        Serial.print(senseActiveHigh ? "HIGH" : "LOW");
        Serial.print(" -> ");
        Serial.print(pressedNow ? "PRESSED" : "released");

        if (longNow) {
            Serial.print(" (long)");
        }

        Serial.print(", ");
        Serial.print(isMomentary ? "momentary" : "toggle");
        Serial.print(", ");
        Serial.print(transitionCount);
        Serial.println(" transitions");

        if (!seenRelease) {
            Serial.println("brew switch: HAS READ AS PRESSED EVER SINCE BOOT - presses ignored.");
            Serial.println("             Either the switch really is closed, or the sense or pull");
            Serial.println("             is backwards. Try 'brewsense' / 'brewpull'.");
        }
    }

} // namespace switches
