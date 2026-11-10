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

    /**
     * @brief Report RAW bytes arriving on the link UART, for a fixed window.
     *
     * Exists because the ordinary counters cannot answer the question that
     * matters when the link is one-way. `recv`, `badsum` and `malformed` all
     * count things the PARSER saw, so all three read zero both when bytes are
     * arriving and failing to frame, and when no bytes are arriving at all.
     * Those two have completely different causes and the status line cannot
     * tell them apart.
     *
     * This reads the port directly and reports the byte count plus a dump, so
     * "nothing on the wire" and "noise on the wire" stop looking identical.
     *
     * CONSUMES the bytes it sees, so a frame in flight is lost. That is
     * acceptable in a bench diagnostic and is why it is not left running.
     *
     * Feeds the watchdog while it waits - the timeout is 4 s and a useful
     * window is longer than that.
     *
     * @param windowMs how long to listen.
     */
    void sniffRaw(uint32_t windowMs);

    /**
     * @brief Look at the link RX pad directly, as a plain input.
     *
     * Answers the two questions `sniffRaw` cannot: is the pin free or is
     * something holding it, and does the other board's signal physically
     * arrive. Takes the pin off Serial1 for the duration and gives it back.
     *
     * @param windowMs how long to count edges for.
     */
    void probeRxPin(uint32_t windowMs);

} // namespace comms
