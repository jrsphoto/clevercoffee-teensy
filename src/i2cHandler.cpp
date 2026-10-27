/**
 * @file i2cHandler.cpp
 *
 * @brief The shared I2C bus: recovery, startup, and an address scan
 */

#include "i2cHandler.h"

#include <Arduino.h>
#include <Wire.h>

#include "hardware/pinmapping.h"

namespace i2c {

    namespace {

        const char* nameFor(const uint8_t addr) {
            switch (addr) {
                case kAddrDuppaEncoder: return "Duppa i2cEncoder V2";
                case kAddrPressure: return "ABP2 brew pressure sensor";
                case 0x04: return "DuPPa encoder (no address jumpers soldered)";
                default: break;
            }

            // Ranges worth naming even when not expected here, because seeing a
            // recognised family is more useful than a bare number.
            if (addr >= 0x20 && addr <= 0x27) {
                return "PCF8574 (plain) - not expected on this board";
            }

            if (addr >= 0x38 && addr <= 0x3F) {
                return "PCF8574A or SSD1306 display";
            }

            return "unknown";
        }

    } // namespace

    void init() {
        // Bus recovery, BEFORE Wire.begin() takes the pins.
        //
        // If a device was mid-transaction when the Teensy last reset, it may
        // still be holding SDA low and will ignore a fresh start. Clocking SCL
        // with SDA released walks it out of that state. Nine clocks is the
        // conventional figure: enough for any device to finish the byte it
        // thinks it is sending.
        pinMode(PIN_I2CSDA, INPUT_PULLUP);
        pinMode(PIN_I2CSCL, INPUT_PULLUP);

        if (digitalRead(PIN_I2CSDA) == LOW) {
            pinMode(PIN_I2CSCL, OUTPUT);

            for (int i = 0; i < 9; i++) {
                digitalWrite(PIN_I2CSCL, LOW);
                delayMicroseconds(5);
                digitalWrite(PIN_I2CSCL, HIGH);
                delayMicroseconds(5);
            }

            pinMode(PIN_I2CSCL, INPUT_PULLUP);
        }

        Wire.begin();
        Wire.setClock(kBusHz);
    }

    void checkLines() {
        Wire.end(); // release the pins so they can be read as plain GPIO

        // Floating: no internal pull-up. If the bus has external pull-ups these
        // still read high; without them the lines are undriven.
        pinMode(PIN_I2CSDA, INPUT);
        pinMode(PIN_I2CSCL, INPUT);
        delayMicroseconds(50);
        const int sdaFloat = digitalRead(PIN_I2CSDA);
        const int sclFloat = digitalRead(PIN_I2CSCL);

        // With the Teensy's internal pull-ups (~22k). Anything still reading low
        // here is being actively held down.
        pinMode(PIN_I2CSDA, INPUT_PULLUP);
        pinMode(PIN_I2CSCL, INPUT_PULLUP);
        delayMicroseconds(50);
        const int sdaPullup = digitalRead(PIN_I2CSDA);
        const int sclPullup = digitalRead(PIN_I2CSCL);

        Serial.println("I2C line check:");
        Serial.print("    SDA (pin ");
        Serial.print(PIN_I2CSDA);
        Serial.print("): floating=");
        Serial.print(sdaFloat ? "HIGH" : "LOW");
        Serial.print("  with pull-up=");
        Serial.println(sdaPullup ? "HIGH" : "LOW");

        Serial.print("    SCL (pin ");
        Serial.print(PIN_I2CSCL);
        Serial.print("): floating=");
        Serial.print(sclFloat ? "HIGH" : "LOW");
        Serial.print("  with pull-up=");
        Serial.println(sclPullup ? "HIGH" : "LOW");

        if (!sdaPullup || !sclPullup) {
            Serial.println("    >>> A line is held LOW even with a pull-up. Something is");
            Serial.println("        driving it down: a wedged device, a short to ground, or");
            Serial.println("        SDA/SCL wired to the wrong pins.");
        }
        else if (sdaFloat && sclFloat) {
            Serial.println("    >>> Both lines idle high WITHOUT the internal pull-ups, so the");
            Serial.println("        bus has external pull-ups and is electrically healthy.");
            Serial.println("        An empty scan then means nothing is answering - check power");
            Serial.println("        to the device and that SDA/SCL are not swapped.");
        }
        else {
            Serial.println("    >>> Lines only idle high with the Teensy's internal pull-ups, so");
            Serial.println("        there are NO external pull-ups on this bus. The internal ones");
            Serial.println("        (~22k) are weak but usually enough at 100 kHz on short wires.");
        }

        init(); // restore the bus
    }

