/**
 * @file link.h
 *
 * @brief The head's end of the Teensy link (UART0, IO43/IO44)
 *
 * This side is a listener and a petitioner. It renders whatever the Teensy
 * reports and asks for things it would like done; the Teensy decides. Nothing
 * here may assume a request was honoured - the next telemetry frame is the only
 * statement of truth about the machine.
 *
 * If the cable is unplugged the machine keeps running perfectly. All that
 * happens here is that the readout goes stale, and the UI should say so rather
 * than keep showing confident numbers.
 */

#pragma once

#include <stdint.h>

namespace machinelink {

    /** Everything the head knows about the machine. Refreshed by telemetry. */
    struct State {
        float boilerC = 0.0f;
        float groupC = 0.0f;
        bool boilerOk = false;
        bool groupOk = false;
        float flowMlPerSec = 0.0f;
        float volumeMl = 0.0f;
        long encoder = 0;
        char status[16] = "?";
        char fault[24] = "?";
    };

    /** @brief Open the UART to the Teensy. */
    void init();

    /** @brief Service the link. Call every loop pass. */
    void loop();

    /** @brief Latest machine state. Only meaningful while isConnected(). */
    const State& state();

    /**
     * @brief Has a valid telemetry frame arrived recently?
     *
     * False means the readout is stale - it does NOT mean the machine has a
     * problem. The Teensy may be running a shot perfectly with this cable out.
     */
    bool isConnected();

    /** @brief Milliseconds since the last valid frame. */
    uint32_t msSinceLastFrame();

    /** @brief Valid frames received. */
    uint32_t framesReceived();

    /** @brief Frames rejected for a bad checksum. Climbing = electrical noise. */
    uint32_t badChecksums();

    /** @brief Ask the Teensy to do something. It may refuse. */
    void sendCommand(const char* body);

} // namespace machinelink
