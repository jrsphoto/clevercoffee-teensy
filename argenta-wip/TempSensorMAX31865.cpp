/**
 * @file TempSensorMAX31865.cpp
 *
 * @brief Handler for a PT100 / PT1000 RTD via a MAX31865 amplifier (SPI)
 */

#include "TempSensorMAX31865.h"
#include "Logger.h"

TempSensorMAX31865::TempSensorMAX31865(int csPin, int mosiPin, int misoPin, int sclkPin, float rNominal, float rRef, int wires) :
    rNominal_(rNominal), rRef_(rRef) {
    // Software SPI so each sensor is self-contained; the boiler and group
    // instances can share MOSI/MISO/SCLK and differ only by chip-select.
    rtd_ = new Adafruit_MAX31865(csPin, mosiPin, misoPin, sclkPin);

    max31865_numwires_t wireMode = (wires == 2) ? MAX31865_2WIRE : (wires == 4) ? MAX31865_4WIRE : MAX31865_3WIRE;
    rtd_->begin(wireMode);
}

bool TempSensorMAX31865::sample_temperature(double& temperature) const {
    // temperature() triggers a conversion and applies the Callendar-Van Dusen fit
    float temp = rtd_->temperature(rNominal_, rRef_);

    // Check the fault register after the read; a set bit means the value is suspect
    uint8_t fault = rtd_->readFault();

    if (fault) {
        LOGF(WARNING, "MAX31865 fault 0x%02X", fault);
        rtd_->clearFault();
        return false;
    }

    temperature = temp;
    return true;
}
