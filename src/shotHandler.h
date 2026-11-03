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
 *   Idle       valve closed, pump stopped
 *   PreInfuse  valve open, pump at a low speed - wets the puck gently
 *   Soak       valve open, pump stopped - lets the puck absorb and swell
 *   Brew       valve open, pump at brew speed - the extraction
 *   Finished   valve closed, pump stopped; drops back to Idle shortly after
 *
 * Pre-infusion and soak are each skipped if their duration is zero, so a plain
 * shot is the same code path with two stages of length nothing rather than a
 * separate branch.
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
        Preinfusion,
        PreinfusionPause,
        Running,
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

    /** @brief Current stage. */
    State state();

    /** @brief Human-readable stage name. */
    const char* stateName();

    /** @brief True unless Idle or Finished. */
    bool isRunning();

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
