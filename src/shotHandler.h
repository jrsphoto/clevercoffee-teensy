/**
 * @file shotHandler.h
 *
 * @brief The brew sequence: pre-infusion, soak, extraction, and how it stops
 *
 * PHASE 3. Drives the group valve and the pump through a shot, and decides when
 * the shot is over. It does not touch the heater - the PID runs independently
 * and the only interaction between them is the load-shed interlock, which both
 * sides already enforce.
 *
 * WHY THIS IS NOT CALLED brewHandler
 *
 * Upstream's name is taken by src/brewHandler.h, the ESP32-era header still
 * included by the frozen reference build. Same situation as weightHandler. It
 * can take the upstream name when the head splits out properly in phase 6.
 *
 * THE SEQUENCE
 *
 *   Idle          valve closed, pump stopped
 *   CoolingFlush  valve open, pump running - dumps the overheated slug
 *   Recovery      valve closed, pump stopped - lets the exchanger settle
 *   PreInfuse     valve open, pump at a low speed - wets the puck gently
 *   Soak          valve open, pump stopped - lets the puck absorb and swell
 *   Brew          valve open, pump at brew speed - the extraction
 *   Finished      valve closed, pump stopped; drops back to Idle shortly after
 *
 * Every stage is skipped if its duration is zero, so a plain shot straight
 * into extraction is the same code path with several stages of length nothing
 * rather than a separate branch.
 *
 * HOW A SHOT STOPS, which is the part that matters
 *
 * Three conditions, whichever comes first:
 *
 *   by WEIGHT  when the scale reads the target. Only consulted while
 *              weight::healthy() - a broken, unplugged or mis-tared scale
 *              never satisfies it.
 *   by VOLUME  when the flow sensor has passed the target. Only consulted
 *              while the flow reading is usable.
 *   by TIME    ALWAYS. Not optional, not a setting that can be turned off.
 *
 * The time limit is the whole reason this is safe. Stopping "when the weight
 * reaches 36 g" means waiting for a number that a dead scale will never
 * produce, and waiting means the pump keeps running into a locked portafilter
 * or onto the floor. So the time limit is enforced regardless of what else is
 * configured, and it is the one stop condition with no health check in front
 * of it - there is no sensor to fail.
 *
 * A shot also aborts immediately on any latched fault. The pump and valve both
 * go off through the same path as a manual stop.
 *
 * ON RE-ASSERTING THE PUMP
 *
 * loop() re-commands the pump speed every pass rather than setting it once at
 * each stage change. That looks redundant and is not: pump commands expire
 * after 1.5 s precisely so that a wedged caller stops the pump. If this handler
 * set the speed once and then hung, the expiry would catch it - but only if the
 * handler was not also the thing refreshing it. Re-asserting from the main loop
 * means a hang here stops the pump, which is the behaviour wanted.
 *
 * MANUAL FLUSH, and why the brew switch now behaves differently
 *
 * Ported from upstream's manualFlush() in brewHandler.h. Hold the brew switch
 * past the long-press threshold and the group valve opens with the pump
 * running; release and it stops. On a heat-exchanger machine this is the flush
 * that clears the overheated slug out of the exchanger before a shot, so it
 * earns a place on the switch rather than in a menu.
 *
 * Adding it forced a change in how the switch is read, because the two
 * behaviours cannot coexist:
 *
 *   BEFORE  a shot started the instant the switch closed.
 *   NOW     the switch runs through upstream's BrewSwitchState machine. On a
 *           MOMENTARY switch a short press - released inside the 500 ms
 *           long-press threshold - starts a shot ON RELEASE, and holding past
 *           that threshold starts a flush instead. On a TOGGLE switch the
 *           level starts the shot exactly as before, and upstream hardwires
 *           longPressDetected() to false for toggles, so a toggle switch has
 *           no manual flush. That is upstream's limitation, kept deliberately.
 *
 * There is no way to have press-to-start AND hold-to-flush on one switch. The
 * firmware cannot know whether a press is about to become a hold without
 * waiting to find out. Upstream resolves that by waiting, and so does this.
 *
 * THE COOLING FLUSH, which upstream does not have at all
 *
 * This machine is a heat exchanger, and that changes what "ready" means. While
 * it idles, the water sitting in the exchanger and in the line to the group
 * creeps up towards boiler temperature - around 123 C, well past anything you
 * would brew with. The first water through the group is therefore far too hot,
 * and the fix is to dump that slug before extracting. On most machines that is
 * folklore: flush "a few seconds", or "until it stops spitting".
 *
 * Here there is a probe on the exchanger outlet line, so it can be computed
 * instead. `documents/thermal-management.md` section 6 calls this the prize,
 * and it matters more at home than in a cafe: the machine is always coming off
 * hours of idle, so every shot is a first shot and the flush is always the
 * worst case.
 *
 * WHY IT IS FEED-FORWARD AND NOT CLOSED-LOOP, which is the whole design
 *
 * The obvious implementation is to flush until the group probe reads cool.
 * That would be wrong, and it is worth being explicit about why so nobody
 * "improves" it into existence.
 *
 * The probe is a clamp-on surface sensor on the outlet line. Per the thermal
 * document it "reads close on a stagnant line, lags during flow." So during a
 * flush it is still reading hot pipe metal while cool water is already past it.
 * Waiting for that reading to come down would flush long after the job was
 * done, dumping boiler water and overcooling the group.
 *
 * So the reading is taken while the line is STAGNANT - which is exactly the
 * condition before a shot, and exactly where the probe is accurate - and used
 * to compute a duration up front. The loop is closed only as an EARLY STOP: if
 * the probe does fall to target mid-flush, stop. That can only ever shorten a
 * flush, never extend one, so a lagging sensor cannot cause harm through it.
 *
 * THE COEFFICIENTS ARE NOT CALIBRATED, and the feature is off by default
 *
 * Converting "the group is 118 C and I want 95 C" into a number of seconds
 * needs a constant that depends on this machine's exchanger volume, line
 * length and flow rate. That cannot be derived from a datasheet and there is
 * no water in the machine to measure it with.
 *
 * It is therefore NOT invented. Same refusal as the millilitres-to-grams
 * constant in TEENSY-PORT-BRIEF.md 9.1: a made-up constant presented as a
 * measurement is worse than an obviously missing one, because it looks like it
 * works. kDefaultFlushMsPerDegree is a placeholder, printStatus() says so, and
 * the whole feature defaults OFF until somebody measures it on a machine with
 * water in it.
 *
 * DIVERGENCE FROM UPSTREAM: a flush is time-capped, upstream's is not.
 *
 * Upstream's manual flush runs for exactly as long as the switch is held, with
 * the operator's finger as the only limit. That is the right shape, a
 * dead-man's switch, but it assumes the switch is telling the truth. A stuck
 * panel switch, or a signal lead chafed onto the rail inside a cabinet, is
 * indistinguishable from a finger that never lets go, and the result is a pump
 * running until somebody notices. So the cap is here for the same reason the
 * shot time limit is: it is the only stop condition with no sensor in front of
 * it. A real flush is a few seconds and never approaches it.
 */

