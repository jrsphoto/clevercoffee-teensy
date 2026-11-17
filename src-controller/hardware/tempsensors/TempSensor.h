/**
 * @file TempSensor.h
 *
 * @brief Interface that all temperature sensors have to implement
 */

#pragma once

#include <cmath>

#include "Logger.h"
#include "utils/Timer.h"

extern bool temperatureUpdateRunning;

class TempSensor {
    public:
        /**
         * @brief Abstract temperature sensor class
         * @details This class controls a temperature sensor. It updates the value in regular intervals of 400ms, controlled
         *          by a timer. The temperature sampling result is checked for errors and an internal error counter is kept
         *          to detect consecutive reading errors
         */
        /**
         * POLLED EVERY 2 ms, NOT EVERY 400 ms.
         *
         * This used to be the sampling interval, because sampling was a
         * single blocking call. It is now how often the sensor's own state
         * machine gets a chance to advance, and the SENSOR decides how often
         * it actually converts. 2 ms is fine either way: the work on a pass
         * with nothing to do is one comparison.
         */
        TempSensor() :
            update_temperature([this] { update_temperature_reading(); }, 2) {
        }

        /**
         * @brief Returns the current temperature
         * @details Requests sampling from attached sensor and returns reading.
         * @return Temperature in degrees Celsius
         */
        double getCurrentTemperature() {
            // Trigger the timer to update the temperature:
            update_temperature();
            return last_temperature_;
        }

        /*
         * getAverageTemperatureRate() and the moving-average filter behind it
         * are GONE, 2026-11-17. See the note above update_temperature_reading()
         * for why. If something ever needs a rate of change, write it fresh
         * rather than restoring that code.
         */

        /**
         * @brief Default destructor
         */
        virtual ~TempSensor() = default;

        /**
         * @brief Returns error state of the temperature sensor
         * @return true if the sensor is in error, false otherwise
         */
        [[nodiscard]] bool hasError() const {
            return error_;
        }

        /**
         * @brief Milliseconds since the last SUCCESSFUL reading.
         *
         * hasError() only goes true after max_bad_treadings_ consecutive
         * failures - several seconds at the 400 ms update rate. Until then
         * getCurrentTemperature() keeps returning the last good value, so a
         * failing sensor looks perfectly healthy and plausible to a caller.
         *
         * Safety-critical callers must use this instead: it is the only way to
         * tell "this value was just measured" from "this value is a souvenir of
         * the last time the sensor worked". Found on the bench by pulling a
         * probe lead from a running sensor, where the heater was still permitted
         * while the chip was reporting an open circuit four times a second.
         */
        [[nodiscard]] uint32_t millisSinceLastGoodReading() const {
            return millis() - last_good_reading_ms_;
        }

        /**
         * @brief Block ONCE, at startup, until the first conversion lands.
         *
         * THE ONLY PLACE THIS CLASS IS ALLOWED TO BLOCK, and it exists to
         * restore an invariant the old blocking read provided for free: that
         * a real reading exists before the control loop ever runs.
         *
         * Without it the port regressed in a way that looked like a dead
         * probe. The sensor now answers "not ready" for the first 75 ms, and
         * boot spends about five seconds in the scale's start-up before the
         * loop begins - so the first staleness check ran against a sensor
         * that had never been read, saw the whole uptime as the gap, and
         * latched "sensor stale" on a probe reading a perfectly good
         * 109 ohms.
         *
         * Costs roughly one conversion time per sensor, once, during a boot
         * that already blocks for far longer elsewhere.
         *
         * @return true if a reading was obtained.
         */
        bool primeFirstReading(const uint32_t timeoutMs = 500) {
            const uint32_t began = millis();

            while (static_cast<int32_t>(millis() - began) < static_cast<int32_t>(timeoutMs)) {
                const Sample result = sample_temperature(last_temperature_);

                if (result == Sample::Fresh) {
                    last_good_reading_ms_ = millis();
                    bad_readings_ = 0;
                    error_ = false;

                    return true;
                }

                if (result == Sample::Failed) {
                    return false;
                }
            }

            return false;
        }

