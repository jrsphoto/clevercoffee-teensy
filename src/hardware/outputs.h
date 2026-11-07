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
 * Teensy pin HIGH = load ON, which is why all three are HIGH_TRIGGER. Each ULN
 * input in use has a 10k pulldown holding it low while the Teensy pins are
 * floating during reset.
 *
 * THE FULL CHAIN, read out of the schematic rather than assumed (2026-11-05,
 * after John corrected a signal swap on the ULN inputs). Worth having written
 * down, because the channel numbers are not in the order you would guess and a
 * rotation here would mean a heater pin opening a solenoid:
 *
 *   Teensy D2 -> net HEATER -> U2 I3 -> U2 O3 -> net HTR_RL  -> HEATER_RL1 (SSR)
 *   Teensy D3 -> net FILL   -> U2 I2 -> U2 O2 -> net FILL_RL -> BLR_FILL1  (fill)
 *   Teensy D5 -> net GROUP  -> U2 I1 -> U2 O1 -> net GRP_RL  -> GRP_SOL1   (group)
 *
 * Pulldowns: R17 on HEATER, R16 on FILL, R15 on GROUP. ULN channels 4-7 are
 * unused and marked no-connect at both ends.
 *
 * The pump is not on the ULN at all - see the Channel note below.
 */

#pragma once

#include <stdint.h>

namespace outputs {

    /**
     * @brief The relay-driven actuator channels.
     *
     * Order matches the ULN2003 inputs on the schematic to make reading the
     * board and reading this file the same exercise.
     *
     * THE PUMP IS NOT HERE. It is a brushed DC motor on a DRV8871 driver,
     * driven by hardware PWM from pumpHandler, because the whole point of it is
     * proportional speed - pre-infusion, pressure ramps, declining profiles.
     * The board originally had a "Pmode" jumper so pin 9 could instead feed a
     * ULN channel as a plain on/off pump relay; that has been removed, since
     * this machine will only ever run a DC pump. Pin 9 now goes only to the
     * 2-pin PUMP_PWM1 header, and the ULN has three channels in use, not four.
     *
     * Keeping the pump out of this list entirely is deliberate: two code paths
     * must never both drive pin 9, and the surest way to guarantee that is for
     * only one of them to know the pin exists.
     */
    enum class Channel : uint8_t {
        Heater = 0, // D2 -> ULN I3/O3 -> heater SSR
        Valve,      // D5 -> ULN I1/O1 -> 3-way group solenoid
        Fill,       // D3 -> ULN I2/O2 -> inlet fill solenoid
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
