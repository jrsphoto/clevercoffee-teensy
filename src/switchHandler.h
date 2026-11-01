/**
 * @file switchHandler.h
 *
 * @brief The hardwired brew switch on the machine's panel (Teensy pin 24)
 *
 * Phase 2 hardware bring-up: read the switch reliably and report what it is
 * doing. It deliberately does NOT start a brew - the brew state machine is
 * phase 3. What this provides is a debounced, sense-corrected, safety-gated
 * answer to "is the operator asking for a shot", which phase 3 then consults.
 *
 * Built on upstream CleverCoffee's own Switch/IOSwitch classes rather than a
 * fresh implementation, so the debounce and short/long-press behaviour match
 * upstream exactly and brewHandler.h can be ported later without reworking what
 * it reads.
 *
 * HOW THE BOARD WIRES IT, read from the schematic rather than assumed
 * (relays.kicad_sch, connector BPSW_SW1):
 *
 *   BPSW_SW1 is a 3-pin header - pin 1 signal, pin 2 +3.3 V, pin 3 GND.
 *   R9 (47 k) pulls the signal line, and JP2 - a 3-pad solder jumper - selects
 *   whether it pulls UP to +3.3 V or DOWN to GND. C5 (100 nF) decouples the
 *   3.3 V going out to the switch; there is no filter capacitor on the signal
 *   itself.
 *
 * THAT BOARD DOES NOT EXIST YET. The PCB is unfabricated, so R9, JP2 and C5 are
 * schematic-only: on the breadboard pin 24 has NO pull of any kind, and an
 * unconnected input there reads an arbitrary level that means nothing. Do not
 * read anything into what a floating pin happens to show.
 *
 * So the pull is selectable, and the default is the one that needs no parts:
 *
 *   PULL_INTERNAL_UP  breadboard default. The Teensy's own pull-up holds the pin
 *                     high, and the switch shorts pin 24 to GND. One wire, no
 *                     components, and it is the more noise-robust arrangement
 *                     anyway because the switch actively drives the asserted
 *                     level instead of letting a weak pull do it.
 *   PULL_INTERNAL_DOWN same idea with the switch going to +3.3 V.
 *   PULL_EXTERNAL     what the finished PCB wants: a plain input, internal pull
 *                     off, because R9 provides the pull. Selecting this on a
 *                     breadboard leaves the pin floating.
 *
 * THE ACTIVE LEVEL FOLLOWS FROM THE PULL, and is kept separate rather than
 * inferred, because on the real board it also depends on how JP2 is fitted and
 * which rail the switch is wired to - a physical fact that cannot be derived
 * here:
 *   pull up   + switch to GND     -> closed reads LOW  (active low)
 *   pull down + switch to +3.3 V  -> closed reads HIGH (active high)
 * In upstream's vocabulary that is the NORMALLY_CLOSED / NORMALLY_OPEN mode.
 * Settle it by operating the switch and watching, never by guessing.
 *
 * SAFETY
 *
 * This is an input and owns no actuator, so it cannot energise anything by
 * itself. Two properties still matter, and both are enforced here rather than
 * left to the caller:
 *
 *   - A BREW REQUEST IS ADVISORY. brewRequested() answering true means the
 *     operator asked, not that brewing may begin. Phase 3 must still gate it on
 *     the boiler sensor being healthy, no latched fault, water present, and so
 *     on - exactly as outputs::set() already refuses to energise while faulted.
 *
 *   - THE SWITCH MUST BE SEEN RELEASED BEFORE IT IS BELIEVED. If the panel
 *     switch is already closed at power-on - left on, or miswired, or the line
 *     shorted - honouring it immediately would start a shot the instant the
 *     machine is plugged in, with nobody's hand near it. So a press is ignored
 *     until a release has been observed at least once since boot. This is
 *     upstream's brewSwitchWasOff idea, enforced at the source instead of in
 *     the brew logic.
 *
 * A NOTE ON THE HARDWARE, for John
 *
 * R9 is 47 k where most of the other switch inputs on this board use 10 k, and
 * there is no capacitor on the signal line. A panel switch runs on a long-ish
 * lead inside a cabinet containing a pump and a 1500 W heater, and a 47 k pull
 * is weak enough to pick up noise from that. Debouncing in firmware hides short
 * glitches but cannot distinguish a noise burst that outlasts the debounce
 * window from a real press. Worth considering 10 k for R9 plus 100 nF from the
 * signal to GND; see the review notes for the exact KiCad steps.
 */

