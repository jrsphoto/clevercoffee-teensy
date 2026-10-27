/**
 * @file tempHandler.cpp
 *
 * @brief Both MAX31865 PT100 RTDs, and the safety rules that depend on them
 */

#include "tempHandler.h"

#include <Arduino.h>
#include <SPI.h>

#include "hardware/pinmapping.h"
#include "hardware/tempsensors/TempSensorMAX31865.h"
#include "safetyHandler.h"

// TempSensor.h declares this; the ESP32 build defines it in main.cpp. The Teensy
// control loop does not gate anything on it yet (that logic belongs with the PID
// in phase 3), but the sensor base class writes to it, so it needs a home.
bool temperatureUpdateRunning = false;

namespace temps {

    namespace {

        /**
         * PT100 probes, confirmed by measurement on the bench: ~110 ohm through
         * the element at room temperature (a PT1000 would read ~1100). The two
         * matching leads are the paired end, so these are wired 3-wire.
         */
        constexpr float kRNominal = 100.0f; // PT100
        constexpr float kRRef = 430.0f;     // reference resistor on a PT100 board (marked 4300)

        /**
         * 4-wire, not 3-wire, and deliberately so.
         *
         * 3-wire needs the breakout's FORCE2 pin strapped to FORCE+ via its
         * three-pad jumper, which means cutting a trace. On these boards that
         * conversion left FORCE2 floating and no measurement was possible at all.
         * 4-wire is the factory state: no cuts, and FORCE2 simply goes to GND.
         *
         * A 3-wire probe runs happily on a 4-wire setup by linking RTD- to F- in
         * the screw terminals. The only cost is losing automatic lead-resistance
         * compensation, which is about 0.2 ohm here - roughly 0.5 C on a boiler
         * held to +/-1 C. Not worth a trace cut.
         */
        constexpr int kWires = 4;

        TempSensorMAX31865* boilerSensor = nullptr;
        TempSensorMAX31865* groupSensor = nullptr;

        double boilerTempC = 0.0;
        double groupTempC = 0.0;

        bool boilerOk = false;
        bool groupOk = false;

        uint32_t boilerLastGoodMs = 0;

        bool plausible(const double c) {
            return c > kPlausibleMinC && c < kPlausibleMaxC;
        }

    } // namespace

    void init() {
        // On Teensy the default hardware SPI pins already are SCK 13, MOSI 11,
        // MISO 12 - the same pins the board uses - so no remapping is needed.
        // (The ESP32 build has to remap; this one deliberately does not.)
        SPI.begin();

        boilerSensor = new TempSensorMAX31865(PIN_RTD_CS_BOILER, kRNominal, kRRef, kWires);
        groupSensor = new TempSensorMAX31865(PIN_RTD_CS_GROUP, kRNominal, kRRef, kWires);

        boilerLastGoodMs = millis();
    }

    bool selfCheck() {
        if (boilerSensor == nullptr) {
            return false;
        }

        // Give the sensor a few cycles to produce a first reading. The sensor
        // layer refreshes on a 400 ms timer, so a single read right after init
        // can legitimately return nothing yet.
        for (int attempt = 0; attempt < 10; attempt++) {
            const double c = boilerSensor->getCurrentTemperature();

            if (!boilerSensor->hasError() && plausible(c)) {
                boilerTempC = c;
                boilerOk = true;
                boilerLastGoodMs = millis();
                safety::boilerSensorHealthy = true;

                return true;
            }

            delay(100);
        }

        return false;
    }

