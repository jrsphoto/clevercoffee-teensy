/**
 * @file shotHandler.cpp
 *
 * @brief The brew sequence
 */

#include "shotHandler.h"

#include <Arduino.h>

#include "encoderHandler.h"
#include "flowHandler.h"
#include "hardware/outputs.h"
#include "pumpHandler.h"
#include "safetyHandler.h"
#include "switchHandler.h"
#include "weightHandler.h"

namespace shot {

    namespace {

        State current = State::Idle;

        uint32_t shotStartedMs = 0;
        uint32_t stageStartedMs = 0;
        uint32_t finishedAtMs = 0;

        double weightAtStart = 0.0;
        double volumeAtStart = 0.0;

        const char* stopReason = "never run";

        /**
         * The profiles. Deliberately plain data: these will come from the
         * config system once it spans both processors, and nothing here should
         * make that harder.
         */
        Profile profiles[kProfileCount] = {
            {kDefaultPreInfuseMs, kDefaultPauseMs, kDefaultPreInfusePercent, kDefaultBrewPercent,
             kDefaultTargetGrams, kDefaultTargetMl, 0x0040FF, "espresso"},
            {1000, 0, 40.0, 80.0, 60.0, 0.0, 0x00FFA0, "lungo"},
        };

        uint8_t active = 0;

        // The hard time limit is NOT per profile. It is a safety backstop, not
        // a brew setting, and giving each profile its own would make it look
        // like something to tune.
        uint32_t maxBrewMs = kDefaultMaxBrewMs;

        /** The selected profile. A function rather than a reference because
         *  `active` changes, and rather than macros because this is not the
         *  file to be clever in. */
        Profile& P() {
            return profiles[active];
        }

        bool switchControl = true;

        /**
         * Upstream's brewSwitchWasOff, same job and same name in spirit.
         *
         * A shot runs ONCE per press. Without this, holding the brew switch
         * down would restart the sequence the instant it finished, forever.
         * The switch must be seen released before another shot can begin.
         */
        bool switchWasReleased = true;

        /** @brief Valve and pump to their resting state. */
        void allOff() {
            pump::stop();
            outputs::set(outputs::Channel::Valve, false);
        }

        /** @brief Weight delivered so far, or a negative number if unknown. */
        double deliveredGrams() {
            if (!weight::healthy()) {
                return -1.0;
            }

            return weight::grams() - weightAtStart;
        }

        double deliveredMl() {
            return flow::totalMl() - volumeAtStart;
        }

        void enter(const State next) {
            current = next;
            stageStartedMs = millis();
        }

        /**
         * @brief Has the shot met a stop condition?
         *
         * Checked only while extracting. The time limit is last so the more
         * informative reasons win when several are true at once, but it is the
         * only one that cannot be prevented by a failed sensor.
         */
        bool shouldStop(const char** why) {
            if (P().targetGrams > 0.0) {
                if (const double g = deliveredGrams(); g >= 0.0 && g >= P().targetGrams) {
                    *why = "target weight";

                    return true;
                }
            }

            if (P().targetMl > 0.0 && deliveredMl() >= P().targetMl) {
                *why = "target volume";

                return true;
            }

            // THE BACKSTOP. No health check in front of it, because there is no
            // sensor to fail - just the clock. Everything above can be defeated
            // by a broken scale or a jammed flow turbine; this cannot.
            if ((millis() - shotStartedMs) >= maxBrewMs) {
                *why = "MAXIMUM TIME - check the scale and flow sensor";

                return true;
            }

            return false;
        }

    } // namespace

    void init() {
        current = State::Idle;
        allOff();

        Serial.print("shot: ready. pre-infuse ");
        Serial.print(P().preInfuseMs);
        Serial.print(" ms at ");
        Serial.print(P().preInfusePercent, 0);
        Serial.print("%, soak ");
        Serial.print(P().pauseMs);
        Serial.print(" ms, brew at ");
        Serial.print(P().brewPercent, 0);
        Serial.print("%, stop at ");
        Serial.print(P().targetGrams, 1);
        Serial.print(" g / max ");
        Serial.print(maxBrewMs / 1000);
        Serial.println(" s");
    }