#pragma once

#include <stdint.h>

namespace shot {

    /**
     * @enum State
     * @brief Where the shot is in its sequence.
     *
     * Names mirror upstream CleverCoffee's BrewState (brewStates.h) so the two
     * read the same. An earlier version of this file called the pause "Soak",
     * which was an invention that made the port harder to cross-reference for
     * no gain.
     */
    enum class State : uint8_t {
        Idle,

        /**
         * Leading stages, net-new and not from upstream. They come before
         * Preinfusion so the shot timer and the delivered-volume baselines can
         * be taken AFTER them - flush water is not espresso and must not count
         * towards either. See start() and beginExtraction().
         */
        CoolingFlush,
        Recovery,

        Preinfusion,
        PreinfusionPause,
        Running,
        Finished,
    };

    /**
     * @enum SwitchState
     * @brief What the brew switch is doing, as a gesture rather than a level.
     *
     * A direct port of upstream's BrewSwitchState (brewStates.h), same states
     * and same meanings, so the two state machines read alike:
     *
     *   Idle            not pressed, nothing pending
     *   Pressed         pressed, but not yet known to be short or long
     *   ShortPressed    a press that completed inside the long-press window
     *   LongPressed     held past the threshold - this is the flush gesture
     *   WaitForRelease  the gesture has been consumed; ignore until released
     *
     * WaitForRelease is what makes one press do one thing. Without it a held
     * switch would re-trigger the moment its action finished, forever.
     */
    enum class SwitchState : uint8_t {
        Idle,
        Pressed,
        ShortPressed,
        LongPressed,
        WaitForRelease,
    };

