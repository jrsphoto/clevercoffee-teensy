/**
 * @file outputs.cpp
 *
 * @brief The four actuator channels, and the one gate every command passes through
 */

#include "outputs.h"

#include <Arduino.h>

#include "../safetyHandler.h"
#include "GPIOPin.h"
#include "Relay.h"
#include "pinmapping.h"

namespace outputs {

    namespace {

        /**
         * One GPIOPin and one Relay per channel, in Channel enum order so a
         * channel can index straight into these.
         *
         * All four are HIGH_TRIGGER: the ULN2003 inverts, so a HIGH on the
         * Teensy pin sinks the load to ground and energizes it.
         *
         * These are file-scope objects, so the GPIOPin constructors run during
         * static initialization - BEFORE setup() and therefore before
         * safetyInitOutputs(). That is safe, but only for a reason worth
         * writing down: the constructor calls pinMode(OUTPUT) without writing a
         * level, and the i.MX RT's GPIO data register is zero after reset, so
         * the pins drive LOW. LOW is the de-energized state for every one of
         * these low-side channels. Combined with the board's pulldowns
         * (R15-R18, and the PMP_PWM pulldown), there is no instant at which an
         * output is undefined.
         *
         * Do not add a constructor here that drives a pin HIGH, and do not
         * reorder these so that a Relay is constructed before its GPIOPin.
         */
        GPIOPin heaterPin(PIN_HEATER, GPIOPin::OUT);
        GPIOPin pumpPin(PIN_PUMP, GPIOPin::OUT);
        GPIOPin valvePin(PIN_VALVE, GPIOPin::OUT);
        GPIOPin fillPin(PIN_FILL, GPIOPin::OUT);

        Relay heaterRelay(heaterPin, Relay::HIGH_TRIGGER);
        Relay pumpRelay(pumpPin, Relay::HIGH_TRIGGER);
        Relay valveRelay(valvePin, Relay::HIGH_TRIGGER);
        Relay fillRelay(fillPin, Relay::HIGH_TRIGGER);

        Relay* const relays[kChannelCount] = {
            &heaterRelay,
            &pumpRelay,
            &valveRelay,
            &fillRelay,
        };

        const char* const names[kChannelCount] = {
            "heater",
            "pump",
            "valve",
            "fill",
        };

        const uint8_t pins[kChannelCount] = {
            PIN_HEATER,
            PIN_PUMP,
            PIN_VALVE,
            PIN_FILL,
        };

        /**
         * Last commanded state. Not a reading - there is no feedback path from
         * the SSRs or solenoids, so this records intent only.
         */
        bool commanded[kChannelCount] = {false, false, false, false};

        bool valid(const Channel channel) {
            return static_cast<uint8_t>(channel) < kChannelCount;
        }

    } // namespace

    void init() {
        // The GPIOPin constructors above already ran pinMode(OUTPUT). Drive
        // everything off through the objects so their recorded state and the
        // physical pins start out agreeing with each other.
        allOff();
    }

    bool set(const Channel channel, const bool on) {
        if (!valid(channel)) {
            return false;
        }

        const uint8_t index = static_cast<uint8_t>(channel);

        if (!on) {
            // Off is never gated. A safety path must not be able to fail
            // because of a safety check.
            relays[index]->off();
            commanded[index] = false;

            return true;
        }

        if (safety::isFaulted()) {
            // Refuse to energize anything while a fault is latched, and make
            // sure the channel really is off rather than just declining.
            relays[index]->off();
            commanded[index] = false;

            return false;
        }

        relays[index]->on();
        commanded[index] = true;

        return true;
    }

    void allOff() {
        for (uint8_t i = 0; i < kChannelCount; i++) {
            relays[i]->off();
            commanded[i] = false;
        }
    }

    bool isOn(const Channel channel) {
        return valid(channel) ? commanded[static_cast<uint8_t>(channel)] : false;
    }

    uint8_t activeCount() {
        uint8_t count = 0;

        for (uint8_t i = 0; i < kChannelCount; i++) {
            if (commanded[i]) {
                count++;
            }
        }

        return count;
    }

    const char* name(const Channel channel) {
        return valid(channel) ? names[static_cast<uint8_t>(channel)] : "?";
    }

    uint8_t pin(const Channel channel) {
        return valid(channel) ? pins[static_cast<uint8_t>(channel)] : 0;
    }

} // namespace outputs
