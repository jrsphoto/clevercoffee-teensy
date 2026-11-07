/**
 * @file configStore.h
 *
 * @brief The Teensy's cache of the control settings the head holds the master of
 *
 * WHAT THIS IS, AND WHAT IT DELIBERATELY IS NOT
 *
 * The ESP32-S3 head holds the master configuration, persisted as JSON on its
 * filesystem, exactly as upstream CleverCoffee does. The reasoning is in
 * TEENSY-PORT-BRIEF.md 9.6 and was agreed rather than assumed: the head is the
 * board with a filesystem and a web UI, and upstream's config system is built
 * around having one.
 *
 * This file is the OTHER half. It is a cache, not a second master:
 *
 *   - It receives values pushed over the link and applies them.
 *   - It persists them in the Teensy's EEPROM so the machine boots and runs
 *     correctly with no head attached, which is the whole point of the split.
 *   - It holds NO schema. No display names, no help text, no sections, no
 *     ordering. Those live in the head's ParameterRegistry because they exist
 *     only to be rendered in a browser, and duplicating them here would create
 *     two things to keep in step.
 *
 * WHY A TABLE OF SETTERS RATHER THAN A BLOB OF VARIABLES
 *
 * Every key routes through the setter the console already uses. That is not
 * tidiness, it is the safety story: `shot::setMaxBrewMs` refuses anything
 * outside 5 to 180 seconds and cannot be disabled, `heater::setSetpointC`
 * refuses anything near the over-temperature cutoff, and so on. If the link
 * wrote variables directly, all of that would be bypassed by the one caller
 * most likely to be wrong - a web form on a device across a wire.
 *
 * So the rule is absolute: THE LINK MAY ONLY REACH A SETTING THROUGH ITS
 * VALIDATING SETTER. A wrong UI, a corrupted frame that still checksums, or a
 * future head with a bug must be unable to do anything a person at the panel
 * could not do.
 *
 * An unknown key is not an error. The head's config legitimately holds settings
 * that are nothing to do with control - WiFi credentials, display brightness,
 * MQTT topics - and it pushes its whole config on handshake rather than
 * maintaining a separate list of which keys are the Teensy's business. Unknown
 * keys are counted and ignored.
 *
 * KEY NAMES MATCH UPSTREAM'S CONFIG PATHS where an equivalent setting exists,
 * so the two remain cross-referenceable. Settings with no upstream equivalent -
 * the cooling flush, the pump's variable speed - use the same dotted style.
 */

#pragma once

#include <stdint.h>

namespace configStore {

    /**
     * @brief Load persisted values from EEPROM and apply them.
     *
     * Call AFTER every handler's init(), because applying a value calls that
     * handler's setter and those must already be ready to receive it.
     *
     * A blank or corrupt EEPROM is not an error: nothing is applied and the
     * compiled-in defaults stand. That is the state on a new board and after
     * the full erase described in BENCH.md, and in both cases the right
     * behaviour is to run on defaults rather than refuse to start.
     */
    void begin();

    /**
     * @brief Apply one setting, as received from the head.
     *
     * Routes through the owning handler's validating setter. A value the setter
     * refuses is refused here too, and reported as such.
     *
     * @param key    dotted path, e.g. "brew.pre_infusion.time"
     * @param value  the value as ASCII
     * @param why    set to a short reason, suitable for an Ack
     * @return true if a known key was applied and accepted
     */
    bool apply(const char* key, const char* value, const char** why);

    /**
     * @brief Persist the current values of every owned key to EEPROM.
     *
     * Deliberately explicit rather than on every apply(). The head pushes its
     * whole config at once on handshake, so writing per key would mean dozens
     * of EEPROM writes for one logical change, on a part with a finite write
     * endurance. commsHandler calls this once the burst has settled.
     */
    void save();

    /** @brief Are there applied values not yet written to EEPROM? */
    bool dirty();

    /**
     * @brief Service the deferred save. Call every main-loop pass.
     *
     * Writes EEPROM a short time after the last change, so a burst of config
     * becomes one write. Upstream does the same thing in ParameterRegistry
     * with its _pendingChanges / _lastChangeTime pair.
     */
    void loop();

    /** @brief How many keys this firmware owns. */
    uint16_t ownedKeyCount();

    /** @brief Keys applied since boot. */
    uint32_t appliedCount();

    /** @brief Keys refused by their setter since boot. */
    uint32_t refusedCount();

    /** @brief Keys received that this firmware does not own. Not an error. */
    uint32_t unknownCount();

    /** @brief Print every owned key and its current value, for the console. */
    void printStatus();

    /** @brief Forget the EEPROM copy, so the next boot uses defaults. */
    void clearStored();

} // namespace configStore