    void probe(const uint8_t addr) {
        Wire.beginTransmission(addr);
        const uint8_t result = Wire.endTransmission();

        Serial.print("    0x");

        if (addr < 0x10) {
            Serial.print("0");
        }

        Serial.print(addr, HEX);
        Serial.print(" (");
        Serial.print(nameFor(addr));
        Serial.print("): ");

        switch (result) {
            case 0:
                Serial.println("ACK - present");
                break;
            case 2:
                Serial.println("NACK on address - nothing is answering here");
                break;
            case 3:
                Serial.println("NACK on data");
                break;
            case 4:
                Serial.println("bus error - check wiring, SDA/SCL may be swapped");
                break;
            case 5:
                Serial.println("timeout - a line may be held low");
                break;
            default:
                Serial.print("error ");
                Serial.println(result);
                break;
        }
    }

    void probeExpected() {
        Serial.println("Targeted probe of expected addresses:");
        probe(kAddrDuppaEncoder);
        probe(kAddrPressure);
    }

    namespace {

        // Open-drain helpers: release = let the pull-up take it high, drive =
        // actively pull low. Never drive high - that is what makes I2C able to
        // share a wire without contention.
        inline void release(const uint8_t pin) {
            pinMode(pin, INPUT_PULLUP);
        }

        inline void driveLow(const uint8_t pin) {
            pinMode(pin, OUTPUT);
            digitalWrite(pin, LOW);
        }

        uint32_t bitDelayUs = 5; // ~100 kHz by default

    } // namespace

    namespace {

        /**
         * @brief Bit-bang a single address probe. Returns true if the device ACKs.
         *
         * Used for the reserved address ranges, which the hardware peripheral
         * refuses to send, and for testing reversed wire orientations.
         */
        bool bitBangProbe(const uint8_t sdaPin, const uint8_t sclPin, const uint8_t addr) {
            release(sdaPin);
            release(sclPin);
            delayMicroseconds(bitDelayUs);

            // START
            driveLow(sdaPin);
            delayMicroseconds(bitDelayUs);
            driveLow(sclPin);
            delayMicroseconds(bitDelayUs);

            const uint8_t byteOut = static_cast<uint8_t>(addr << 1);

            for (int bit = 7; bit >= 0; bit--) {
                if ((byteOut >> bit) & 1) {
                    release(sdaPin);
                }
                else {
                    driveLow(sdaPin);
                }

                delayMicroseconds(bitDelayUs);
                release(sclPin);
                delayMicroseconds(bitDelayUs);
                driveLow(sclPin);
                delayMicroseconds(bitDelayUs);
            }

            // ACK slot
            release(sdaPin);
            delayMicroseconds(bitDelayUs);
            release(sclPin);
            delayMicroseconds(bitDelayUs);

            const bool ack = (digitalRead(sdaPin) == LOW);

            driveLow(sclPin);
            delayMicroseconds(bitDelayUs);

            // STOP
            driveLow(sdaPin);
            delayMicroseconds(bitDelayUs);
            release(sclPin);
            delayMicroseconds(bitDelayUs);
            release(sdaPin);
            delayMicroseconds(bitDelayUs);

            return ack;
        }

    } // namespace

    void scanBitBang(const uint8_t sdaPin, const uint8_t sclPin, const char* label) {
        Wire.end(); // hand the pins back so they can be driven manually

        Serial.print("  bit-banged scan, ");
        Serial.print(label);
        Serial.println(":");

        int found = 0;

        for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
            release(sdaPin);
            release(sclPin);
            delayMicroseconds(bitDelayUs);

            // START: SDA falls while SCL is high
            driveLow(sdaPin);
            delayMicroseconds(bitDelayUs);
            driveLow(sclPin);
            delayMicroseconds(bitDelayUs);

            // 7 address bits then the write bit, MSB first
            const uint8_t byteOut = static_cast<uint8_t>(addr << 1);

            for (int bit = 7; bit >= 0; bit--) {
                if ((byteOut >> bit) & 1) {
                    release(sdaPin);
                }
                else {
                    driveLow(sdaPin);
                }

                delayMicroseconds(bitDelayUs);
                release(sclPin);
                delayMicroseconds(bitDelayUs);
                driveLow(sclPin);
                delayMicroseconds(bitDelayUs);
            }

            // ACK slot: release SDA and see whether the device pulls it down
            release(sdaPin);
            delayMicroseconds(bitDelayUs);
            release(sclPin);
            delayMicroseconds(bitDelayUs);

            const bool ack = (digitalRead(sdaPin) == LOW);

            driveLow(sclPin);
            delayMicroseconds(bitDelayUs);

            // STOP: SDA rises while SCL is high
            driveLow(sdaPin);
            delayMicroseconds(bitDelayUs);
            release(sclPin);
            delayMicroseconds(bitDelayUs);
            release(sdaPin);
            delayMicroseconds(bitDelayUs);

            if (ack) {
                found++;

                Serial.print("      0x");

                if (addr < 0x10) {
                    Serial.print("0");
                }

                Serial.print(addr, HEX);
                Serial.print("  ");
                Serial.println(nameFor(addr));
            }
        }

