/**
 * @file HX711Scale.cpp
 * @brief HX711-based scale implementation
 */

#include "HX711Scale.h"
#include "hardware/GPIOPin.h"
#include "hardware/pinmapping.h"

HX711Scale::HX711Scale(const int dataPin, const int clkPin, const float calibrationFactor) :
    loadCell1(new HX711_ADC(dataPin, clkPin)), loadCell2(nullptr), currentWeight(0.0), calibrationFactor1(calibrationFactor), calibrationFactor2(1.0), isDualCell(false), readSecondScale(false), weight1(0.0), weight2(0.0) {
}

HX711Scale::HX711Scale(int dataPin1, int dataPin2, int clkPin, float calibrationFactor1, float calibrationFactor2) :
    loadCell1(new HX711_ADC(dataPin1, clkPin)),
    loadCell2(new HX711_ADC(dataPin2, clkPin)),
    currentWeight(0.0),
    calibrationFactor1(calibrationFactor1),
    calibrationFactor2(calibrationFactor2),
    isDualCell(true),
    readSecondScale(false),
    weight1(0.0),
    weight2(0.0) {
}

HX711Scale::~HX711Scale() {
    delete loadCell1;
    delete loadCell2;
}

bool HX711Scale::init() {
    loadCell1->begin();
    GPIOPin(PIN_HXDAT, GPIOPin::IN_PULLUP);

    if (isDualCell) {
        loadCell2->begin();
        GPIOPin(PIN_HXDAT2, GPIOPin::IN_PULLUP);
    }

    constexpr unsigned long stabilizingTime = 5000;
    constexpr boolean _tare = true;

    if (!isDualCell) {
        while (!loadCell1->startMultiple(stabilizingTime, _tare)) {
            // Wait for initialization
        }
    }
    else {
        byte loadCell1Ready = 0;
        byte loadCell2Ready = 0;

        while (loadCell1Ready + loadCell2Ready < 2) {
            if (!loadCell1Ready) {
                loadCell1Ready = loadCell1->startMultiple(stabilizingTime, _tare);
            }

            if (!loadCell2Ready) {
                loadCell2Ready = loadCell2->startMultiple(stabilizingTime, _tare);
            }
        }
    }

    if (loadCell1->getTareTimeoutFlag() || loadCell1->getSignalTimeoutFlag()) {
        return false;
    }

    if (isDualCell && (loadCell2->getTareTimeoutFlag() || loadCell2->getSignalTimeoutFlag())) {
        return false;
    }

    loadCell1->setCalFactor(calibrationFactor1);

    if (isDualCell) {
        loadCell2->setCalFactor(calibrationFactor2);
    }

    return true;
}

bool HX711Scale::update() {
    if (!isDualCell) {
        if (loadCell1->update()) {
            weight1 = loadCell1->getData();
            currentWeight = weight1;
            return true;
        }
    }
    else {
        bool updated = false;

        if (!readSecondScale) {
            if (loadCell1->update()) {
                weight1 = loadCell1->getData();
                readSecondScale = true;
                updated = true;
            }
        }
        else {
            if (loadCell2->update()) {
                weight2 = loadCell2->getData();
                readSecondScale = false;
                updated = true;
            }
        }

        if (updated) {
            currentWeight = weight1 + weight2;
            return true;
        }
    }

    return false;
}

float HX711Scale::getWeight() const {
    return currentWeight;
}

/**
 * @brief Zero the cells. DOES NOT TOUCH THE CALIBRATION FACTORS.
 *
 * It used to re-apply this wrapper's own copies of them, and that was a
 * loaded gun. `weightHandler` calibrates by calling `setCalFactor()` straight
 * on the cells, so this object's copies are whatever it happened to be
 * constructed with - 1.0 on a machine that has not calibrated yet. One call
 * to this function would have thrown away a factor measured against a real
 * mass and replaced it with 1.0, silently.
 *
 * Nothing calls this today, which is the only reason it never fired. The
 * scale's calibration has already been destroyed once by the head pushing a
 * 1.0 default, so a second route to the same outcome is not worth keeping.
 *
 * Taring and calibrating are separate operations anyway: zeroing the reading
 * has no bearing on the scale factor, so re-applying one from inside the
 * other was never right. Found in review 2026-11-17.
 */
void HX711Scale::tare() {
    loadCell1->tare();

    if (isDualCell) {
        loadCell2->tare();
    }
}

void HX711Scale::setSamples(const int samples) {
    if (loadCell1) {
        loadCell1->setSamplesInUse(samples);
    }
    if (loadCell2) {
        loadCell2->setSamplesInUse(samples);
    }
}

/**
 * @brief The factor the cell is ACTUALLY using, not this object's copy of it.
 *
 * Same reasoning as tare(). `weightHandler` calibrates by calling
 * `setCalFactor()` on the cells directly, so the members below go stale the
 * moment anything is calibrated. Asking the cell cannot be wrong.
 *
 * The members are still the value this object was CONSTRUCTED with, which is
 * what init() applies at startup, and that is all they are for now.
 */
float HX711Scale::getCalibrationFactor(const int cellNumber) const {
    if (cellNumber == 1 && loadCell1 != nullptr) {
        return loadCell1->getCalFactor();
    }

    if (cellNumber == 2 && isDualCell && loadCell2 != nullptr) {
        return loadCell2->getCalFactor();
    }

    return cellNumber == 1 ? calibrationFactor1 : calibrationFactor2;
}

void HX711Scale::setCalibrationFactor(const float factor, const int cellNumber) {
    if (cellNumber == 1) {
        calibrationFactor1 = factor;

        if (loadCell1) {
            loadCell1->setCalFactor(factor);
        }
    }
    else if (cellNumber == 2 && isDualCell) {
        calibrationFactor2 = factor;

        if (loadCell2) {
            loadCell2->setCalFactor(factor);
        }
    }
}

HX711_ADC* HX711Scale::getLoadCell(const int cellNumber) const {
    if (cellNumber == 1) {
        return loadCell1;
    }

    if (cellNumber == 2 && isDualCell) {
        return loadCell2;
    }

    return nullptr;
}
