/**
 * @file TempSensorMAX31865.h
 *
 * @brief Handler for a PT100 / PT1000 RTD via a MAX31865 amplifier (SPI)
 */

#pragma once

#include "TempSensor.h"
#include <Adafruit_MAX31865.h>

class TempSensorMAX31865 : public TempSensor {
    public:
        /**
         * @brief Construct an RTD sensor on a (software) SPI bus.
         *
         * Two of these can share the same MOSI/MISO/SCLK pins and differ only by
         * their chip-select, which is how the boiler and group sensors ride one bus.
         *
         * @param csPin    chip-select GPIO for this sensor
         * @param mosiPin  shared SPI data-out GPIO
         * @param misoPin  shared SPI data-in GPIO
         * @param sclkPin  shared SPI clock GPIO
         * @param rNominal RTD nominal resistance (100 for PT100, 1000 for PT1000)
         * @param rRef     reference resistor on the MAX31865 board (~430 for PT100, ~4300 for PT1000)
         * @param wires    number of RTD wires: 2, 3 or 4
         */
        TempSensorMAX31865(int csPin, int mosiPin, int misoPin, int sclkPin, float rNominal = 100.0f, float rRef = 430.0f, int wires = 3);

    protected:
        bool sample_temperature(double& temperature) const override;

    private:
        Adafruit_MAX31865* rtd_;
        float rNominal_;
        float rRef_;
};