        if (found == 0) {
            Serial.println("      nothing answered");
        }

        init(); // restore the hardware bus
    }

    void scanSlow() {
        // Long flying leads with 4.7k pull-ups give slow rise times, and some
        // devices are fussy immediately after power-up. Dropping the clock by
        // 100x costs nothing and rules out marginal timing as a cause.
        const uint32_t saved = bitDelayUs;

        for (const uint32_t d : {50u, 250u, 500u}) {
            bitDelayUs = d;

            char label[48];
            snprintf(label, sizeof(label), "SDA=18 SCL=19 at ~%lu kHz", (unsigned long)(500 / d));
            scanBitBang(PIN_I2CSDA, PIN_I2CSCL, label);
        }

        bitDelayUs = saved;
    }

    void probeSdaDrive() {
        // Can ANYTHING out there pull SDA down? Hold SCL low (so no device is
        // being clocked) and watch SDA while sending nothing. Then drive SDA low
        // ourselves and release it, and time how fast it recovers - a slow rise
        // means heavy capacitance or a weak pull-up.
        Wire.end();

        release(PIN_I2CSCL);
        release(PIN_I2CSDA);
        delayMicroseconds(200);

        Serial.println("SDA drive test:");
        Serial.print("    SDA at rest: ");
        Serial.println(digitalRead(PIN_I2CSDA) ? "HIGH" : "LOW");

        driveLow(PIN_I2CSDA);
        delayMicroseconds(200);
        Serial.print("    SDA while we pull it low: ");
        Serial.println(digitalRead(PIN_I2CSDA) ? "HIGH (cannot pull it down - line is driven high!)" : "LOW (normal)");

        // Release and measure the rise.
        noInterrupts();
        release(PIN_I2CSDA);
        uint32_t t0 = micros();
        uint32_t riseUs = 0;

        while ((micros() - t0) < 1000) {
            if (digitalRead(PIN_I2CSDA) == HIGH) {
                riseUs = micros() - t0;
                break;
            }
        }
        interrupts();

        Serial.print("    SDA rise time after release: ");

        if (riseUs == 0) {
            Serial.println("did not rise within 1000 us - pull-up missing or line shorted low");
        }
        else {
            Serial.print(riseUs);
            Serial.println(" us  (a few us is healthy; hundreds means weak pull-up / heavy capacitance)");
        }

        init();
    }

    void scanReserved() {
        // Probe the RESERVED range 0x00-0x07, bit-banged because the hardware
        // Wire library will not send these addresses.
        //
        // Normally nothing should live here. But the DuPPa i2cEncoder V2 takes
        // its address from solder jumpers A0-A6, and with all of them OPEN - the
        // factory state - it answers at 0x00. DuPPa warn against leaving it that
        // way precisely because 0x00 is reserved, but a brand-new unit arrives
        // like that, and a scanner that correctly skips the reserved range will
        // never see it.
        Wire.end();

        Serial.println("Probing the RESERVED range 0x00-0x07 (where an unjumpered DuPPa sits):");

        int found = 0;

        for (uint8_t addr = 0x00; addr <= 0x07; addr++) {
            release(PIN_I2CSDA);
            release(PIN_I2CSCL);
            delayMicroseconds(bitDelayUs);

            driveLow(PIN_I2CSDA);
            delayMicroseconds(bitDelayUs);
            driveLow(PIN_I2CSCL);
            delayMicroseconds(bitDelayUs);

            const uint8_t byteOut = static_cast<uint8_t>(addr << 1);

            for (int bit = 7; bit >= 0; bit--) {
                if ((byteOut >> bit) & 1) {
                    release(PIN_I2CSDA);
                }
                else {
                    driveLow(PIN_I2CSDA);
                }

                delayMicroseconds(bitDelayUs);
                release(PIN_I2CSCL);
                delayMicroseconds(bitDelayUs);
                driveLow(PIN_I2CSCL);
                delayMicroseconds(bitDelayUs);
            }

            release(PIN_I2CSDA);
            delayMicroseconds(bitDelayUs);
            release(PIN_I2CSCL);
            delayMicroseconds(bitDelayUs);

            const bool ack = (digitalRead(PIN_I2CSDA) == LOW);

            driveLow(PIN_I2CSCL);
            delayMicroseconds(bitDelayUs);
            driveLow(PIN_I2CSDA);
            delayMicroseconds(bitDelayUs);
            release(PIN_I2CSCL);
            delayMicroseconds(bitDelayUs);
            release(PIN_I2CSDA);
            delayMicroseconds(bitDelayUs);

            Serial.print("    0x0");
            Serial.print(addr, HEX);
            Serial.print(": ");

            if (ack) {
                found++;
                Serial.println("*** ACK - SOMETHING IS HERE ***");
            }
            else {
                Serial.println("no response");
            }
        }

        if (found > 0) {
            Serial.println("  >>> A device answered in the reserved range. For a DuPPa encoder");
            Serial.println("      that means NO address jumpers are soldered (A0-A6 all open).");
            Serial.println("      Solder one jumper to move it to a legal address - A1+A2+A5+A6");
            Serial.println("      gives 0x66, which is what the ESP32 board used.");
        }

        init();
    }

    void compareWireVsBitBang(const uint8_t addr) {
        // The encoder answers a bit-banged probe at a RESERVED address. The
        // question that decides everything: can the Arduino Wire library - which
        // the encoder driver uses - actually address it there? Reserved
        // addresses have special meaning in the I2C spec (0x04-0x07 are the
        // Hs-mode master code), and a conforming library may refuse to send
        // them. If bit-bang works and Wire does not, no amount of driver code
        // will help and the address jumpers have to move.
        Serial.print("Comparing access methods at 0x0");
        Serial.print(addr, HEX);
        Serial.println(":");

        Wire.end();
        const bool bb = bitBangProbe(PIN_I2CSDA, PIN_I2CSCL, addr);
        init();

        Wire.beginTransmission(addr);
        const uint8_t wireResult = Wire.endTransmission();

        Serial.print("    bit-banged : ");
        Serial.println(bb ? "ACK" : "no response");
        Serial.print("    Wire library: ");
        Serial.print(wireResult == 0 ? "ACK" : "NACK/error ");

        if (wireResult != 0) {
            Serial.println(wireResult);
        }
        else {
            Serial.println();
        }

        if (bb && wireResult != 0) {
            Serial.println("    >>> The device IS there, but the Wire library cannot reach it at a");
            Serial.println("        reserved address. The encoder driver uses Wire, so it can never");
            Serial.println("        work here. Solder address jumpers to move the encoder to a legal");
            Serial.println("        address: A1+A2+A5+A6 gives 0x66.");
        }
        else if (bb && wireResult == 0) {
            Serial.println("    >>> Both work - the address is usable as-is.");
        }
    }

    void scanBothOrientations() {
        Serial.println("Testing both wire orientations:");
        scanBitBang(PIN_I2CSDA, PIN_I2CSCL, "SDA=18 SCL=19  (as wired/expected)");
        scanBitBang(PIN_I2CSCL, PIN_I2CSDA, "SDA=19 SCL=18  (REVERSED)");
        Serial.println("  If devices appear only in the REVERSED line, the two wires are swapped.");
    }

    void scanOtherPinPairs() {
        // The device might simply be on a different pair of pins. Teensy 4.1 has
        // three hardware I2C buses, and it is entirely possible to wire to the
        // wrong one. Bit-banging means any pair can be tested, both ways round,
        // without moving a single wire.
        Serial.println("Searching other Teensy I2C pin pairs:");

        scanBitBang(17, 16, "Wire1: SDA=17 SCL=16");
        scanBitBang(16, 17, "Wire1 reversed: SDA=16 SCL=17");
        scanBitBang(25, 24, "Wire2: SDA=25 SCL=24");
        scanBitBang(24, 25, "Wire2 reversed: SDA=24 SCL=25");

        Serial.println("  Anything found above means the device is wired to that pair.");
    }

    void reportPullupPins() {
        // Where are there pull-ups? A pin with an external pull-up reads high
        // with the internal one OFF; a bare pin floats and reads low. This finds
        // which pins actually have something attached, without any guessing.
        Serial.println("Pins reading HIGH with internal pull-ups disabled (something is pulling them up):");

        const uint8_t candidates[] = {14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29};
        bool any = false;

        for (const uint8_t pin : candidates) {
            // Skip pins already in use by other peripherals.
            if (pin == PIN_RTD_CS_BOILER || pin == PIN_RTD_CS_GROUP) {
                continue;
            }

            pinMode(pin, INPUT);
            delayMicroseconds(50);

            if (digitalRead(pin) == HIGH) {
                any = true;
                Serial.print("    pin ");
                Serial.println(pin);
            }
        }

        if (!any) {
            Serial.println("    none");
        }
    }

    namespace {

        bool intWatchActive = false;
        uint32_t intWatchUntilMs = 0;
        uint32_t intWatchEdges = 0;
        uint32_t intWatchStartMs = 0;
        int intWatchLastLevel = HIGH;

    } // namespace

    void watchEncoderInt(const uint32_t seconds) {
        pinMode(PIN_ENC_INT, INPUT_PULLUP);
        delayMicroseconds(100);

        intWatchActive = true;
        intWatchStartMs = millis();
        intWatchUntilMs = intWatchStartMs + (seconds * 1000);
        intWatchEdges = 0;
        intWatchLastLevel = digitalRead(PIN_ENC_INT);

        Serial.print("Watching encoder INT (pin ");
        Serial.print(PIN_ENC_INT);
        Serial.print(") for ");
        Serial.print(seconds);
        Serial.println("s - TURN AND PRESS THE KNOB NOW.");
        Serial.print("    idle level: ");
        Serial.println(intWatchLastLevel ? "HIGH (expected - INT is open-drain)" : "LOW (held down)");
    }

    void serviceEncoderIntWatch() {
        if (!intWatchActive) {
            return;
        }

        const int level = digitalRead(PIN_ENC_INT);

        if (level != intWatchLastLevel) {
            intWatchEdges++;
            intWatchLastLevel = level;

            if (intWatchEdges <= 20) {
                Serial.print("    ");
                Serial.print(millis() - intWatchStartMs);
                Serial.print(" ms  INT -> ");
                Serial.println(level ? "HIGH" : "LOW");
            }
        }

        if (millis() < intWatchUntilMs) {
            return;
        }

        intWatchActive = false;

        Serial.print("    total edges: ");
        Serial.println(intWatchEdges);

        if (intWatchEdges > 0) {
            Serial.println("    >>> The encoder IS alive and powered. INT responds to the knob,");
            Serial.println("        so the fault is confined to the I2C bus or its address.");
        }
        else {
            Serial.println("    >>> No INT activity. If the knob was moved, the encoder is not");
            Serial.println("        responding - check 3.3V and GND at the encoder itself.");
        }
    }

    void scan() {
        Serial.println("I2C scan (SDA 18, SCL 19), full 0x00-0x7F range:");

        int found = 0;

        // Reserved ranges first, bit-banged: the hardware peripheral refuses to
        // send these addresses, and this scanner used to skip them entirely.
        // That is textbook-correct and it hid a working device for hours - a
        // DuPPa encoder with no address jumpers soldered answers at 0x00-0x07.
        // A scanner that silently declines to look somewhere is worse than one
        // that looks and explains what it found.
        Wire.end();

        for (uint8_t addr = 0x00; addr <= 0x07; addr++) {
            if (bitBangProbe(PIN_I2CSDA, PIN_I2CSCL, addr)) {
                found++;
                Serial.print("    0x0");
                Serial.print(addr, HEX);
                Serial.print("  ");
                Serial.print(nameFor(addr));
                Serial.println("   [RESERVED - an unjumpered DuPPa encoder lands here]");
            }
        }

        for (uint8_t addr = 0x78; addr <= 0x7F; addr++) {
            if (bitBangProbe(PIN_I2CSDA, PIN_I2CSCL, addr)) {
                found++;
                Serial.print("    0x");
                Serial.print(addr, HEX);
                Serial.println("   [RESERVED]");
            }
        }

        init(); // hardware bus back for the normal range

        for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
            Wire.beginTransmission(addr);

            if (Wire.endTransmission() == 0) {
                found++;
                Serial.print("    0x");

                if (addr < 0x10) {
                    Serial.print("0");
                }

                Serial.print(addr, HEX);
                Serial.print("  ");
                Serial.println(nameFor(addr));
            }
        }

        if (found == 0) {
            Serial.println("    no devices found.");
        }
        else {
            Serial.print("    ");
            Serial.print(found);
            Serial.println(" device(s).");
        }
    }

} // namespace i2c
