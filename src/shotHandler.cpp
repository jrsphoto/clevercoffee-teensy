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
#include "tempHandler.h"
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
            // Order matches the home grid's top row, left to right. Each
            // stops on ONE thing and the panel labels it with that - there is
            // no pressure sensor installed and no profiling loop, so a tile
            // claiming "9 bar" would be describing something that does not
            // exist. See TEENSY-PORT-BRIEF.md 9.8.
            //
            // ristretto: short, with a bloom. Stops on weight.
            {4000, 3000, 25.0, 85.0, 20.0, 0.0, 0xFF3000, "ristretto"},
            // espresso: the daily double. Stops on weight.
            {kDefaultPreInfuseMs, kDefaultPauseMs, kDefaultPreInfusePercent, kDefaultBrewPercent,
             kDefaultTargetGrams, kDefaultTargetMl, 0x0040FF, "espresso"},
            // lungo: long, stops on VOLUME rather than weight - targetGrams 0
            // disables the weight stop, targetMl sets the volume one.
            {1000, 0, 40.0, 80.0, 0.0, 60.0, 0x00FFA0, "lungo"},
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

        // --- Manual flush ------------------------------------------------
        FlushState flush = FlushState::Idle;

        uint32_t flushStartedMs = 0;
        double flushPercent = kDefaultFlushPercent;
        uint32_t maxFlushMs = kDefaultMaxFlushMs;
        const char* flushStopReason = "never run";


        // --- Cooling flush -----------------------------------------------
        bool coolingEnabled = false;
        bool coolingCalibrated = false;

        double flushTarget = kDefaultFlushTargetC;
        uint32_t flushMsPerDeg = kDefaultFlushMsPerDegree;
        uint32_t maxCoolingMs = kDefaultMaxCoolingFlushMs;
        uint32_t recoveryMs = kDefaultRecoveryMs;
        double coolingPercent = kDefaultCoolingFlushPercent;

        uint32_t minIdleMs = kDefaultMinIdleForFlushMs;

        /** Duration decided at the start of the current shot, 0 if none. */
        uint32_t coolingPlannedMs = 0;
        const char* coolingReason = "never run";

        /**
         * When water last moved through the group.
         *
         * Upstream's idea, from standby.h: a timestamp reset on activity, with
         * idle time derived by subtraction. Reset at the END of anything that
         * moved water, because what matters to the exchanger is time since
         * flow stopped, not time since it started.
         *
         * Initialised to zero rather than millis() on purpose: at boot the
         * machine has by definition been sitting, so it should read as fully
         * idle rather than as just-used.
         */
        uint32_t lastFlowEndedMs = 0;

        void noteFlowEnded() {
            lastFlowEndedMs = millis();
        }

        /**
         * @brief How long to flush, from the stagnant group reading.
         *
         * Called once, when a shot is requested and before anything moves, so
         * the probe is reading a still line. See the header for why this is
         * not a loop.
         *
         * Returns 0 - meaning do not flush - whenever the answer is not known
         * rather than guessing. A skipped flush makes a hot shot, which is bad
         * coffee; a flush computed from a bad reading runs the pump for a time
         * nobody chose.
         */
        uint32_t planCoolingFlush(const char** why) {
            if (!coolingEnabled) {
                *why = "disabled";

                return 0;
            }

            if (!temps::groupHealthy()) {
                // No probe, no plan. This is the bring-up state and will also
                // be the state if the probe ever falls off.
                *why = "no group reading";

                return 0;
            }

            // The reading is only trustworthy on a line that has been still
            // for a while. See kDefaultMinIdleForFlushMs - this gate is what
            // idle time is actually for.
            if ((millis() - lastFlowEndedMs) < minIdleMs) {
                *why = "used too recently - group reading still settling";

                return 0;
            }

            const double excess = temps::groupC() - flushTarget;

            if (excess <= 0.0) {
                *why = "group already at or below target";

                return 0;
            }

            const uint32_t wanted = static_cast<uint32_t>(excess * flushMsPerDeg);

            if (wanted > maxCoolingMs) {
                *why = "CLAMPED to the cap - coefficient may be wrong";

                return maxCoolingMs;
            }

            *why = "computed from the group reading";

            return wanted;
        }

        // --- Backflush ---------------------------------------------------
        BackflushState backflush = BackflushState::Idle;

        bool backflushIsArmed = false;

        /**
         * When the arming happened, and how long it lasts.
         *
         * Arming EXPIRES. The same reasoning as every other expiry in this
         * firmware: the safe state is what you get by doing nothing. Armed
         * means a short press on the brew switch drives the pump into a
         * blind basket instead of brewing, and leaving that true indefinitely
         * because somebody armed it and walked away is exactly the trap the
         * two-step arming was meant to avoid.
         *
         * 30 s is deliberate: long enough to arm at the panel and walk to the
         * group, short enough that it cannot be forgotten about.
         */
        uint32_t backflushArmedAtMs = 0;
        constexpr uint32_t kBackflushArmedTimeoutMs = 30000;
        uint8_t backflushCycles = kDefaultBackflushCycles;
        uint8_t backflushCycleNow = 1;
        uint32_t backflushFillMs = kDefaultBackflushFillMs;
        uint32_t backflushFlushMs = kDefaultBackflushFlushMs;
        double backflushPercent = kDefaultBackflushPercent;
        uint32_t backflushStageStartedMs = 0;
        const char* backflushStopReason = "never run";

        // --- Brew switch gesture machine ---------------------------------
        SwitchState switchStateNow = SwitchState::Idle;

        /**
         * @brief Turn the switch level into a gesture.
         *
         * A direct port of upstream's checkBrewSwitch() (brewHandler.h), both
         * branches, with upstream's state names. The only substantive change is
         * that this reads switches::brewRequested() rather than a raw pin, so
         * the debounce, the sense correction and the released-since-boot latch
         * are already applied - see switchHandler.h.
         *
         * Upstream also blocks the switch on an empty water tank and while hot
         * water is being drawn. Neither applies here: the water-level probe
         * circuit does not exist yet, and this machine's hot water is a manual
         * tap with no firmware involvement. The dry-fire interlock still gates
         * the heater, and outputs::set() still refuses while faulted, so
         * nothing is weakened by their absence.
         */
        void updateSwitchState() {
            const bool pressed = switches::brewRequested();

            if (!switches::momentary()) {
                // TOGGLE. The switch is a level: on means brew, off means stop.
                // longPressDetected() is hardwired false for toggles upstream,
                // so there is no flush gesture on this path at all.
                switch (switchStateNow) {
                    case SwitchState::Idle:
                        if (pressed) {
                            switchStateNow = SwitchState::ShortPressed;
                        }

                        break;

                    case SwitchState::ShortPressed:
                        if (!pressed) {
                            switchStateNow = SwitchState::Idle;
                        }
                        else if (current == State::Finished) {
                            switchStateNow = SwitchState::WaitForRelease;
                        }

                        break;

                    case SwitchState::WaitForRelease:
                        if (!pressed) {
                            switchStateNow = SwitchState::Idle;
                        }

                        break;

                    default:
                        switchStateNow = SwitchState::Idle;

                        break;
                }

                return;
            }

            // MOMENTARY. A press is held in Pressed until it resolves one way
            // or the other: released inside the threshold is a short press,
            // still held past it is a long press. This waiting is why a shot
            // starts on release rather than on contact.
            switch (switchStateNow) {
                case SwitchState::Idle:
                    if (pressed) {
                        switchStateNow = SwitchState::Pressed;
                    }

                    break;

                case SwitchState::Pressed:
                    if (!pressed) {
                        switchStateNow = SwitchState::ShortPressed;
                    }
                    else if (switches::longPressed()) {
                        switchStateNow = SwitchState::LongPressed;
                    }

                    break;

                case SwitchState::ShortPressed:
                    // The switch is released by now, so a HIGH here is a NEW
                    // press, which upstream treats as "stop what is running".
                    if (pressed) {
                        switchStateNow = SwitchState::WaitForRelease;
                    }
                    else if (current == State::Finished) {
                        switchStateNow = SwitchState::WaitForRelease;
                    }

                    break;

                case SwitchState::LongPressed:
                    if (!pressed) {
                        switchStateNow = SwitchState::WaitForRelease;
                    }

                    break;

                case SwitchState::WaitForRelease:
                    if (!pressed) {
                        switchStateNow = SwitchState::Idle;
                    }

                    break;
            }
        }

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
         * @brief Start the extraction proper, taking the baselines here.
         *
         * Separated out because extraction can begin either immediately or
         * after a cooling flush and recovery, and in the second case the
         * baselines MUST be taken at this point rather than when the shot was
         * requested. Flush water is not espresso: baselining earlier would
         * count it as delivered volume and would spend part of the hard shot
         * time limit on flushing.
         */
        void beginExtraction() {
            weightAtStart = weight::healthy() ? weight::grams() : 0.0;
            volumeAtStart = flow::totalMl();
            shotStartedMs = millis();

            enter(P().preInfuseMs > 0 ? State::Preinfusion : State::Running);
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
        flush = FlushState::Idle;
        backflush = BackflushState::Idle;

        // Never armed at boot. A backflush diverts the brew switch away from
        // brewing, so coming up armed after a power cut would mean the first
        // press of the day backflushed instead of making coffee.
        backflushIsArmed = false;

        // Arm the gesture machine according to what the switch is doing RIGHT
        // NOW. Starting it in Idle with the switch already closed would make
        // the first press look like a completed gesture and could fire a shot
        // or a flush at boot. switchHandler's released-since-boot latch also
        // guards this, so this is the second of two independent defences.
        switchStateNow = switches::brewRequested() ? SwitchState::WaitForRelease : SwitchState::Idle;
        switchWasReleased = !switches::brewRequested();

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

        // Both want the valve and the pump, so they cannot overlap. Refusing
        // here rather than quietly stopping the flush, because a shot that
        // silently cancelled a flush would be a surprise mid-flush.
        if (flush != FlushState::Idle) {
            Serial.println("shot: REFUSED - a manual flush is running");

            return false;
        }

        if (safety::isFaulted()) {
            Serial.print("shot: REFUSED - fault latched (");
            Serial.print(safetyFaultName(safety::latchedFault));
            Serial.println(")");

            return false;
        }

        stopReason = "running";

        // Decide the cooling flush BEFORE anything moves, while the line is
        // still stagnant and the group probe is therefore believable.
        coolingPlannedMs = planCoolingFlush(&coolingReason);

        if (coolingPlannedMs > 0) {
            // Baselines and the shot clock are deliberately NOT set here. They
            // are taken in beginExtraction(), after the flush and recovery,
            // because flush water is not espresso: counting it would make the
            // volume target trigger instantly and would spend part of the
            // 60 s shot limit on flushing.
            enter(State::CoolingFlush);

            Serial.print("shot: COOLING FLUSH first - group ");
            Serial.print(temps::groupC(), 1);
            Serial.print(" C, target ");
            Serial.print(flushTarget, 1);
            Serial.print(" C, flushing ");
            Serial.print(coolingPlannedMs / 1000.0, 1);
            Serial.print(" s (");
            Serial.print(coolingReason);
            Serial.println(")");

            if (!coolingCalibrated) {
                Serial.println("      NOTE: the duration coefficient is an UNCALIBRATED placeholder");
            }

            return true;
        }

        beginExtraction();

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
        noteFlowEnded();

        // Captured before enter() moves the state on, so the message can say
        // whether this was a real shot or a flush that never got that far.
        const bool beforeExtraction = (current == State::CoolingFlush || current == State::Recovery);

        stopReason = reason;
        finishedAtMs = millis();
        enter(State::Finished);

        if (beforeExtraction) {
            // No shot clock and no baselines yet, so there is nothing
            // meaningful to report about volume or time - and printing
            // millis() minus an unset start would claim an absurd shot time.
            Serial.print("shot: STOPPED before extraction - ");
            Serial.println(reason);

            return;
        }

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
        // Let the arming lapse. Checked before anything else so a press
        // arriving in the same pass as the expiry is treated as unarmed.
        if (backflushIsArmed && !isBackflushing() &&
            (millis() - backflushArmedAtMs) > kBackflushArmedTimeoutMs) {
            backflushIsArmed = false;

            Serial.print("backflush: arming EXPIRED after ");
            Serial.print(kBackflushArmedTimeoutMs / 1000);
            Serial.println(" s - disarmed, the brew switch brews again");
        }

        // --- Brew switch, mirroring upstream's brew() ------------------
        //
        // Three behaviours taken from upstream, all of which matter:
        //   a press starts a shot;
        //   the switch returning to idle ABORTS from any state;
        //   a shot runs once per press, never restarting under a held switch.
        if (switchControl) {
            // EVERYTHING HERE KEYS OFF EDGES, NOT LEVELS, and that distinction
            // is the whole correctness of this block.
            //
            // The obvious way to write the abort is "if the switch is idle,
            // stop whatever is running". That is wrong, and wrong in a way
            // that looks right: the switch is idle almost all the time, so
            // anything NOT started by the switch - a console command, later a
            // tap on the display - gets killed on the very next loop pass. It
            // started and stopped inside one millisecond.
            //
            // Upstream never hit this because upstream has no way to start a
            // shot except the switch, so its gesture state is never Idle while
            // something is running. The moment a second way in exists, the
            // level test breaks and the edge test is the one that holds.
            //
            // Keying on the edge also keeps the switch's authority intact: a
            // console-started shot can still be stopped at the panel, because
            // pressing and releasing the switch produces a real transition
            // back to Idle.
            const SwitchState before = switchStateNow;
            updateSwitchState();
            const SwitchState after = switchStateNow;

            if (after == SwitchState::Idle) {
                switchWasReleased = true;
            }

            // ABORT, upstream's rule: the gesture machine RETURNING to Idle
            // ends whatever is running. On a toggle that is the switch being
            // turned off; on a momentary it is the release after a second
            // press. Applies to a backflush as well as a shot.
            if (const bool justReleased = (after == SwitchState::Idle && before != SwitchState::Idle); justReleased) {
                if (isBackflushing()) {
                    stopBackflush("brew switch released");
                }

                if (isRunning()) {
                    stop("brew switch released");

                    return;
                }
            }

            // MANUAL FLUSH is hold-to-run, so it starts when the switch
            // ENTERS the long press and stops when it LEAVES it. Both edges,
            // for the same reason as above - a console flush never enters
            // LongPressed, so it is untouched by either.
            if (after == SwitchState::LongPressed && before != SwitchState::LongPressed) {
                if (flush == FlushState::Idle && !isRunning()) {
                    startFlush("brew switch held");
                }
            }
            else if (before == SwitchState::LongPressed && after != SwitchState::LongPressed && flush == FlushState::Running) {
                stopFlush("brew switch released");
            }

            // A COMPLETED SHORT PRESS starts a shot - or a BACKFLUSH, if one
            // is armed. Upstream diverts the same gesture the same way, so the
            // one switch never has to mean two things at once: arming
            // backflush is what decides which.
            //
            // On a momentary switch this edge arrives on release, which is the
            // cost of being able to tell a press from a hold at all.
            if (after == SwitchState::ShortPressed && before != SwitchState::ShortPressed && switchWasReleased && flush == FlushState::Idle) {
                if (backflushIsArmed && backflush == BackflushState::Idle) {
                    switchWasReleased = false;
                    startBackflush("brew switch");
                }
                else if (current == State::Idle && backflush == BackflushState::Idle) {
                    switchWasReleased = false;
                    start();
                }
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

        if (flush == FlushState::Running && safety::isFaulted()) {
            stopFlush("fault latched");
        }

        if (isBackflushing() && safety::isFaulted()) {
            stopBackflush("fault latched");
        }

        // Disarming mid-cycle stops the cycle, as upstream does - its comment
        // reads "Force reset in case backflushOn is reset during backflush".
        if (isBackflushing() && !backflushIsArmed) {
            stopBackflush("disarmed");
        }

        const uint32_t now = millis();

        // --- Manual flush ------------------------------------------------
        //
        // Serviced before the shot stages below, and deliberately not inside
        // that switch: a flush is not a brew stage, it is a separate thing
        // that happens to use the same two actuators. Upstream keeps them as
        // two functions for the same reason.
        //
        // The valve and pump are re-asserted every pass, exactly as the shot
        // stages do, so that a hang here lets the pump's own command expiry
        // stop it rather than leaving it running.
        if (flush == FlushState::Running) {
            if ((now - flushStartedMs) >= maxFlushMs) {
                stopFlush("MAXIMUM TIME - check the brew switch is not stuck");
            }
            else {
                outputs::set(outputs::Channel::Valve, true);
                pump::setSpeed(flushPercent);
            }
        }

        // --- Backflush ---------------------------------------------------
        //
        // A direct port of upstream's backflush(), same states and the same
        // cycle counting. Every stage is bounded by its own timer and there is
        // no sensor anywhere in the sequence, so unlike a shot there is nothing
        // here that needs a separate time backstop: the cycle cannot run long
        // without the timers themselves being wrong. A hang in this handler is
        // caught by the pump's command expiry, which stops the pump whether or
        // not this code ever runs again.
        switch (backflush) {
            case BackflushState::Idle:
                break;

            case BackflushState::Filling:
                outputs::set(outputs::Channel::Valve, true);
                pump::setSpeed(backflushPercent);

                if ((now - backflushStageStartedMs) >= backflushFillMs) {
                    backflushStageStartedMs = now;
                    allOff();

                    // The last cycle's flush is Ending rather than Flushing, so
                    // the state machine has somewhere to go that is not "start
                    // another fill". Upstream splits it the same way.
                    backflush = (backflushCycleNow >= backflushCycles) ? BackflushState::Ending : BackflushState::Flushing;

                    Serial.println("backflush: flushing into the drip tray");
                }

                break;

            case BackflushState::Flushing:
                allOff();

                if ((now - backflushStageStartedMs) >= backflushFlushMs) {
                    backflushStageStartedMs = now;
                    backflushCycleNow++;
                    backflush = BackflushState::Filling;

                    Serial.print("backflush: cycle ");
                    Serial.print(backflushCycleNow);
                    Serial.print(" of ");
                    Serial.print(backflushCycles);
                    Serial.println(" - filling");
                }

                break;

            case BackflushState::Ending:
                allOff();

                if ((now - backflushStageStartedMs) >= backflushFlushMs) {
                    backflush = BackflushState::Finished;
                }

                break;

            case BackflushState::Finished:
                stopBackflush("completed");

                break;
        }

        switch (current) {
            case State::Idle:
                return;

            case State::CoolingFlush: {
                outputs::set(outputs::Channel::Valve, true);
                pump::setSpeed(coolingPercent);

                const uint32_t elapsed = now - stageStartedMs;

                // EARLY STOP, the only place the loop is closed. It can only
                // shorten a flush, never extend one, so the probe's lag during
                // flow cannot cause harm through it - a lagging probe simply
                // never triggers this and the planned duration stands.
                if (temps::groupHealthy() && temps::groupC() <= flushTarget) {
                    Serial.print("shot: cooling flush reached target early at ");
                    Serial.print(elapsed / 1000.0, 1);
                    Serial.println(" s");

                    allOff();
                    enter(State::Recovery);

                    return;
                }

                if (elapsed >= coolingPlannedMs) {
                    allOff();
                    enter(State::Recovery);

                    Serial.print("shot: cooling flush done, ");
                    Serial.print(elapsed / 1000.0, 1);
                    Serial.print(" s - recovering for ");
                    Serial.print(recoveryMs / 1000.0, 1);
                    Serial.println(" s");
                }

                return;
            }

            case State::Recovery:
                // Valve shut, pump stopped. Letting the exchanger pick the
                // heat back up before extraction, so the shot does not start
                // on water that is now too cold.
                allOff();

                if ((now - stageStartedMs) >= recoveryMs) {
                    beginExtraction();

                    Serial.println("shot: START (after cooling flush)");

                    if (!weight::healthy()) {
                        Serial.println("      note: no usable scale - this shot will stop on time or volume");
                    }
                }

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

    // --- Manual flush --------------------------------------------------

    bool startFlush(const char* why) {
        if (flush == FlushState::Running) {
            return false;
        }

        if (isRunning()) {
            Serial.println("flush: REFUSED - a shot is running");

            return false;
        }

        if (safety::isFaulted()) {
            Serial.print("flush: REFUSED - fault latched (");
            Serial.print(safetyFaultName(safety::latchedFault));
            Serial.println(")");

            return false;
        }

        flushStartedMs = millis();
        flush = FlushState::Running;
        flushStopReason = "running";

        // The actuators are not touched here. loop() asserts them on its next
        // pass, which keeps every valve-open and pump-on command in exactly
        // one place and means the re-assertion rule has no exception to it.
        Serial.print("flush: START - ");
        Serial.print(why);
        Serial.print(", pump ");
        Serial.print(flushPercent, 0);
        Serial.print("%, max ");
        Serial.print(maxFlushMs / 1000.0, 0);
        Serial.println(" s");

        return true;
    }

    void stopFlush(const char* why) {
        if (flush == FlushState::Idle) {
            return;
        }

        allOff();
        noteFlowEnded();

        flush = FlushState::Idle;
        flushStopReason = why;

        Serial.print("flush: STOPPED after ");
        Serial.print((millis() - flushStartedMs) / 1000.0, 1);
        Serial.print(" s - ");
        Serial.println(why);
    }

    FlushState flushState() {
        return flush;
    }

    bool isFlushing() {
        return flush == FlushState::Running;
    }

    uint32_t flushElapsedMs() {
        if (flush == FlushState::Idle) {
            return 0;
        }

        return millis() - flushStartedMs;
    }

    const char* lastFlushStopReason() {
        return flushStopReason;
    }

    void setFlushPercent(const double percent) {
        flushPercent = (percent < 0.0) ? 0.0 : ((percent > 100.0) ? 100.0 : percent);
    }

    bool setMaxFlushMs(const uint32_t ms) {
        // Same reasoning as setMaxBrewMs: this is the only thing that stops a
        // flush if the switch is stuck, so it cannot be disabled or stretched
        // until it stops being a backstop.
        if (ms < 1000 || ms > 60000) {
            Serial.println("flush: REFUSED - the time cap must be 1 to 60 s. It cannot be disabled.");

            return false;
        }

        maxFlushMs = ms;

        Serial.print("flush: maximum flush time now ");
        Serial.print(maxFlushMs / 1000.0, 1);
        Serial.println(" s");

        return true;
    }

    // --- Cooling flush --------------------------------------------------

    void setCoolingFlushEnabled(const bool enabled) {
        coolingEnabled = enabled;

        Serial.print("coolflush: ");
        Serial.println(enabled ? "ENABLED" : "disabled");

        if (enabled && !coolingCalibrated) {
            Serial.println("  WARNING: TWO numbers here are still shipped placeholders, so both the");
            Serial.println("  flush duration and the decision to flush at all come from guesses:");
            Serial.println("    perdeg  ms of flush per degree - depends on exchanger volume and flow");
            Serial.println("    minidle how long before the probe is trusted - set by the group's");
            Serial.println("            8 lb copper mass, so 2 min is probably far too short");
            Serial.println("  Measure both on a machine with water in it, then 'coolflush calibrated 1'.");
        }
    }

    bool coolingFlushEnabled() {
        return coolingEnabled;
    }

    uint32_t plannedCoolingFlushMs() {
        const char* why = "";

        return planCoolingFlush(&why);
    }

    const char* coolingFlushPlanReason() {
        const char* why = "";
        planCoolingFlush(&why);

        return why;
    }

    bool setFlushTargetC(const double degreesC) {
        // Bounded to temperatures that make sense for water about to meet
        // coffee. Outside this the setting is a typo, not an intention.
        if (degreesC < 70.0 || degreesC > 110.0) {
            Serial.println("coolflush: REFUSED - target must be 70 to 110 C");

            return false;
        }

        flushTarget = degreesC;
        Serial.print("coolflush: target now ");
        Serial.print(flushTarget, 1);
        Serial.println(" C");

        return true;
    }

    double flushTargetC() {
        return flushTarget;
    }

    bool setFlushMsPerDegree(const uint32_t ms) {
        if (ms < 10 || ms > 5000) {
            Serial.println("coolflush: REFUSED - coefficient must be 10 to 5000 ms per degree");

            return false;
        }

        flushMsPerDeg = ms;
        Serial.print("coolflush: ");
        Serial.print(flushMsPerDeg);
        Serial.println(" ms per degree above target");
        Serial.println("           still marked uncalibrated - 'coolflush calibrated 1' if measured");

        return true;
    }

    uint32_t flushMsPerDegree() {
        return flushMsPerDeg;
    }

    bool coolingFlushCalibrated() {
        return coolingCalibrated;
    }

    void setCoolingFlushCalibrated(const bool calibrated) {
        coolingCalibrated = calibrated;

        Serial.print("coolflush: coefficient marked ");
        Serial.println(calibrated ? "CALIBRATED" : "uncalibrated");
    }

    bool setMaxCoolingFlushMs(const uint32_t ms) {
        // Same rule as every other cap here: cannot be disabled, cannot be
        // stretched until it stops being a backstop.
        if (ms < 1000 || ms > 60000) {
            Serial.println("coolflush: REFUSED - the cap must be 1 to 60 s. It cannot be disabled.");

            return false;
        }

        maxCoolingMs = ms;
        Serial.print("coolflush: cap now ");
        Serial.print(maxCoolingMs / 1000.0, 1);
        Serial.println(" s");

        return true;
    }

    void setMinIdleForFlushMs(const uint32_t ms) {
        minIdleMs = ms;

        Serial.print("coolflush: group must be undisturbed for ");
        Serial.print(minIdleMs / 1000.0, 0);
        Serial.println(" s before its reading is trusted");
    }

    uint32_t idleMs() {
        return millis() - lastFlowEndedMs;
    }

    void setRecoveryMs(const uint32_t ms) {
        // Clamped only so a slip cannot leave a shot sitting in Recovery for
        // minutes. Everything is off during it, so this is about the shot not
        // appearing to hang rather than about safety.
        recoveryMs = (ms > 30000) ? 30000 : ms;

        Serial.print("coolflush: recovery pause now ");
        Serial.print(recoveryMs / 1000.0, 1);
        Serial.println(" s");
    }

    void setCoolingFlushPercent(const double percent) {
        coolingPercent = (percent < 0.0) ? 0.0 : ((percent > 100.0) ? 100.0 : percent);
    }

    // --- Backflush ------------------------------------------------------

    void setBackflushArmed(const bool armed) {
        backflushIsArmed = armed;
        backflushArmedAtMs = millis();

        if (!armed) {
            // loop() also catches this, but doing it here means disarming
            // takes effect on the spot rather than on the next pass.
            if (isBackflushing()) {
                stopBackflush("disarmed");
            }

            Serial.println("backflush: DISARMED");

            return;
        }

        Serial.print("backflush: ARMED for ");
        Serial.print(kBackflushArmedTimeoutMs / 1000);
        Serial.println(" s - a short press on the brew switch now backflushes,");
        Serial.println("           it does NOT brew. 'backflush off' to disarm sooner.");
        Serial.println();
        Serial.println("  WARNING, and the firmware cannot check any of this:");
        Serial.println("  This drives the pump against a blind basket. The MG304 is a gear pump");
        Serial.println("  with no internal pressure relief, so a closed outlet means pressure");
        Serial.println("  rises until something gives. A mechanical relief valve is MANDATORY");
        Serial.println("  and is not fitted yet. There is no brew pressure sensor either, so");
        Serial.println("  nothing here can detect over-pressure. Bench use only for now.");
    }

    bool backflushArmed() {
        return backflushIsArmed;
    }

    bool startBackflush(const char* why) {
        if (backflush != BackflushState::Idle) {
            return false;
        }

        if (!backflushIsArmed) {
            Serial.println("backflush: REFUSED - not armed. 'backflush on' first.");

            return false;
        }

        if (isRunning() || flush != FlushState::Idle) {
            Serial.println("backflush: REFUSED - a shot or flush is running");

            return false;
        }

        if (safety::isFaulted()) {
            Serial.print("backflush: REFUSED - fault latched (");
            Serial.print(safetyFaultName(safety::latchedFault));
            Serial.println(")");

            return false;
        }

        backflushCycleNow = 1;
        backflushStageStartedMs = millis();
        backflush = BackflushState::Filling;
        backflushStopReason = "running";

        Serial.print("backflush: START - ");
        Serial.print(why);
        Serial.print(", ");
        Serial.print(backflushCycles);
        Serial.print(" cycles of ");
        Serial.print(backflushFillMs / 1000.0, 1);
        Serial.print(" s fill at ");
        Serial.print(backflushPercent, 0);
        Serial.print("% / ");
        Serial.print(backflushFlushMs / 1000.0, 1);
        Serial.println(" s flush");
        Serial.println("backflush: cycle 1 - filling");

        return true;
    }

    void stopBackflush(const char* why) {
        if (backflush == BackflushState::Idle) {
            return;
        }

        allOff();
        noteFlowEnded();

        const uint8_t completed = backflushCycleNow;

        backflush = BackflushState::Idle;
        backflushCycleNow = 1;
        backflushStopReason = why;

        // A FINISHED BACKFLUSH DISARMS ITSELF.
        //
        // It did not, and that was wrong in both directions: the brew switch
        // kept backflushing instead of brewing until somebody noticed, and
        // the panel's tile had no way back - first tap arms, second runs, and
        // there was no third state that disarmed. John found both on the
        // bench 2026-11-10.
        //
        // Disarms on ANY end, not just a completed run, so there is one rule
        // rather than an exception to remember. Re-arming after an abort is
        // two taps; a machine left silently armed is the thing that drives a
        // gear pump into a blind basket when somebody expected a shot.
        backflushIsArmed = false;

        // A press is only consumed once. Upstream advances the switch to
        // WaitForRelease when a backflush finishes, and that matters here: a
        // shot gets the same treatment for free because stop() leaves the shot
        // in Finished, which updateSwitchState() watches for. A backflush has
        // no lingering Finished state to watch, so it has to say so itself -
        // otherwise the gesture machine would sit in ShortPressed with nothing
        // to move it on.
        if (switchStateNow == SwitchState::ShortPressed || switchStateNow == SwitchState::Pressed) {
            switchStateNow = SwitchState::WaitForRelease;
        }

        switchWasReleased = false;

        Serial.print("backflush: STOPPED after ");
        Serial.print(completed);
        Serial.print(" of ");
        Serial.print(backflushCycles);
        Serial.print(" cycles - ");
        Serial.println(why);
    }

    BackflushState backflushState() {
        return backflush;
    }

    const char* backflushStateName() {
        switch (backflush) {
            case BackflushState::Idle:
                return "idle";
            case BackflushState::Filling:
                return "filling";
            case BackflushState::Flushing:
                return "flushing";
            case BackflushState::Ending:
                return "ending";
            case BackflushState::Finished:
                return "finished";
        }

        return "?";
    }

    bool isBackflushing() {
        return backflush != BackflushState::Idle;
    }

    uint8_t backflushCycle() {
        return backflushCycleNow;
    }

    bool setBackflushCycles(const uint8_t cycles) {
        if (cycles < kMinBackflushCycles || cycles > kMaxBackflushCycles) {
            Serial.print("backflush: REFUSED - cycles must be ");
            Serial.print(kMinBackflushCycles);
            Serial.print(" to ");
            Serial.println(kMaxBackflushCycles);

            return false;
        }

        backflushCycles = cycles;
        Serial.println("set");

        return true;
    }

    bool setBackflushFillMs(const uint32_t ms) {
        if (ms < kMinBackflushFillMs || ms > kMaxBackflushFillMs) {
            Serial.print("backflush: REFUSED - fill must be ");
            Serial.print(kMinBackflushFillMs);
            Serial.print(" to ");
            Serial.print(kMaxBackflushFillMs);
            Serial.println(" ms");

            return false;
        }

        backflushFillMs = ms;
        Serial.println("set");

        return true;
    }

    bool setBackflushFlushMs(const uint32_t ms) {
        if (ms < kMinBackflushFlushMs || ms > kMaxBackflushFlushMs) {
            Serial.print("backflush: REFUSED - flush must be ");
            Serial.print(kMinBackflushFlushMs);
            Serial.print(" to ");
            Serial.print(kMaxBackflushFlushMs);
            Serial.println(" ms");

            return false;
        }

        backflushFlushMs = ms;
        Serial.println("set");

        return true;
    }

    void setBackflushPercent(const double percent) {
        backflushPercent = (percent < 0.0) ? 0.0 : ((percent > 100.0) ? 100.0 : percent);
    }

    SwitchState switchState() {
        return switchStateNow;
    }

    const char* switchStateName() {
        switch (switchStateNow) {
            case SwitchState::Idle:
                return "idle";
            case SwitchState::Pressed:
                return "pressed (deciding)";
            case SwitchState::ShortPressed:
                return "short press";
            case SwitchState::LongPressed:
                return "long press (flush)";
            case SwitchState::WaitForRelease:
                return "waiting for release";
        }

        return "?";
    }

    bool isBusy() {
        return isRunning() || isFlushing() || isBackflushing();
    }

    void setSwitchControlEnabled(const bool enabled) {
        switchControl = enabled;

        // Re-arm, so enabling control with the switch already held does not
        // immediately fire a shot or a flush. Same reasoning as init().
        switchStateNow = switches::brewRequested() ? SwitchState::WaitForRelease : SwitchState::Idle;
        switchWasReleased = !switches::brewRequested();

        // Disabling switch control must not abandon a flush the switch started.
        if (!enabled && flush == FlushState::Running) {
            stopFlush("switch control disabled");
        }

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
            case State::CoolingFlush:
                return "cooling flush";
            case State::Recovery:
                return "recovery";
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
        return current == State::CoolingFlush || current == State::Recovery || current == State::Preinfusion || current == State::PreinfusionPause || current == State::Running;
    }

    uint32_t elapsedMs() {
        // Zero until extraction actually begins. During the cooling flush and
        // recovery shotStartedMs has not been set yet, and subtracting from it
        // would report a shot time of however long the Teensy has been awake.
        if (current == State::Idle || current == State::CoolingFlush || current == State::Recovery || shotStartedMs == 0) {
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

        Serial.print("      cooling flush ");
        Serial.print(coolingEnabled ? "ON" : "off");

        if (!coolingCalibrated) {
            Serial.print(" - COEFFICIENT UNCALIBRATED (placeholder)");
        }

        Serial.println();

        Serial.print("        target ");
        Serial.print(flushTarget, 1);
        Serial.print(" C, ");
        Serial.print(flushMsPerDeg);
        Serial.print(" ms/degree, cap ");
        Serial.print(maxCoolingMs / 1000.0, 1);
        Serial.print(" s, recovery ");
        Serial.print(recoveryMs / 1000.0, 1);
        Serial.print(" s, pump ");
        Serial.print(coolingPercent, 0);
        Serial.println("%");

        Serial.print("        group now ");

        if (temps::groupHealthy()) {
            Serial.print(temps::groupC(), 1);
            Serial.print(" C");
        }
        else {
            Serial.print("--");
        }

        Serial.print(", undisturbed ");
        Serial.print(idleMs() / 1000.0, 0);
        Serial.print(" s of ");
        Serial.print(minIdleMs / 1000.0, 0);
        Serial.println(" s needed");

        {
            const char* why = "";
            const uint32_t planned = planCoolingFlush(&why);

            Serial.print("        if a shot started now: ");

            if (planned == 0) {
                Serial.print("no flush (");
                Serial.print(why);
                Serial.println(")");
            }
            else {
                Serial.print(planned / 1000.0, 1);
                Serial.print(" s (");
                Serial.print(why);
                Serial.println(")");
            }
        }

        Serial.print("      flush ");

        if (flush == FlushState::Running) {
            Serial.print("RUNNING, ");
            Serial.print(flushElapsedMs() / 1000.0, 1);
            Serial.print(" s");
        }
        else {
            Serial.print("idle");
        }

        Serial.print(" - pump ");
        Serial.print(flushPercent, 0);
        Serial.print("%, cap ");
        Serial.print(maxFlushMs / 1000.0, 1);
        Serial.print(" s, last stop: ");
        Serial.println(flushStopReason);

        Serial.print("      backflush ");
        Serial.print(backflushIsArmed ? "ARMED" : "disarmed");
        Serial.print(", ");
        Serial.print(backflushStateName());

        if (isBackflushing()) {
            Serial.print(" cycle ");
            Serial.print(backflushCycleNow);
            Serial.print("/");
            Serial.print(backflushCycles);
        }

        Serial.print(" - ");
        Serial.print(backflushCycles);
        Serial.print(" x (");
        Serial.print(backflushFillMs / 1000.0, 1);
        Serial.print(" s @ ");
        Serial.print(backflushPercent, 0);
        Serial.print("% + ");
        Serial.print(backflushFlushMs / 1000.0, 1);
        Serial.print(" s), last stop: ");
        Serial.println(backflushStopReason);

        if (backflushIsArmed) {
            Serial.println("      WARNING: armed - a short press backflushes, it does NOT brew.");
            Serial.println("               No relief valve fitted and no pressure sensor. Bench only.");
        }

        Serial.print("      brew switch control ");
        Serial.print(switchControl ? "on" : "off");
        Serial.print(", ");
        Serial.print(switches::momentary() ? "momentary" : "toggle");
        Serial.print(", currently ");
        Serial.print(switches::brewRequested() ? "PRESSED" : "released");
        Serial.print(", gesture ");
        Serial.println(switchStateName());

        if (!switches::momentary()) {
            Serial.println("      NOTE: a toggle switch has no manual flush - upstream hardwires");
            Serial.println("            longPressDetected() false for toggles. Use 'flush' here, or");
            Serial.println("            'brewtype' to switch to momentary.");
        }

        Serial.print("      last stop: ");
        Serial.println(stopReason);
    }

} // namespace shot
