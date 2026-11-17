/**
 * @file GPIOPin.cpp
 *
 * @brief Abstraction of a GPIO pin
 */

#include "GPIOPin.h"

GPIOPin::GPIOPin(const int pinNumber, const Type pinType) :
    pin(pinNumber), pinType(pinType) {

    setType(pinType);
}

void GPIOPin::write(const bool value) const {
    if (pinType == OUT) {
        digitalWrite(pin, value);
    }
}

int GPIOPin::read() const {
    if (pinType == IN_ANALOG) {
        return analogRead(pin);
    }

    return digitalRead(pin);
}

GPIOPin::Type GPIOPin::getType() const {
    return pinType;
}

void GPIOPin::setType(const Type pinType) const {
    switch (pinType) {
        case OUT:
            pinMode(pin, OUTPUT);
            break;
        case IN_STANDARD:
            pinMode(pin, INPUT);
            break;
        // No digitalWrite() after these two, and that is deliberate. It is an
        // AVR habit: on an old Arduino, writing HIGH to a pin set to INPUT was
        // how you switched the pull-up on. On the Teensy 4.1 the pad's pull
        // resistor is configured entirely by pinMode(), and digitalWrite() on
        // an input just writes a register that has no effect on it. Harmless,
        // but it reads as though the pull needs help to engage, which would
        // mislead anyone copying this for a new pin. Removed 2026-11-17.
        case IN_PULLUP:
            pinMode(pin, INPUT_PULLUP);
            break;
        case IN_PULLDOWN:
            pinMode(pin, INPUT_PULLDOWN);
            break;
        case IN_HARDWARE:
        case IN_ANALOG:
            pinMode(pin, INPUT);
            break;
    }
}