    bool start() {
        if (isRunning()) {
            Serial.println("shot: already running");

            return false;
        }

        if (safety::isFaulted()) {
            Serial.print("shot: REFUSED - fault latched (");
            Serial.print(safetyFaultName(safety::latchedFault));
            Serial.println(")");

            return false;
        }

        // Baselines, so the shot measures what IT delivered rather than
        // whatever the totals happened to be beforehand.
        weightAtStart = weight::healthy() ? weight::grams() : 0.0;
        volumeAtStart = flow::totalMl();

        shotStartedMs = millis();
        stopReason = "running";

        enter(P().preInfuseMs > 0 ? State::Preinfusion : State::Running);

        Serial.println("shot: START");

        if (!weight::healthy()) {
            Serial.println("      note: no usable scale - this shot will stop on time or volume");
        }

        return true;
    }

    void stop(const char* reason) {
        if (current == State::Idle) {
            return;
        }

        allOff();

        stopReason = reason;
        finishedAtMs = millis();
        enter(State::Finished);

        Serial.print("shot: STOPPED after ");
        Serial.print((millis() - shotStartedMs) / 1000.0, 1);
        Serial.print(" s, ");
        Serial.print(deliveredMl(), 1);
        Serial.print(" ml");

        if (const double g = deliveredGrams(); g >= 0.0) {
            Serial.print(", ");
            Serial.print(g, 1);
            Serial.print(" g");
        }

        Serial.print(" - ");
        Serial.println(reason);
    }

    void loop() {
        // --- Brew switch, mirroring upstream's brew() ------------------
        //
        // Three behaviours taken from upstream, all of which matter:
        //   a press starts a shot;
        //   the switch returning to idle ABORTS from any state;
        //   a shot runs once per press, never restarting under a held switch.
        if (switchControl) {
            const bool pressed = switches::brewRequested();

            if (!pressed) {
                switchWasReleased = true;

                // Releasing mid-shot stops it. Upstream does the same, and it
                // is the behaviour anyone operating the machine expects: let
                // go and it stops.
                if (isRunning()) {
                    stop("brew switch released");

                    return;
                }
            }
            else if (switchWasReleased && current == State::Idle) {
                switchWasReleased = false;
                start();
            }
        }

        // --- Encoder button: one press for profile 1, two for profile 2 ---
        //
        // A long press stops a running shot, which gives the knob an abort
        // that does not need the brew switch.
        if (switchControl) {
            switch (encoder::takeGesture()) {
                case encoder::Gesture::Short:
                    if (current == State::Idle) {
                        startProfile(0);
                    }
                    else {
                        stop("encoder press");
                    }

                    break;

                case encoder::Gesture::Double:
                    if (current == State::Idle) {
                        startProfile(1);
                    }

                    break;

                case encoder::Gesture::Long:
                    if (isRunning()) {
                        stop("encoder long press");
                    }

                    break;

                case encoder::Gesture::None:
                    break;
            }
        }

        // A fault ends a shot immediately, wherever it is. Checked before
        // anything else so no stage can act on a machine that has just failed.
        if (current != State::Idle && current != State::Finished && safety::isFaulted()) {
            stop("fault latched");

            return;
        }

        const uint32_t now = millis();

        switch (current) {
            case State::Idle:
                return;

            case State::Preinfusion:
                // Re-asserted every pass on purpose - see the header. If this
                // handler hangs, the pump's command expires and it stops.
                outputs::set(outputs::Channel::Valve, true);
                pump::setSpeed(P().preInfusePercent);

                if ((now - stageStartedMs) >= P().preInfuseMs) {
                    enter(P().pauseMs > 0 ? State::PreinfusionPause : State::Running);
                }

                return;

            case State::PreinfusionPause:
                outputs::set(outputs::Channel::Valve, true);
                pump::setSpeed(0.0);

                if ((now - stageStartedMs) >= P().pauseMs) {
                    enter(State::Running);
                }

                return;

            case State::Running: {
                outputs::set(outputs::Channel::Valve, true);
                pump::setSpeed(P().brewPercent);

                const char* why = nullptr;

                if (shouldStop(&why)) {
                    stop(why);
                }

                return;
            }

            case State::Finished:
                // Held briefly so the result stays on screen, then back to
                // idle. Outputs are already off; this is display state only.
                allOff();

                if ((now - finishedAtMs) >= kFinishedLingerMs) {
                    current = State::Idle;
                }

                return;
        }
    }

    void setSwitchControlEnabled(const bool enabled) {
        switchControl = enabled;

        // Re-arm, so enabling control with the switch already held does not
        // immediately fire a shot.
        switchWasReleased = !switches::brewRequested();

        Serial.print("shot: brew switch control ");
        Serial.println(enabled ? "ENABLED" : "disabled");
    }

    bool switchControlEnabled() {
        return switchControl;
    }