    /**
     * @enum FlushState
     * @brief Manual grouphead flush. Upstream's ManualFlushState.
     */
    enum class FlushState : uint8_t {
        Idle,
        Running,
    };

    /**
     * @enum BackflushState
     * @brief Automatic backflush cycle. Upstream's BackflushState.
     *
     *   Filling   valve open, pump running - pressurises against the blind basket
     *   Flushing  valve closed, pump stopped - dumps back through the drain
     *   Ending    the last flush of the last cycle
     *   Finished  everything off, counters reset
     */
    enum class BackflushState : uint8_t {
        Idle,
        Filling,
        Flushing,
        Ending,
        Finished,
    };

    // --- Defaults. All adjustable; none of them are safety limits except
    //     kDefaultMaxBrewMs, which is.
    constexpr uint32_t kDefaultPreInfuseMs = 3000;
    constexpr uint32_t kDefaultPauseMs = 2000;
    constexpr double kDefaultPreInfusePercent = 30.0;
    constexpr double kDefaultBrewPercent = 100.0;
    constexpr double kDefaultTargetGrams = 36.0;
    constexpr double kDefaultTargetMl = 0.0; // 0 disables the volume stop

    /**
     * Hard ceiling on a shot, always enforced.
     *
     * Sixty seconds is far longer than any espresso shot and far shorter than
     * the time it takes to make a mess. This is a backstop, not a brew setting
     * - if a shot is ending because of this, something is wrong.
     */
    constexpr uint32_t kDefaultMaxBrewMs = 60000;

    /** How long Finished is held before dropping back to Idle. */
    constexpr uint32_t kFinishedLingerMs = 3000;

    // --- Manual flush ----------------------------------------------------

    /**
     * Pump speed during a manual flush.
     *
     * A cooling flush wants volume through the exchanger, not pressure through
     * a puck, and there is no puck in the way - the portafilter is out, or
     * empty. Full speed moves the overheated slug out fastest, which is the
     * entire point of the exercise.
     */
    constexpr double kDefaultFlushPercent = 100.0;

    /**
     * Hard ceiling on a manual flush, always enforced. See the header.
     *
     * Twenty seconds is far longer than any real flush - a cooling flush on
     * this machine is a few seconds - and short enough that a stuck switch
     * cannot empty much. Like the shot limit it is a backstop, not a setting:
     * if a flush ends because of this, the switch is lying.
     */
    constexpr uint32_t kDefaultMaxFlushMs = 20000;

    // --- Backflush -------------------------------------------------------
    //
    // Upstream's numbers and limits exactly (defaults.h: BACKFLUSH_CYCLES,
    // BACKFLUSH_FILL_TIME, BACKFLUSH_FLUSH_TIME and their MIN/MAX), in
    // milliseconds instead of seconds because nothing else in this file works
    // in seconds.
    constexpr uint8_t kDefaultBackflushCycles = 5;
    constexpr uint8_t kMinBackflushCycles = 2;
    constexpr uint8_t kMaxBackflushCycles = 20;

    constexpr uint32_t kDefaultBackflushFillMs = 5000;
    constexpr uint32_t kMinBackflushFillMs = 3000;
    constexpr uint32_t kMaxBackflushFillMs = 10000;

    constexpr uint32_t kDefaultBackflushFlushMs = 10000;
    constexpr uint32_t kMinBackflushFlushMs = 5000;
    constexpr uint32_t kMaxBackflushFlushMs = 20000;

    /**
     * Pump speed during the fill stage. Upstream's pump is on/off, so full
     * speed is the faithful port.
     *
     * READ THE HARDWARE WARNING on setBackflushArmed() before changing this
     * upward thinking it will clean better. It will not; it will just raise
     * the pressure the pump dead-heads at.
     */
    constexpr double kDefaultBackflushPercent = 100.0;

    // --- Cooling flush ---------------------------------------------------

    /**
     * Group temperature at or below which no cooling flush is needed.
     *
     * Not a brew temperature. It is the temperature of the water standing in
     * the exchanger outlet line, measured stagnant, above which the first
     * water through the puck would be too hot.
     */
    constexpr double kDefaultFlushTargetC = 95.0;

