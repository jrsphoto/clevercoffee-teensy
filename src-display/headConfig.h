/**
 * @file headConfig.h
 *
 * @brief The head's configuration store and parameter registry
 *
 * The head holds the persistent master copy of the configuration, as upstream
 * does, because it is the board with a filesystem and a web UI. The Teensy
 * keeps a validated EEPROM cache of the control values it needs so it runs
 * correctly with this board unplugged. Full reasoning in
 * TEENSY-PORT-BRIEF.md 9.6; the parameter list itself is in headConfig.cpp.
 */

#pragma once

#include <stdint.h>

class Config;

namespace headconfig {

    /**
     * @brief Load the stored configuration and register the parameters.
     *
     * Call AFTER the filesystem is mounted - the config lives on it - and
     * after the link is open, because registering a parameter does not push
     * anything but the first edit will.
     *
     * A missing or unreadable config file is not an error: upstream's
     * Config::begin() writes a fresh one from the registered defaults, which
     * is exactly right for a new board.
     *
     * @return true if the store loaded or was created.
     */
    bool begin();

    /** @brief Service the deferred save. Call every loop pass. */
    void loop();

    /** @brief The store. Only valid after begin(). */
    Config& config();

    /** @brief Did begin() succeed? */
    bool ready();

    /**
     * @brief Push every controller-owned value to the Teensy.
     *
     * The handshake push from 9.6: the head holds the master, so when a
     * controller appears - at boot, or after it restarts - it gets told what
     * the settings actually are rather than running on whatever its EEPROM
     * happened to hold.
     *
     * Values the Teensy does not own come back "notmine" and are ignored,
     * which is why this can simply send everything rather than maintaining a
     * second list of which keys are its business.
     */
    void pushAllToController();

    /** @brief How many parameters are registered. */
    int parameterCount();

    /** @brief Settings the controller has refused since boot. */
    uint32_t refusedByController();

    /** @brief Are any config writes still waiting for the controller's answer? */
    bool hasPendingAcks();

} // namespace headconfig
