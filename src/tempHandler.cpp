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
        constexpr float kRRef = 430.0f;     // reference resistor on a PT100 board
        constexpr int kWires = 3;

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

        if (boilerSensor->hasError()) {
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

        // A sensor that simply stops updating is as dangerous as one reporting a
        // fault, and it will not trip either check above - so time it explicitly.
        if ((millis() - boilerLastGoodMs) > kStaleAfterMs) {
            boilerOk = false;
            safety::latchFault(FaultCode::SensorStale, "boiler RTD stopped updating");
        }

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
            Serial.println(" C");
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
            groupSensor->printDiagnostics();
        }
    }

} // namespace temps
