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

        /**
         * @brief Bring-up aid: force this sensor to report failed readings.
         *
         * Safety logic that cannot be triggered on demand does not get tested -
         * it gets assumed. Pulling a probe lead is unreliable (a terminal that
         * still grips one strand keeps reading perfectly), so the fault paths
         * need a deterministic way in.
         *
         * While set, sample_temperature() returns false exactly as a real sensor
         * failure does, so every downstream check sees the genuine article: the
         * error counter climbs, the freshness stamp stops advancing, and the
         * machine should latch safe.
         */
        void setFaultInjection(bool on) const {
            injectFault_ = on;
        }

        [[nodiscard]] bool faultInjected() const {
            return injectFault_;
        }

    protected:
        Sample sample_temperature(double& temperature) override;

    private:
        /**
         * Where the conversion has got to.
         *
         * The MAX31865 needs the bias voltage on and settled before it
         * measures, then the conversion itself. The library spends both waits
         * in delay() - 10 ms and 65 ms - and with two probes that measured as
         * a 152 ms worst-case main-loop pass. Here each wait is just a
         * deadline, checked on whatever pass happens to be running.
         *
         * The ELECTRICAL behaviour is unchanged on purpose: same settling
         * time, same one-shot, same bias duty cycle of roughly 75 ms in every
         * 400. Continuous auto-conversion would have been simpler but leaves
         * the bias on permanently, which changes how the probe self-heats -
         * not a thing to alter quietly on a safety input.
         */
        enum class Phase : uint8_t {
            Idle,          ///< waiting for the next sampling interval
            BiasSettling,  ///< bias on, waiting for the RC to settle
            Converting,    ///< one-shot fired, waiting for the result
        };

        Phase phase_{Phase::Idle};
        uint32_t phaseStartedMs_{0};
        uint32_t lastConversionStartedMs_{0};

        /**
         * Skip the interval wait for the very first conversion, so
         * primeFirstReading() costs one conversion rather than a conversion
         * plus a whole sampling interval.
         */
        bool firstConversion_{true};

        /** @brief Raw register access, so a conversion need not block. */
        uint8_t readReg8(uint8_t addr) const;
        uint16_t readReg16(uint8_t addr) const;
        void writeReg8(uint8_t addr, uint8_t value) const;

        /** @brief Decode a completed conversion. Faults are reported here. */
        Sample finishConversion(double& temperature);

        Adafruit_MAX31865* rtd_;
        int csPin_; // kept so dumpRegisters() can talk to the chip directly
        int wires_;                   // so the diagnostics can restore the configured mode
        mutable bool injectFault_{false}; // bring-up fault injection, see setFaultInjection()
        float rNominal_;
        float rRef_;
};
