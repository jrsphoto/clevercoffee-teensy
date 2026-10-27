/**
 * @file TempSensorMAX31865.h
 *
 * @brief Handler for a PT100 / PT1000 RTD via a MAX31865 amplifier (SPI)
 */

#pragma once

#include "TempSensor.h"
#include <Adafruit_MAX31865.h>

class TempSensorMAX31865 final : public TempSensor {
    public:
        /**
         * @brief Construct an RTD sensor on the shared hardware SPI bus.
         *
         * Uses the global (hardware) SPI instance, which must already be started
         * with this board's custom SCK/MISO/MOSI pins (main.cpp does this once at
         * startup). Multiple sensors share that bus and differ only by their
         * chip-select, which is how the boiler and group probes ride one bus
         * alongside the SPI display.
         *
         * @param csPin    chip-select GPIO for this sensor
         * @param rNominal RTD nominal resistance (100 for PT100, 1000 for PT1000)
         * @param rRef     reference resistor on the MAX31865 board (~430 for PT100, ~4300 for PT1000)
         * @param wires    number of RTD wires: 2, 3 or 4
         */
        explicit TempSensorMAX31865(int csPin, float rNominal = 100.0f, float rRef = 430.0f, int wires = 3);

        /**
         * @brief Bring-up aid: report the resistance the chip actually measures.
         *
         * The fault register says something is wrong; this says what. An open
         * probe reads ~0 ohm, a healthy PT100 at room temperature ~110 ohm, and
         * a specific wrong value points at a specific miswiring. Prints to the
         * serial console. Not used by the control path.
         */
        void printDiagnostics() const;

        /**
         * @brief Bring-up aid: prove SPI WRITES reach the chip, not just reads.
         *
         * Reads and writes fail independently. If SDO/MISO is good but SDI/MOSI
         * is not, every register read returns believable data while every write
         * is silently discarded - so the chip never gets VBIAS enabled and never
         * measures anything, reporting raw=0 and an open-circuit fault that looks
         * exactly like a disconnected probe.
         *
         * Writes a known pattern to the threshold registers and reads it back.
         * They are ordinary read/write registers that have no effect on the
         * measurement, so this is safe to run at any time. Restores them after.
         *
         * @return true if writes land.
         */
        bool testSpiWrites() const;

        /**
         * @brief Bring-up aid: dump every MAX31865 register, read raw over SPI.
         *
         * Bypasses the library entirely and talks to the chip directly, so what
         * is printed is the chip's actual state rather than what the driver
         * believes it set. Decodes the config register, which is the one that
         * matters: it shows whether VBIAS is actually on and whether the chip is
         * really in 3-wire mode. Without VBIAS the chip cannot drive current
         * through the RTD and will report an open circuit no matter how good the
         * wiring is.
         */
        void dumpRegisters() const;

    protected:
        bool sample_temperature(double& temperature) const override;

    private:
        Adafruit_MAX31865* rtd_;
        int csPin_; // kept so dumpRegisters() can talk to the chip directly
        float rNominal_;
        float rRef_;
};