    bool selectProfile(const uint8_t index) {
        if (index >= kProfileCount) {
            return false;
        }

        if (isRunning()) {
            Serial.println("shot: cannot change profile during a shot");

            return false;
        }

        active = index;

        Serial.print("shot: profile ");
        Serial.print(active + 1);
        Serial.print(" - ");
        Serial.println(profiles[active].name);

        // The encoder says which profile is live, so it is visible without a
        // screen. Status colours still win - see encoderHandler.
        encoder::setProfileColor(profiles[active].colour);

        return true;
    }

    bool startProfile(const uint8_t index) {
        if (!selectProfile(index)) {
            return false;
        }

        return start();
    }

    uint8_t profileIndex() {
        return active;
    }

    const Profile& profile() {
        return profiles[active];
    }

    Profile& profileAt(const uint8_t index) {
        return profiles[index < kProfileCount ? index : 0];
    }

    State state() {
        return current;
    }

    const char* stateName() {
        switch (current) {
            case State::Idle:
                return "idle";
            case State::Preinfusion:
                return "preinfusion";
            case State::PreinfusionPause:
                return "preinfusion pause";
            case State::Running:
                return "brewing";
            case State::Finished:
                return "finished";
        }

        return "?";
    }

    bool isRunning() {
        return current == State::Preinfusion || current == State::PreinfusionPause || current == State::Running;
    }

    uint32_t elapsedMs() {
        if (current == State::Idle) {
            return 0;
        }

        return millis() - shotStartedMs;
    }

    const char* lastStopReason() {
        return stopReason;
    }

    void setPreInfuse(const uint32_t ms, const double percent) {
        P().preInfuseMs = ms;
        P().preInfusePercent = (percent < 0.0) ? 0.0 : ((percent > 100.0) ? 100.0 : percent);
    }

    void setPreinfusionPause(const uint32_t ms) {
        P().pauseMs = ms;
    }

    void setBrewPercent(const double percent) {
        P().brewPercent = (percent < 0.0) ? 0.0 : ((percent > 100.0) ? 100.0 : percent);
    }

    void setTargetGrams(const double grams) {
        P().targetGrams = (grams < 0.0) ? 0.0 : grams;
    }

    void setTargetMl(const double ml) {
        P().targetMl = (ml < 0.0) ? 0.0 : ml;
    }

    bool setMaxBrewMs(const uint32_t ms) {
        // Cannot be disabled, and cannot be set so long that it stops being a
        // backstop. This is the only stop condition with no sensor behind it,
        // so it is the only one that must never be weakened at runtime.
        if (ms < 5000 || ms > 180000) {
            Serial.println("shot: REFUSED - the time limit must be 5 to 180 s. It cannot be disabled.");

            return false;
        }

        maxBrewMs = ms;

        Serial.print("shot: maximum brew time now ");
        Serial.print(maxBrewMs / 1000);
        Serial.println(" s");

        return true;
    }

    void printStatus() {
        Serial.print("shot: ");
        Serial.print(stateName());

        if (current != State::Idle) {
            Serial.print(", ");
            Serial.print(elapsedMs() / 1000.0, 1);
            Serial.print(" s, ");
            Serial.print(deliveredMl(), 1);
            Serial.print(" ml");

            if (const double g = deliveredGrams(); g >= 0.0) {
                Serial.print(", ");
                Serial.print(g, 1);
                Serial.print(" g");
            }
        }

        Serial.println();

        Serial.print("      pre-infuse ");
        Serial.print(P().preInfuseMs);
        Serial.print(" ms @ ");
        Serial.print(P().preInfusePercent, 0);
        Serial.print("%, soak ");
        Serial.print(P().pauseMs);
        Serial.print(" ms, brew @ ");
        Serial.print(P().brewPercent, 0);
        Serial.println("%");

        Serial.print("      stop at ");

        if (P().targetGrams > 0.0) {
            Serial.print(P().targetGrams, 1);
            Serial.print(" g");
            Serial.print(weight::healthy() ? "" : " (SCALE UNUSABLE - will not trigger)");
            Serial.print(" / ");
        }

        if (P().targetMl > 0.0) {
            Serial.print(P().targetMl, 1);
            Serial.print(" ml / ");
        }

        Serial.print("max ");
        Serial.print(maxBrewMs / 1000);
        Serial.println(" s (always enforced)");

        Serial.print("      brew switch control ");
        Serial.print(switchControl ? "on" : "off");
        Serial.print(", switch currently ");
        Serial.println(switches::brewRequested() ? "PRESSED" : "released");

        Serial.print("      last stop: ");
        Serial.println(stopReason);
    }

} // namespace shot