    /**
     * Milliseconds of flush per degree the group is above target.
     *
     * A PLACEHOLDER. It depends on exchanger volume, line length and flow
     * rate, none of which can be looked up, and there is no water in the
     * machine to measure it with. See the header. printStatus() reports the
     * feature as uncalibrated until this is set from a real measurement, and
     * the feature is off by default.
     *
     * To calibrate, once the machine holds water: let it idle to temperature,
     * note the stagnant group reading, flush in short bursts until the group
     * settles at the target, and divide the total flush time by the original
     * excess in degrees.
     */
    constexpr uint32_t kDefaultFlushMsPerDegree = 200;

    /**
     * Hard ceiling on a cooling flush, always enforced.
     *
     * Fifteen seconds at a sensible flow is a lot of water, and far more than
     * any real cooling flush. As with every other cap here it is a backstop:
     * if a flush ends on this, the coefficient above is wrong.
     */
    constexpr uint32_t kDefaultMaxCoolingFlushMs = 15000;

    /**
     * Pause after the cooling flush, before extraction starts.
     *
     * The exchanger has just been refilled with cold mains water. This gives
     * it a moment to pick up heat and the pressure to settle, so extraction
     * does not begin on a slug that is now too COLD. Also stops the pump
     * running continuously from flush straight into pre-infusion.
     */
    constexpr uint32_t kDefaultRecoveryMs = 3000;

    /** Pump speed during a cooling flush. Volume is the point, not pressure. */
    constexpr double kDefaultCoolingFlushPercent = 100.0;

    /**
     * How long the group must have been undisturbed before the probe reading
     * is trusted enough to plan a flush from.
     *
     * This is what `thermal-management.md` means by using idle time, and the
     * role is narrower than it first looks. Idle time is NOT a second input to
     * the duration - the temperature already measures the thing that matters.
     * What idle time decides is whether the temperature can be BELIEVED.
     *
     * The probe is accurate on a stagnant line and lags during flow, so a
     * reading taken shortly after water last moved is still settling. Below
     * this threshold the flush is skipped rather than computed from a reading
     * that is in transit.
     *
     * That also happens to be the physically correct behaviour: back-to-back
     * shots need no cooling flush, because the exchanger has not had time to
     * overheat. The case this feature exists for is the machine that has sat
     * for hours, which at home is every single shot.
     *
     * Upstream tracks idle time already, in standby.h: standbyModeStartTimeMillis
     * reset by resetStandbyTimer() on activity. Standby is not ported yet, so
     * the same reset-on-activity shape is used locally here. When standby
     * lands, these should become ONE clock rather than two that disagree.
     *
     * ALSO A PLACEHOLDER, and probably badly wrong. John pointed out
     * (2026-11-06) that this machine's group head is solid copper and weighs
     * about 8 lb. The settling time of the probe reading is therefore set by
     * that copper mass, not by the small volume of water next to it, and two
     * minutes is very likely an order of magnitude short.
     *
     * Measure it: with the machine at temperature, pull a shot, then watch the
     * group reading until it stops climbing. That interval is this number.
     * Until then it carries the same uncalibrated warning as
     * kDefaultFlushMsPerDegree.
     *
     * The same copper mass has a second consequence worth checking at the same
     * time: a group that cools slowly also REHEATS the fresh water left behind
     * by a flush, so kDefaultRecoveryMs may need to be longer, and a flush may
     * be less effective than a simple model suggests.
     */
    constexpr uint32_t kDefaultMinIdleForFlushMs = 120000; // 2 minutes, UNMEASURED

    /** How many shot profiles exist. Two is what the encoder can select. */
    constexpr uint8_t kProfileCount = 2;

    /**
     * @brief A named set of shot settings.
     *
     * Two profiles, chosen from the encoder: one press for the first, two for
     * the second. Each carries its own LED colour so the machine says which is
     * selected without anyone reading a screen - the point of a knob with a
     * light in it.
     */
    struct Profile {
            uint32_t preInfuseMs;
            uint32_t pauseMs;
            double preInfusePercent;
            double brewPercent;
            double targetGrams;
            double targetMl;
            uint32_t colour; // 0xRRGGBB shown on the encoder when idle
            const char* name;
    };