    public:
        /**
         * What one attempt to read the sensor produced.
         *
         * THREE OUTCOMES, NOT TWO, and the middle one is the point. Reading a
         * MAX31865 properly takes 75 ms of settling and conversion, which used
         * to be spent inside a blocking delay - measured on hardware at a
         * worst single main-loop pass of 152 ms with both probes fitted. That
         * is long enough to drop load-cell conversions, make the pump ramp in
         * lurches and freeze the encoder and brew switch.
         *
         * Spreading that wait across loop passes means a poll can land while
         * a conversion is still running. "Not ready" must NOT be counted as a
         * failure - doing so would march the error counter up to the fault
         * threshold on a perfectly healthy sensor.
         */
        enum class Sample {
            Fresh,    ///< a new conversion completed; `temperature` is updated
            NotReady, ///< still converting. Not an error. Do nothing.
            Failed,   ///< the sensor or its wiring is bad
        };

    protected:
        /**
         * @brief Advance the sensor and report a reading if one is ready.
         *
         * Called often - the timer below polls at a few milliseconds - so an
         * implementation MUST return promptly and must never block waiting on
         * a conversion. It owns its own sampling cadence.
         */
        virtual Sample sample_temperature(double& temperature) = 0;

    private:
        /**
         * @brief Small helper method to be wrapped in a timer for updating the temperature from the sensors
         */
        void update_temperature_reading() {
            const Sample result = sample_temperature(last_temperature_);

            // STILL CONVERTING IS NOT A FAILURE. Returning here without
            // touching the error counter or the good-reading stamp is the
            // whole reason this is a three-way result: the sensor is polled
            // far more often than it produces readings, and counting every
            // poll in between as an error would fault a healthy probe within
            // a fraction of a second.
            if (result == Sample::NotReady) {
                return;
            }

            if (result == Sample::Fresh) {
                LOGF(TRACE, "Temperature reading successful: %.1f", last_temperature_);

                // Stamp the time of a genuinely successful read. See
                // millisSinceLastGoodReading() for why this matters.
                last_good_reading_ms_ = millis();

                // Reset error counter and error state
                bad_readings_ = 0;
                error_ = false;
                temperatureUpdateRunning = true;
            }
            else if (!error_) {
                // Increment error counter
                LOGF(DEBUG, "Error during temperature reading, incrementing error counter to %i", bad_readings_);
                bad_readings_++;
            }

            if (bad_readings_ >= max_bad_treadings_ && !error_) {
                error_ = true;
                LOGF(ERROR, "Temperature sensor malfunction, %i consecutive errors", bad_readings_);
            }
        }

        Timer update_temperature;
        double last_temperature_{};
        uint32_t last_good_reading_ms_{0};
        int bad_readings_{0};
        int max_bad_treadings_{10};
        bool error_{false};

        /*
         * REMOVED 2026-11-17: upstream's FIR moving-average filter, its three
         * 15-element arrays and getAverageTemperatureRate().
         *
         * It fed upstream's SOFTWARE BREW DETECTION, which infers that a shot
         * has started from the rate the boiler cools. This machine does not
         * use it and never will - it has a real brew switch on a real pin,
         * and `switchHandler` reads it. Nothing in this firmware read the
         * number: the getter was called by nobody.
         *
         * It also wrote OUT OF BOUNDS on the first call for every sensor.
         * `valueIndex` started at -1 and the branch that initialised the
         * arrays never set it to 0, so the next three statements wrote to
         * `timeValues[-1]`, `tempValues[-1]` and `tempChangeRates[-1]` -
         * past the front of each array, over whatever the compiler had put
         * there. Separately, `std::accumulate` was seeded with the integer
         * `0`, which made the running total an int and truncated every
         * double it added.
         *
         * Deleted rather than repaired, because fixing arithmetic that
         * nothing consumes is work with no result, and the undefined
         * behaviour goes away completely this way. Found in review; the
         * out-of-bounds writes had been running on the bench unnoticed since
         * the sensors were brought up. Frees about 300 bytes per sensor.
         *
         * If a rate of change is ever wanted - for the pressure sensor, say -
         * write it fresh against a specific requirement.
         */
};
