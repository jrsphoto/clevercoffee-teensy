/**
 * @file TempSensorMAX31865.cpp
 *
 * @brief Handler for a PT100 / PT1000 RTD via a MAX31865 amplifier (SPI)
 */

#include "TempSensorMAX31865.h"
#include "Logger.h"
#include <Arduino.h>

TempSensorMAX31865::TempSensorMAX31865(const int csPin, const float rNominal, const float rRef, const int wires) :
    rNominal_(rNominal), rRef_(rRef) {
    // Hardware SPI: the chip rides the shared bus (started in main.cpp with this
    // board's custom pins) and is selected by its own chip-select, so it coexists
    // cleanly with the SPI display and the other RTD.
    rtd_ = new Adafruit_MAX31865(csPin);

    const max31865_numwires_t wireMode = (wires == 2) ? MAX31865_2WIRE : (wires == 4) ? MAX31865_4WIRE : MAX31865_3WIRE;
    rtd_->begin(wireMode);
}

bool TempSensorMAX31865::sample_temperature(double& temperature) const {
    // temperature() triggers a conversion and applies the Callendar-Van Dusen fit
    const float temp = rtd_->temperature(rNominal_, rRef_);

    // Check the fault register after the read; a set bit means the value is suspect
    const uint8_t fault = rtd_->readFault();

    if (fault) {
        // Report EVERY set bit, not just the first match. A real miswiring sets
        // several at once and the most diagnostic bit is rarely the highest one:
        // 0x58 for example is "below low threshold" PLUS both FORCE- open bits,
        // and it is the FORCE- bits that tell you what is actually wrong.
        //
        // Bit meanings are from the MAX31865 datasheet fault register (0x07),
        // which is worth preferring over the library's constant names - e.g.
        // REFINHIGH is the datasheet's "REFIN- < 0.85 x VBIAS (FORCE- open)".
        if (fault & MAX31865_FAULT_HIGHTHRESH) {
            LOG(WARNING, "MAX31865: RTD resistance above high threshold");
        }

        if (fault & MAX31865_FAULT_LOWTHRESH) {
            LOG(WARNING, "MAX31865: RTD resistance below low threshold");
        }

        if (fault & MAX31865_FAULT_REFINLOW) {
            LOG(WARNING, "MAX31865: REFIN- > 0.85 x VBIAS");
        }

        if (fault & MAX31865_FAULT_REFINHIGH) {
            LOG(WARNING, "MAX31865: REFIN- < 0.85 x VBIAS - FORCE- is OPEN");
        }

        if (fault & MAX31865_FAULT_RTDINLOW) {
            LOG(WARNING, "MAX31865: RTDIN- < 0.85 x VBIAS - FORCE- is OPEN");
        }

        if (fault & MAX31865_FAULT_OVUV) {
            LOG(WARNING, "MAX31865: over/under voltage on the input");
        }

        // The two FORCE- open bits together are the signature of an incomplete
        // current path. On a 3-wire probe that is usually the board's own
        // 2/3-wire jumper not being set, rather than anything wrong at the probe.
        if ((fault & (MAX31865_FAULT_REFINHIGH | MAX31865_FAULT_RTDINLOW)) == (MAX31865_FAULT_REFINHIGH | MAX31865_FAULT_RTDINLOW)) {
            LOGF(WARNING, "MAX31865 fault 0x%02X: FORCE- open. Check the 2/3-wire jumper on the breakout, then the probe leads.", fault);
        }
        else {
            LOGF(WARNING, "MAX31865 fault 0x%02X", fault);
        }

        rtd_->clearFault();
        return false;
    }

    temperature = temp;
    return true;
}

void TempSensorMAX31865::printDiagnostics() const {
    const uint16_t raw = rtd_->readRTD();

    // The RTD register is a 15-bit ratio of RTD resistance to the board's
    // reference resistor, with the fault bit in the low position already
    // stripped by readRTD().
    const float ratio = static_cast<float>(raw) / 32768.0f;
    const float ohms = ratio * rRef_;

    Serial.print("    raw=");
    Serial.print(raw);
    Serial.print("  measured=");
    Serial.print(ohms, 2);
    Serial.print(" ohm  (Rref=");
    Serial.print(rRef_, 0);
    Serial.print(", expect ~110 ohm for a PT100 at room temperature)");

    // Classify against what a PT100 can actually do. The first version of this
    // band was far too generous and called 362 ohm "plausible" - nonsense at room
    // temperature, and it masked a real fault. A PT100 is 100 ohm at 0 C, ~110 at
    // 25 C, 138 at 100 C and ~175 at 200 C. Anything much past 200 ohm is not a
    // temperature at all, it is resistance that does not belong in the circuit.
    if (ohms < 5.0f) {
        Serial.println("  -> OPEN: no current path through the element");
    }
    else if (ohms < 90.0f) {
        Serial.println("  -> too low for a PT100: partial short, or the matched pair is on the wrong terminals");
    }
    else if (ohms <= 200.0f) {
        // Linear approximation, good to a fraction of a degree over this range.
        Serial.print("  -> plausible PT100, about ");
        Serial.print((ohms - 100.0f) / 0.385f, 0);
        Serial.println(" C");
    }
    else if (ohms < 800.0f) {
        Serial.println("  -> TOO HIGH for a PT100. That is extra series resistance:");
        Serial.println("       a loose screw terminal or a strand barely touching. Re-seat the leads.");
    }
    else if (ohms < 1400.0f) {
        Serial.println("  -> looks like a PT1000 probe on a PT100 board (wrong Rref)");
    }
    else {
        Serial.println("  -> far too high: open circuit or badly wrong wiring");
    }

    const uint8_t fault = rtd_->readFault();
    Serial.print("    fault register = 0x");
    Serial.println(fault, HEX);
    rtd_->clearFault();
}
