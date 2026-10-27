/**
 * @file TempSensorMAX31865.cpp
 *
 * @brief Handler for a PT100 / PT1000 RTD via a MAX31865 amplifier (SPI)
 */

#include "TempSensorMAX31865.h"
#include "Logger.h"
#include <Arduino.h>
#include <SPI.h>

TempSensorMAX31865::TempSensorMAX31865(const int csPin, const float rNominal, const float rRef, const int wires) :
    rNominal_(rNominal), rRef_(rRef), csPin_(csPin), wires_(wires) {
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

void TempSensorMAX31865::dumpRegisters() const {
    // Talk to the chip directly rather than through the library, so this reports
    // the chip's real state instead of the driver's intentions. MAX31865 is SPI
    // mode 1 (CPOL=0, CPHA=1); a read is the register address with bit 7 clear.
    auto readReg = [this](const uint8_t addr) -> uint8_t {
        SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE1));
        digitalWrite(csPin_, LOW);
        SPI.transfer(addr & 0x7F);
        const uint8_t value = SPI.transfer(0xFF);
        digitalWrite(csPin_, HIGH);
        SPI.endTransaction();

        return value;
    };

    auto writeReg = [this](const uint8_t addr, const uint8_t value) {
        SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE1));
        digitalWrite(csPin_, LOW);
        SPI.transfer(addr | 0x80);
        SPI.transfer(value);
        digitalWrite(csPin_, HIGH);
        SPI.endTransaction();
    };

    // Drive the conversion by hand rather than calling the library, so VBIAS can
    // be confirmed ON at the moment the chip measures. The library's readRTD()
    // enables bias, converts, then disables it again - so reading CONFIG
    // afterwards ALWAYS shows VBIAS off and tells you nothing. An earlier version
    // of this dump printed that as though it were a fault, which was actively
    // misleading.
    constexpr uint8_t kVbias = 0x80;
    constexpr uint8_t kOneShot = 0x20;
    constexpr uint8_t kThreeWire = 0x10;

    // Convert in BOTH 2-wire and 3-wire mode and report them side by side.
    //
    // These use different pins inside the chip, so comparing them separates two
    // faults that otherwise look identical: a probe that is not connected, versus
    // a board whose 3-wire routing has been broken (e.g. by one cut too many when
    // converting the breakout). If 2-wire reads a sane resistance and 3-wire does
    // not, the probe and terminals are fine and the fault is in the 3-wire path.
    auto convert = [&](const uint8_t wireBit) -> uint16_t {
        writeReg(0x00, kVbias | wireBit);
        delay(10);
        writeReg(0x00, kVbias | kOneShot | wireBit);
        delay(70);

        const uint16_t msb = readReg(0x01);
        const uint16_t lsb = readReg(0x02);

        return static_cast<uint16_t>(((msb << 8) | lsb) >> 1);
    };

    const uint16_t raw2 = convert(0x00); // 2/4-wire
    const uint16_t raw3 = convert(kThreeWire);

    // Leave bias on just long enough to read a meaningful CONFIG back.
    writeReg(0x00, kVbias | kThreeWire);
    delay(5);
    const uint8_t config = readReg(0x00);

    const uint8_t rtdMsb = readReg(0x01);
    const uint8_t rtdLsb = readReg(0x02);
    const uint8_t fault = readReg(0x07);

    // Restore the mode the firmware is actually configured for. Leaving the chip
    // in 3-wire here silently corrupted every subsequent read through the normal
    // driver path - the diagnostic was changing the thing it was measuring.
    writeReg(0x00, (wires_ == 3) ? kThreeWire : 0x00); // bias off, configured mode

    const float ohms2 = (static_cast<float>(raw2) / 32768.0f) * rRef_;
    const float ohms3 = (static_cast<float>(raw3) / 32768.0f) * rRef_;

    Serial.print("    2/4-WIRE mode (FORCE2 at GND): raw=");
    Serial.print(raw2);
    Serial.print("  ");
    Serial.print(ohms2, 2);
    Serial.println(" ohm");

    Serial.print("    3-WIRE mode (FORCE2 at FORCE+): raw=");
    Serial.print(raw3);
    Serial.print("  ");
    Serial.print(ohms3, 2);
    Serial.println(" ohm");

    if (ohms2 > 90.0f && ohms2 < 200.0f && ohms3 < 90.0f) {
        Serial.println("    >>> 2-wire reads a sane PT100 but 3-wire does not.");
        Serial.println("        The probe and terminals are GOOD; the 3-wire routing on the");
        Serial.println("        breakout is broken - almost certainly one cut too many.");
    }

    Serial.print("    CONFIG=0x");
    Serial.print(config, HEX);
    Serial.print("  RTD=0x");
    Serial.print(rtdMsb, HEX);
    Serial.print(rtdLsb < 0x10 ? "0" : "");
    Serial.print(rtdLsb, HEX);
    Serial.print("  FAULT=0x");
    Serial.println(fault, HEX);

    // Read with bias deliberately still enabled, so this is meaningful.
    Serial.print("      VBIAS  = ");
    Serial.println((config & 0x80) ? "ON (confirmed during conversion)" : "OFF  <<< chip is not driving current - real fault");
    Serial.print("      3-WIRE = ");
    Serial.println((config & 0x10) ? "yes" : "no (2/4-wire)");
    Serial.print("      auto   = ");
    Serial.println((config & 0x40) ? "yes" : "no (one-shot)");

    if (config == 0x00 || config == 0xFF) {
        Serial.println("      CONFIG reads all-zero/all-ones - the chip is not responding properly");
    }
}

bool TempSensorMAX31865::testSpiWrites() const {
    // Threshold registers are plain read/write storage with no effect on a
    // measurement, which makes them a safe place to prove the write path.
    const uint16_t savedLow = rtd_->getLowerThreshold();
    const uint16_t savedHigh = rtd_->getUpperThreshold();

    // A pattern with bits set in both bytes, so a stuck-low or stuck-high line
    // cannot accidentally match.
    constexpr uint16_t kTestLow = 0x2A55;
    constexpr uint16_t kTestHigh = 0x55AA;

    rtd_->setThresholds(kTestLow, kTestHigh);
    delay(5);

    const uint16_t readLow = rtd_->getLowerThreshold();
    const uint16_t readHigh = rtd_->getUpperThreshold();

    const bool ok = (readLow == kTestLow) && (readHigh == kTestHigh);

    Serial.print("    SPI write test: wrote 0x");
    Serial.print(kTestLow, HEX);
    Serial.print("/0x");
    Serial.print(kTestHigh, HEX);
    Serial.print(", read back 0x");
    Serial.print(readLow, HEX);
    Serial.print("/0x");
    Serial.print(readHigh, HEX);

    if (ok) {
        Serial.println("  -> WRITES OK (SDI/MOSI is good)");
    }
    else if (readLow == 0 && readHigh == 0) {
        Serial.println("  -> WRITES FAILING, reads return zero");
        Serial.println("       Check SDI (chip) to Teensy pin 11, and that the chip is selected.");
    }
    else {
        Serial.println("  -> WRITES FAILING - reads work but writes do not land");
        Serial.println("       SDI/MOSI (Teensy pin 11) is the prime suspect: a broken write");
        Serial.println("       path leaves VBIAS off, so the chip reports an open circuit");
        Serial.println("       that is indistinguishable from a disconnected probe.");
    }

    // Put the thresholds back where they were.
    rtd_->setThresholds(savedLow, savedHigh);

    return ok;
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
