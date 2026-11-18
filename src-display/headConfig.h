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

#include <mutex>
#include <stdint.h>

class Config;

namespace headconfig {

    /**
     * @brief The lock around the config store and the pending-ack list.
     *
     * THERE ARE TWO THREADS ON THIS BOARD AND ONLY ONE OF THEM IS loop().
     *
     * ESPAsyncWebServer does not run its request handlers from loop(). AsyncTCP
     * starts its own FreeRTOS task, "async_tcp", at priority 10 against the
     * Arduino loop task's 1, and unpinned (CONFIG_ASYNC_TCP_RUNNING_CORE is -1),
     * so on this dual-core S3 a web handler both interrupts the main loop AND
     * can genuinely run beside it on the other core.
     *
     * Three things are therefore touched from both sides at once:
     *
     *   - pendingPrevious, the std::map of values waiting for the controller's
     *     answer. A browser save fills it from the web task while the acks come
     *     back and erase from it on the loop task, typically a couple of
     *     milliseconds later - well before a form of twenty-odd fields has
     *     finished being applied. Two tasks rebalancing one red-black tree
     *     corrupts the heap.
     *
     *   - the Config store's JsonDocument. A browser save edits it from the web
     *     task while processPeriodicSave() on the loop task is walking the same
     *     document to serialise it to the filesystem.
     *
     *   - the I2C scan state in net.cpp, which the web handler writes and the
     *     controller's ack handler writes back from the loop task. Harmless by
     *     comparison - it is a few plain integers - but it is the same mistake
     *     and it is guarded by the same lock rather than left as an exception
     *     somebody has to re-derive.
     *
     * Deliberately NOT covering the link's UART. Frames cannot interleave:
     * HardwareSerial::write(buffer, size) passes the whole buffer to
     * uartWriteBuf(), which holds the core's UART mutex for the entire call,
     * and every frame in link.cpp goes out in a single write. Adding a second
     * lock there would suggest a hazard that does not exist.
     *
     * RECURSIVE so that a caller holding it can go through a parameter setter,
     * which takes it again. Hold it for the shortest stretch that is correct:
     * never across a serial write, never across a delay().
     */
    std::recursive_mutex& mutex();

    /** Shorthand for the guard. `const headconfig::Guard g(headconfig::mutex());` */
    using Guard = std::lock_guard<std::recursive_mutex>;

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