#pragma once

#include <stdint.h>

namespace switches {

    /**
     * @enum Pull
     * @brief Where the idle level comes from.
     *
     * See the header notes: the finished PCB uses an external resistor, but it
     * does not exist yet, so the breadboard needs an internal pull.
     */
    enum class Pull : uint8_t {
        InternalUp,   // Teensy pull-up; switch shorts to GND
        InternalDown, // Teensy pull-down; switch shorts to +3.3 V
        External,     // plain input; R9 on the PCB does the pulling
    };

    /** @brief Select where the idle level comes from. Recreates the switch. */
    void setPull(Pull pull);

    /** @brief Current pull setting. */
    Pull pull();

    /** @brief Human-readable name of the current pull setting. */
    const char* pullName();

    /**
     * @brief Create the brew switch and configure its pin.
     *
     * @return true if the switch object was created. There is no way to detect
     *         whether a switch is physically attached - an open input with an
     *         external pull reads as a valid, un-pressed level either way - so
     *         this failing means a software problem, not a missing switch.
     */
    bool init();

    /** @brief Poll the switch. Call every main-loop pass, for the debounce. */
    void loop();

    /**
     * @brief Is the operator asking for a shot?
     *
     * Debounced, corrected for the configured sense, and false until the switch
     * has been seen released since boot. ADVISORY ONLY - see the header.
     */
    bool brewRequested();

    /** @brief Has the switch been held long enough to count as a long press? */
    bool longPressed();

    /** @brief Raw pin level, no debounce and no sense correction. Bring-up. */
    uint8_t rawLevel();

    /**
     * @brief Has a release been observed since boot?
     *
     * False means a press is being ignored on purpose, which is worth being
     * able to see rather than wonder about.
     */
    bool releaseSeen();

    /** @brief Debounced transitions counted since boot. */
    uint32_t transitions();

    /**
     * @brief Set which physical level means "pressed".
     *
     * Must match how JP2 is fitted. Recreates the switch, which resets the
     * debounce state and the release-seen latch - deliberately, because the
     * previous readings were interpreted under the other sense.
     */
    void setActiveHigh(bool activeHigh);

    /** @brief True if a HIGH on the pin means pressed. */
    bool activeHigh();

    /**
     * @brief Set momentary (spring-return) or toggle (latching) behaviour.
     *
     * Only affects long-press reporting here; it matters much more to the brew
     * state machine in phase 3, which reads a toggle switch as a level and a
     * momentary one as a sequence of gestures.
     */
    void setMomentary(bool momentary);

    /** @brief True if configured as a momentary switch. */
    bool momentary();

    /** @brief Print switch state for the bring-up console. */
    void printStatus();

    /**
     * @brief Start watching the switch and reporting every change it sees.
     *
     * Non-blocking, like the encoder INT watch: it arms a window and returns,
     * and serviceWatch() does the reporting from the main loop. A blocking
     * watch would stop the watchdog being fed and stall the link, which for a
     * bring-up aid is an absurd price.
     *
     * Reports the RAW pin level as well as the debounced state, because the raw
     * level is what reveals how JP2 is fitted.
     *
     * @param durationMs how long to watch for
     */
    void startWatch(uint32_t durationMs);

    /** @brief Service an active watch. Call every main-loop pass. */
    void serviceWatch();

} // namespace switches