    void loop() {
        if (boilerSensor == nullptr) {
            return;
        }

        // Once the machine is latched safe there is nothing useful left to do
        // here: every actuator is already off and no reading can change that
        // until an operator clears the fault. Polling on regardless just floods
        // the console with the same fault line several times a second, which
        // actively hinders diagnosis - the first report is the informative one.
        // Reading resumes automatically when the fault is cleared.
        if (safety::isFaulted()) {
            return;
        }

        // --- Boiler: the safety-critical sensor --------------------------
        const double boilerReading = boilerSensor->getCurrentTemperature();

        // Staleness FIRST, and taken from the sensor's own record of its last
        // successful read.
        //
        // This check used to come last and used a timestamp this handler
        // maintained itself - which was worthless. getCurrentTemperature() keeps
        // returning the last good value while a sensor is failing, so the value
        // stayed plausible, the handler kept marking it good, and it refreshed
        // the very timestamp meant to detect the problem. Pulling a probe lead
        // from a running sensor left the heater permitted for seconds while the
        // chip reported an open circuit four times a second.
        const uint32_t sinceGood = boilerSensor->millisSinceLastGoodReading();

        if (sinceGood > kStaleAfterMs) {
            boilerOk = false;
            safety::latchFault(FaultCode::SensorStale, "boiler RTD stopped updating");
        }
        else if (boilerSensor->hasError()) {
            boilerOk = false;
            safety::latchFault(FaultCode::SensorFault, "boiler RTD fault");
        }
        else if (!plausible(boilerReading)) {
            boilerOk = false;
            safety::latchFault(FaultCode::SensorImplausible, "boiler RTD out of range");
        }
        else {
            boilerTempC = boilerReading;
            boilerOk = true;
            boilerLastGoodMs = millis();

            if (boilerReading >= kBoilerOverTempC) {
                safety::latchFault(FaultCode::OverTemperature, "boiler over hard limit");
            }
        }

        // Publish freshness for the output layer, so the heater cannot be
        // energized on a reading nobody is standing behind. Defence in depth:
        // the fault latch above should already have fired, but the heater is the
        // one output that can destroy a boiler, so it gets a second gate that
        // does not depend on a fault having been latched yet.
        safety::boilerSensorHealthy = boilerOk;

        // --- Group: monitoring only, never latches a fault ---------------
        const double groupReading = groupSensor->getCurrentTemperature();

        if (groupSensor->hasError() || !plausible(groupReading)) {
            // Not an error worth stopping for. The group probe feeds flush and
            // readiness hints, cannot cause harm by being wrong, and may simply
            // not be fitted during bring-up.
            groupOk = false;
        }
        else {
            groupTempC = groupReading;
            groupOk = true;
        }
    }

    double boilerC() {
        return boilerTempC;
    }

    double groupC() {
        return groupTempC;
    }

    bool boilerHealthy() {
        return boilerOk;
    }

    bool groupHealthy() {
        return groupOk;
    }

    void printStatus() {
        Serial.print("  boiler RTD (CS pin ");
        Serial.print(PIN_RTD_CS_BOILER);
        Serial.print(") : ");

        if (boilerOk) {
            Serial.print(boilerTempC, 2);
            Serial.print(" C   (last good reading ");
            Serial.print(boilerSensor != nullptr ? boilerSensor->millisSinceLastGoodReading() : 0);
            Serial.print(" ms ago, stale limit ");
            Serial.print(kStaleAfterMs);
            Serial.println(" ms)");
        }
        else {
            Serial.println("FAULT / no reading");
        }

        Serial.print("  group  RTD (CS pin ");
        Serial.print(PIN_RTD_CS_GROUP);
        Serial.print(") : ");

        if (groupOk) {
            Serial.print(groupTempC, 2);
            Serial.println(" C");
        }
        else {
            Serial.println("not fitted / no reading (monitoring only, not a fault)");
        }
    }

    void injectBoilerFault(const bool on) {
        if (boilerSensor != nullptr) {
            boilerSensor->setFaultInjection(on);
        }
    }

    bool boilerFaultInjected() {
        return boilerSensor != nullptr && boilerSensor->faultInjected();
    }

    void printDiagnostics() {
        Serial.println("RTD raw diagnostics:");

        Serial.print("  boiler (CS ");
        Serial.print(PIN_RTD_CS_BOILER);
        Serial.println("):");

        if (boilerSensor != nullptr) {
            boilerSensor->testSpiWrites();
            boilerSensor->dumpRegisters();
            boilerSensor->printDiagnostics();
        }

        Serial.print("  group  (CS ");
        Serial.print(PIN_RTD_CS_GROUP);
        Serial.println("):");

        if (groupSensor != nullptr) {
            // Same depth of diagnostics as the boiler. Previously this only
            // printed the resistance, which cannot distinguish "probe open" from
            // "chip not responding" - both show as zero.
            groupSensor->testSpiWrites();
            groupSensor->dumpRegisters();
            groupSensor->printDiagnostics();
        }
    }

} // namespace temps
