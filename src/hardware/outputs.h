/**
 * @file outputs.h
 *
 * @brief The four actuator channels, and the one gate every command passes through
 *
 * Wraps the existing CleverCoffee Relay/GPIOPin interfaces rather than replacing
 * them: Relay and GPIOPin are unchanged from upstream and port to Teensy as-is.
 * What this adds is a single choke point, so that turning something on is never
 * just a pin write scattered through a handler.
 *
 * Everything on this board is low-side driven through the ULN2003 (U2). The ULN
 * is an inverting sink: a HIGH on the Teensy input pulls its output low, sinking
 * the SSR or solenoid to ground and energizing the load. So for every channel,
 * Teensy pin HIGH = load ON, which is why all four are HIGH_TRIGGER. Each ULN
 * input also has a 10k pulldown (R15-R18) holding it low while the Teensy pins
 * are floating during reset.
 *
 * The pump is the exception worth knowing about: depending on the Pmode jumper
 * its pin either drives ULN channel 2 (plain on/off pump relay) or goes straight
 * out to John's brushed-motor driver as PWM. Same polarity either way - LOW and
 * 0 % duty both mean stopped - so on/off control is safe to use now, and
 * variable speed layers on top in phase 5 without changing the safe state.
 */

#pragma once

#include <stdint.h>

namespace outputs {

    /**
     * @brief The four actuator channels.
     *
     * Order matches the ULN2003 inputs on the schematic (I1-I4) to make reading
     * the board and reading this file the same exercise.
     */
    enum class Channel : uint8_t {
        Heater = 0, // ULN I1 -> heater SSR
        Pump,       // ULN I2 -> pump relay, or PWM to the motor driver
        Valve,      // ULN I3 -> 3-way group solenoid
        Fill,       // ULN I4 -> inlet fill solenoid
        Count
    };

    constexpr uint8_t kChannelCount = static_cast<uint8_t>(Channel::Count);

    /**
     * @brief Construct the GPIO/Relay objects and drive every channel off.
     *
     * Call after safetyInitOutputs(). That has already forced the pins safe with
     * raw writes; this builds the objects that will own them from here on, and
     * re-asserts off so there is no window between the two.
     */
    void init();

    /**
     * @brief Turn a channel on or off.
     *
     * Turning ON is refused outright when a fault is latched. Turning OFF is
     * always honoured - a safety path must never be able to fail because of a
     * safety check.
     *
     * @return true if the channel ended up in the requested state.
     */
    bool set(Channel channel, bool on);

    /**
     * @brief De-energize every channel. Always honoured, never gated.
     */
    void allOff();

    /**
     * @brief Last commanded state of a channel.
     *
     * This is what the firmware asked for, not proof of what the hardware did.
     * There is no feedback path from the SSRs or solenoids, so nothing here can
     * confirm a relay actually moved.
     */
    bool isOn(Channel channel);

    /**
     * @brief How many channels are currently commanded on.
     *
     * Used by the bring-up console to enforce one-at-a-time testing, and later
     * by the load-shed interlock.
     */
    uint8_t activeCount();

    /**
     * @brief Human-readable channel name, for the console and logs.
     */
    const char* name(Channel channel);

    /**
     * @brief The Teensy pin a channel drives, for diagnostics.
     */
    uint8_t pin(Channel channel);

} // namespace outputs
