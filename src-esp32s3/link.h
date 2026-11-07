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

        /** Brew weight. In grams only when weightCalibrated; raw counts if not. */
        float weightG = 0.0f;
        bool weightOk = false;

        /**
         * False means weightG is raw ADC counts, not grams. The display must
         * say so rather than print a number next to a "g" - an uncalibrated
         * scale shown as grams is exactly the kind of plausible wrong reading
         * that would have someone trust a dose that is nonsense.
         */
        bool weightCalibrated = false;
        /** Pump speed actually applied, 0-100%. */
        float pumpPercent = 0.0f;

        /** Boiler setpoint in Celsius, and the duty the PID is asking for. */
        float setpointC = 0.0f;
        float heaterDuty = 0.0f;
        bool heaterEnabled = false;

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

    /**
     * @brief How many times the controller has announced itself.
     *
     * The Teensy sends a Hello when it boots, so this incrementing means the
     * CONTROLLER restarted while this board kept running. Worth noticing: every
     * value on screen came from the old instance and the counters have gone
     * back to zero, so the display should repaint rather than carry stale
     * numbers across the gap.
     *
     * This is the only coupling between the two resets, and it is deliberately
     * one-way and advisory. See the note in README on why the display must
     * never be able to reset the controller.
     */
    uint32_t helloCount();

    /** @brief Ask the Teensy to do something. It may refuse. */
    void sendCommand(const char* body);

    /**
     * @brief Push one config value to the Teensy.
     *
     * This head holds the master copy of the configuration; the Teensy keeps a
     * validated EEPROM cache of the control values it needs, so it runs
     * correctly with this board unplugged. Reasoning in
     * TEENSY-PORT-BRIEF.md 9.6.
     *
     * The Teensy may REFUSE a value, and that is not a failure of the link - it
     * means a validating setter did its job. Watch the Ack: `ok=0,why=refused`
     * is the Teensy declining something out of range, and `why=notmine` simply
     * means the key is not a control setting, which is normal for WiFi, MQTT
     * and display keys.
     *
     * Values are sent as ASCII in THIS BOARD'S units, which for durations means
     * seconds, matching upstream's config. The Teensy converts.
     *
     * @param key    dotted path, e.g. "brew.pre_infusion.time"
     * @param value  the value as text
     */
    void sendConfig(const char* key, const char* value);

} // namespace machinelink
