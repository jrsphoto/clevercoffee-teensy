/**
 * @file TempSensorMAX31865.cpp
 *
 * @brief Handler for a PT100 / PT1000 RTD via a MAX31865 amplifier (SPI)
 */

#include "TempSensorMAX31865.h"
#include "Logger.h"

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
        // Decode rather than print a bare hex code. 0x18 (both FORCE- open bits)
        // is by far the most common and simply means nothing is connected, which
        // is worth saying in words at 2am.
        const char* meaning = "unknown";

        if (fault & MAX31865_FAULT_HIGHTHRESH) {
            meaning = "RTD resistance above high threshold";
        }
        else if (fault & MAX31865_FAULT_LOWTHRESH) {
            meaning = "RTD resistance below low threshold";
        }
        else if (fault & (MAX31865_FAULT_REFINLOW | MAX31865_FAULT_RTDINLOW)) {
            meaning = "open circuit - probe not connected or a broken lead";
        }
        else if (fault & MAX31865_FAULT_REFINHIGH) {
            meaning = "REFIN- too high - check the reference resistor / wiring";
        }
        else if (fault & MAX31865_FAULT_OVUV) {
            meaning = "over/under voltage on the input";
        }

        LOGF(WARNING, "MAX31865 fault 0x%02X: %s", fault, meaning);
        rtd_->clearFault();
        return false;
    }

    temperature = temp;
    return true;
}
