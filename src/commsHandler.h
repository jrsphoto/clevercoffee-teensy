/**
 * @file commsHandler.h
 *
 * @brief The link to the ESP32-S3 display head (Serial1, D0/D1)
 *
 * The Teensy is authoritative. This handler:
 *   - emits telemetry at a fixed rate, whether or not anyone is listening
 *   - accepts commands, and is free to refuse them
 *
 * THE LINK IS NOT A SAFETY INPUT AND MUST NEVER BECOME ONE. There is
 * deliberately no function here that reports "link down" to the control code,
 * because nothing in the control code should ever ask. A dead display is not a
 * reason to abandon a good shot - as John put it, this is coffee, not
 * life-support. Peer presence is tracked only so the bench console can say
 * whether the cable is doing anything.
 *
 * Commands are REQUESTS. Every one of them passes through the same safety gates
 * as a local action, so the head cannot do anything over the wire that a person
 * at the machine could not do at the panel. In particular it cannot energise the
 * heater while a fault is latched, because outputs::set() refuses that
 * regardless of who asked.
 */

#pragma once

#include <stdint.h>

namespace comms {

    /**
     * @brief Open Serial1 and announce ourselves.
     */
    void init();

    /**
     * @brief Service the link. Call every main-loop pass.
     *
     * Sends telemetry on schedule and consumes anything received. Never blocks:
     * it reads only what is already buffered.
     */
    void loop();

    /** @brief True if a valid frame has arrived recently. DISPLAY ONLY. */
    bool peerPresent();

    /** @brief Frames sent since boot. */
    uint32_t framesSent();

    /** @brief Valid frames received since boot. */
    uint32_t framesReceived();

    /** @brief Frames rejected for a bad checksum. Climbing = electrical noise. */
    uint32_t badChecksums();

    /** @brief Print link state for the bench console. */
    void printStatus();

} // namespace comms