    /** @brief Select a profile without starting a shot. */
    bool selectProfile(uint8_t index);

    /** @brief Select a profile and start it. */
    bool startProfile(uint8_t index);

    /** @brief Index of the selected profile. */
    uint8_t profileIndex();

    /** @brief The selected profile. */
    const Profile& profile();

    /** @brief Mutable access, for the console and later the web UI. */
    Profile& profileAt(uint8_t index);

    /** @brief Prepare. Call after outputs::init() and pump::init(). */
    void init();

    /** @brief Advance the sequence. Call every main-loop pass. */
    void loop();

    /**
     * @brief Request a shot.
     *
     * @return false if refused - already running, or a fault is latched
     */
    bool start();

    /** @brief Stop now. Valve closed, pump stopped. Always succeeds. */
    void stop(const char* reason);

    /**
     * @brief Let the brew switch start and stop shots, as upstream does.
     *
     * Upstream drives its state machine from the switch: a short press starts
     * a shot, the switch returning to idle aborts from any state, and
     * brewSwitchWasOff makes it one shot per press rather than restarting
     * while the switch is still held. All three behaviours are kept.
     */
    void setSwitchControlEnabled(bool enabled);

    /** @brief Is the brew switch allowed to start shots? */
    bool switchControlEnabled();

    // --- Manual flush ----------------------------------------------------

    /**
     * @brief Start a manual flush.
     *
     * Refused for the same reasons a shot is - a latched fault, or something
     * already running - and additionally refused if a shot is in progress,
     * because both want the same valve and pump.
     *
     * A flush started this way still stops on the time cap, but nothing else
     * will stop it, so the console form is for bench use. The switch form is
     * hold-to-run and stops when the switch is released.
     *
     * @return false if refused.
     */
    bool startFlush(const char* why);

    /** @brief Stop a manual flush. Valve closed, pump stopped. */
    void stopFlush(const char* why);

    /** @brief Current flush stage. */
    FlushState flushState();

    /** @brief True while a manual flush is running. */
    bool isFlushing();

    /** @brief Milliseconds since the flush started, 0 if not flushing. */
    uint32_t flushElapsedMs();

    /** @brief Why the last flush ended. */
    const char* lastFlushStopReason();

    /** @brief Pump speed used during a flush, 0-100. */
    void setFlushPercent(double percent);

    /**
     * @brief Set the hard flush time cap.
     *
     * Clamped for the same reason as the shot limit: it cannot be disabled,
     * because it is the only thing that stops a flush if the switch is stuck.
     */
    bool setMaxFlushMs(uint32_t ms);

    // --- Cooling flush ---------------------------------------------------

    /**
     * @brief Turn the automatic cooling flush on or off.
     *
     * Default OFF, and deliberately so: the duration coefficient is an
     * uncalibrated placeholder, so switching this on before measuring it would
     * run the pump for a length of time derived from a guess. Turn it on after
     * calibrating, not before.
     */
    void setCoolingFlushEnabled(bool enabled);

    /** @brief Is the automatic cooling flush enabled? */
    bool coolingFlushEnabled();

    /**
     * @brief How long a cooling flush would run if a shot started now, in ms.
     *
     * Zero means none is needed or none is possible. Lets the console and the
     * display show what the machine intends BEFORE it does it, which is the
     * difference between a feature and a surprise.
     */
    uint32_t plannedCoolingFlushMs();

    /** @brief Why the planned duration is what it is, for the console. */
    const char* coolingFlushPlanReason();

    /** @brief Group temperature at or below which no flush is needed. */
    bool setFlushTargetC(double degreesC);

    /** @brief Current flush target temperature. */
    double flushTargetC();

    /** @brief Milliseconds of flush per degree above target. */
    bool setFlushMsPerDegree(uint32_t ms);

    /** @brief Current duration coefficient. */
    uint32_t flushMsPerDegree();

    /** @brief True once the coefficient has been set from a real measurement. */
    bool coolingFlushCalibrated();

    /**
     * @brief Mark the coefficient as measured rather than guessed.
     *
     * Exists so printStatus() can stop shouting UNCALIBRATED, and so the
     * distinction between a measured constant and the shipped placeholder
     * survives in one obvious place instead of in somebody's memory.
     */
    void setCoolingFlushCalibrated(bool calibrated);

    /** @brief Hard cap on a cooling flush. Cannot be disabled. */
    bool setMaxCoolingFlushMs(uint32_t ms);

    /** @brief Minimum undisturbed time before the group reading is trusted. */
    void setMinIdleForFlushMs(uint32_t ms);

    /** @brief How long since water last moved through the group, in ms. */
    uint32_t idleMs();

    /** @brief Pause between the cooling flush and extraction. */
    void setRecoveryMs(uint32_t ms);

    /** @brief Pump speed during a cooling flush. */
    void setCoolingFlushPercent(double percent);

    // --- Backflush -------------------------------------------------------

    /**
     * @brief Arm or disarm backflush. Upstream's backflushOn.
     *
     * While armed, a short press on the brew switch starts a BACKFLUSH instead
     * of a shot - that is upstream's behaviour and it is kept, because it means
     * the one switch cannot do two things by accident. Disarming mid-cycle
     * stops the cycle, also as upstream does.
     *
     * HARDWARE PRECONDITION THE FIRMWARE CANNOT CHECK
     *
     * Backflushing drives the pump against a blind basket, which is a closed
     * outlet. The MG304 is a positive-displacement gear pump with no integral
     * pressure relief, so a closed outlet means pressure rises until something
     * gives. A mechanical over-pressure relief valve in the plumbing is
     * MANDATORY before this is ever run on the real machine, and as of writing
     * it is not fitted.
     *
     * There is no sensor behind this. The brew pressure sensor is not in hand,
     * so the firmware has no way to notice over-pressure and no way to verify a
     * relief valve exists. It therefore cannot protect against this and does
     * not pretend to: arming is a deliberate act, default off, and this comment
     * is the only interlock there is. Do not add a fake one.
     */
    void setBackflushArmed(bool armed);

    /** @brief Is backflush armed? */
    bool backflushArmed();

    /** @brief Start a backflush cycle. Refused unless armed and idle. */
    bool startBackflush(const char* why);

    /** @brief Stop a backflush cycle. Valve closed, pump stopped. */
    void stopBackflush(const char* why);

    /** @brief Current backflush stage. */
    BackflushState backflushState();

    /** @brief Human-readable backflush stage name. */
    const char* backflushStateName();

    /** @brief True while a backflush cycle is running. */
    bool isBackflushing();

    /** @brief Which cycle is in progress, 1-based. */
    uint8_t backflushCycle();

    /** @brief Number of cycles, clamped to upstream's 2-20. */
    bool setBackflushCycles(uint8_t cycles);

    /** @brief Fill duration in ms, clamped to upstream's 3000-10000. */
    bool setBackflushFillMs(uint32_t ms);

    /** @brief Flush duration in ms, clamped to upstream's 5000-20000. */
    bool setBackflushFlushMs(uint32_t ms);

    /** @brief Pump speed during the fill stage, 0-100. */
    void setBackflushPercent(double percent);

    /** @brief What the brew switch gesture machine currently thinks. */
    SwitchState switchState();

    /** @brief Human-readable brew switch gesture name. */
    const char* switchStateName();

    /** @brief Current stage. */
    State state();

    /** @brief Human-readable stage name. */
    const char* stateName();

    /** @brief True unless Idle or Finished. */
    bool isRunning();

    /**
     * @brief True if a shot, a flush or a backflush is running.
     *
     * The one to ask before touching the valve or pump from anywhere else,
     * rather than checking the three separately and getting it wrong once.
     */
    bool isBusy();

    /** @brief Milliseconds since the shot started. */
    uint32_t elapsedMs();

    /** @brief Why the last shot ended. */
    const char* lastStopReason();

    // --- Settings -------------------------------------------------------
    void setPreInfuse(uint32_t ms, double percent);
    void setPreinfusionPause(uint32_t ms);
    void setBrewPercent(double percent);
    void setTargetGrams(double grams);
    void setTargetMl(double ml);

    /**
     * @brief Set the hard time limit.
     *
     * Clamped: it cannot be disabled and cannot be set absurdly long, because
     * it is the only stop condition that does not depend on a sensor.
     */
    bool setMaxBrewMs(uint32_t ms);

    /** @brief Print the sequence settings and current state. */
    void printStatus();

} // namespace shot
