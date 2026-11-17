/**
 * @file es8311.cpp
 *
 * @brief Minimal ES8311 codec driver for the LCDwiki 4.0" ESP32-S3 board
 */

#include "es8311.h"

#include <Arduino.h>
#include <LovyanGFX.hpp>

namespace es8311 {

    namespace {

        /**
         * I2C port 0 - the same one LovyanGFX opened for the touch panel, on
         * the same pins (IO16 SDA, IO15 SCL). Deliberately reusing that port
         * rather than starting Wire on the same pins: two I2C drivers sharing
         * two wires is a reliable source of unreliable behaviour.
         */
        constexpr int kI2cPort = 0;
        constexpr uint32_t kI2cFreq = 100000; // the codec is in no hurry

        // --- Register map (ES8311 datasheet names, as in esp_codec_dev) -----
        constexpr uint8_t kRegReset = 0x00;
        constexpr uint8_t kRegClk01 = 0x01;
        constexpr uint8_t kRegClk02 = 0x02;
        constexpr uint8_t kRegClk03 = 0x03;
        constexpr uint8_t kRegClk04 = 0x04;
        constexpr uint8_t kRegClk05 = 0x05;
        constexpr uint8_t kRegClk06 = 0x06;
        constexpr uint8_t kRegClk07 = 0x07;
        constexpr uint8_t kRegClk08 = 0x08;
        constexpr uint8_t kRegSdpIn09 = 0x09;
        constexpr uint8_t kRegSdpOut0A = 0x0A;
        constexpr uint8_t kRegSystem0B = 0x0B;
        constexpr uint8_t kRegSystem0C = 0x0C;
        constexpr uint8_t kRegSystem0D = 0x0D;
        constexpr uint8_t kRegSystem0E = 0x0E;
        constexpr uint8_t kRegSystem10 = 0x10;
        constexpr uint8_t kRegSystem11 = 0x11;
        constexpr uint8_t kRegSystem12 = 0x12;
        constexpr uint8_t kRegSystem13 = 0x13;
        constexpr uint8_t kRegSystem14 = 0x14;
        constexpr uint8_t kRegAdc15 = 0x15;
        constexpr uint8_t kRegAdc16 = 0x16;
        constexpr uint8_t kRegAdc17 = 0x17;
        constexpr uint8_t kRegAdc1B = 0x1B;
        constexpr uint8_t kRegAdc1C = 0x1C;
        constexpr uint8_t kRegDacMute31 = 0x31;
        constexpr uint8_t kRegDacVol32 = 0x32;
        constexpr uint8_t kRegDacRamp37 = 0x37;
        constexpr uint8_t kRegGpio44 = 0x44;
        constexpr uint8_t kRegGp45 = 0x45;
        constexpr uint8_t kRegChipId1 = 0xFD;
        constexpr uint8_t kRegChipId2 = 0xFE;

        uint8_t foundAddress = 0;
        bool configured = false;
        uint8_t currentVolume = kVolume0dB;
        uint8_t chipId1 = 0;
        uint8_t chipId2 = 0;
        bool ampOn = false;

        /** Count of register writes that were not acknowledged. */
        uint32_t writeFailures = 0;

        bool writeReg(uint8_t reg, uint8_t value) {
            const auto res = lgfx::i2c::writeRegister8(kI2cPort, foundAddress, reg, value, 0, kI2cFreq);

            if (res.has_error()) {
                ++writeFailures;

                return false;
            }

            return true;
        }

        /**
         * @brief Read a register. Returns false on a bus error.
         */
        bool readReg(uint8_t reg, uint8_t* value) {
            const auto res = lgfx::i2c::readRegister8(kI2cPort, foundAddress, reg, kI2cFreq);

            if (res.has_error()) {
                return false;
            }

            *value = res.value();

            return true;
        }

        /**
         * @brief Read-modify-write: keep the bits in `keep`, then OR in `set`.
         *
         * Used only where the existing contents genuinely matter. Where the
         * whole register is determined by this configuration, a plain write is
         * used instead - it is clearer, and it cannot carry over a stale bit
         * from whatever ran before.
         */
        bool updateReg(uint8_t reg, uint8_t keep, uint8_t set) {
            uint8_t current = 0;

            if (!readReg(reg, &current)) {
                ++writeFailures;

                return false;
            }

            return writeReg(reg, static_cast<uint8_t>((current & keep) | set));
        }

        bool deviceResponds(uint8_t addr) {
            // Chip ID registers are read-only and always present, so a
            // successful read here means a real ES8311 rather than just
            // something that happens to acknowledge its address.
            const auto res = lgfx::i2c::readRegister8(kI2cPort, addr, kRegChipId1, kI2cFreq);

            return res.has_value();
        }

    } // namespace

    bool probe() {
        writeFailures = 0;

        for (const uint8_t addr : {kAddrLow, kAddrHigh}) {
            if (deviceResponds(addr)) {
                foundAddress = addr;

                readReg(kRegChipId1, &chipId1);
                readReg(kRegChipId2, &chipId2);

                return true;
            }
        }

        foundAddress = 0;

        return false;
    }

    uint8_t address() {
        return foundAddress;
    }

    void ampEnable(bool enable) {
        pinMode(kPinAmpEnable, OUTPUT);

        // LOW enables the FM8002E, HIGH shuts it down (LCDwiki: "low level
        // enable, high level disable").
        digitalWrite(kPinAmpEnable, enable ? LOW : HIGH);
        ampOn = enable;
    }

    bool begin() {
        if (foundAddress == 0) {
            return false;
        }

        writeFailures = 0;
        configured = false;

        // The amplifier must be down before the codec is touched. Powering the
        // DAC up with the amplifier live puts the whole power-up transient
        // through the speaker as a thump.
        ampEnable(false);

        // --- Reset and basic power-up -----------------------------------
        //
        // Straight from the esp_codec_dev open() sequence. Register 0x44 is
        // written twice deliberately: that driver notes the ES8311's first I2C
        // write occasionally fails, and repeating it is cheaper than finding
        // out the hard way.
        writeReg(kRegSystem0D, 0xFA);
        writeReg(kRegGpio44, 0x08);
        writeReg(kRegGpio44, 0x08);
        writeReg(kRegClk01, 0x30);
        writeReg(kRegClk02, 0x00);
        writeReg(kRegClk03, 0x10);
        writeReg(kRegAdc16, 0x24);
        writeReg(kRegClk04, 0x10);
        writeReg(kRegClk05, 0x00);
        writeReg(kRegSystem0B, 0x00);
        writeReg(kRegSystem0C, 0x00);
        writeReg(kRegSystem10, 0x1F);
        writeReg(kRegSystem11, 0x7F);

        // 0x80 with bit 6 clear = SLAVE mode. The ESP32-S3 generates MCLK,
        // BCLK and LRCK; the codec follows. This is the single most important
        // bit in the file - set the other way the codec would try to drive the
        // same clock lines the ESP32-S3 is driving.
        writeReg(kRegReset, 0x80);

        // 0x3F with bit 7 clear = take MCLK from the MCLK pin (IO4) rather
        // than deriving it from BCLK, and bit 6 clear = do not invert it.
        writeReg(kRegClk01, 0x3F);

        // BCLK not inverted. Read-modify-write because the rest of 0x06 is the
        // BCLK divider, set further down.
        updateReg(kRegClk06, static_cast<uint8_t>(~0x20), 0x00);

        writeReg(kRegSystem13, 0x10);
        writeReg(kRegAdc1B, 0x0A);
        writeReg(kRegAdc1C, 0x6A);

        // 0x08 leaves the DAC-to-ADC internal reference off. The reference
        // driver would set 0x58 here to route DAC output back into the ADC,
        // which is for loopback testing and is not wanted for playback.
        writeReg(kRegGpio44, 0x08);

        // --- Serial format: 16-bit, standard I2S -------------------------
        //
        // 0x0C in the low bits of 0x09/0x0A selects 16-bit samples; leaving the
        // bottom two bits clear selects standard (Philips) I2S framing rather
        // than left-justified or DSP mode.
        writeReg(kRegSdpIn09, 0x0C);
        writeReg(kRegSdpOut0A, 0x0C);

        // --- Clocking for 44.1 kHz with MCLK = 256 x 44100 = 11.2896 MHz --
        //
        // Coefficients for {mclk 11289600, rate 44100} from the esp_codec_dev
        // table: pre_div 1, pre_multi 1, adc_div 1, dac_div 1, single-speed,
        // LRCK divider 256, BCLK divider 4, oversample 0x10.
        //
        // All the dividers are 1, which is the reason for choosing 256 x fs:
        // the codec runs straight off MCLK with nothing in between, so there is
        // no divider arithmetic to get wrong.
        writeReg(kRegClk02, 0x00); // pre-divider 1, pre-multiplier x1
        writeReg(kRegClk05, 0x00); // ADC and DAC clock dividers both 1
        writeReg(kRegClk03, 0x10); // single speed, ADC oversample 0x10
        writeReg(kRegClk04, 0x10); // DAC oversample 0x10
        writeReg(kRegClk07, 0x00); // LRCK divider, high bits
        writeReg(kRegClk08, 0xFF); // LRCK divider, low bits -> 256

        // BCLK divider 4. Only used when the codec is BCLK master, which it is
        // not here, so this has no effect - written anyway to keep the register
        // set self-consistent rather than half-configured.
        updateReg(kRegClk06, 0xE0, 0x03);

        // --- Start the DAC ----------------------------------------------
        writeReg(kRegReset, 0x80);
        writeReg(kRegClk01, 0x3F);
        writeReg(kRegSdpIn09, 0x0C); // bit 6 clear: DAC serial port awake
        writeReg(kRegSdpOut0A, 0x0C);
        writeReg(kRegAdc17, 0xBF);
        writeReg(kRegSystem0E, 0x02);
        writeReg(kRegSystem12, 0x00); // enable DAC
        writeReg(kRegSystem14, 0x1A); // analogue PGA; digital mic off
        writeReg(kRegSystem0D, 0x01); // power up
        writeReg(kRegAdc15, 0x40);
        writeReg(kRegDacRamp37, 0x08); // DAC ramp rate: ramps volume changes
        writeReg(kRegGp45, 0x00);

        setVolume(currentVolume);
        setMute(false);

        configured = (writeFailures == 0);

        return configured;
    }

    bool setVolume(uint8_t reg) {
        currentVolume = reg;

        if (foundAddress == 0) {
            return false;
        }

        return writeReg(kRegDacVol32, reg);
    }

    bool setMute(bool mute) {
        if (foundAddress == 0) {
            return false;
        }

        // Bits 6:5 of 0x31 are the mute control; everything else in there is
        // DAC configuration that must survive.
        return updateReg(kRegDacMute31, 0x9F, mute ? 0x60 : 0x00);
    }

    void suspend() {
        ampEnable(false);

        if (foundAddress == 0) {
            return;
        }

        // The esp_codec_dev suspend sequence, verbatim. The order matters: the
        // analogue side is shut down before the clocks stop, which is what
        // keeps the power-down quiet.
        writeReg(kRegDacVol32, 0x00);
        writeReg(kRegAdc17, 0x00);
        writeReg(kRegSystem0E, 0xFF);
        writeReg(kRegSystem12, 0x02);
        writeReg(kRegSystem14, 0x00);
        writeReg(kRegSystem0D, 0xFA);
        writeReg(kRegAdc15, 0x00);
        writeReg(kRegClk02, 0x10);
        writeReg(kRegReset, 0x00);
        writeReg(kRegReset, 0x1F);
        writeReg(kRegClk01, 0x30);
        writeReg(kRegClk01, 0x00);
        writeReg(kRegGp45, 0x00);
        writeReg(kRegSystem0D, 0xFC);
        writeReg(kRegClk02, 0x00);

        configured = false;
    }

    void printStatus() {
        Serial.print("codec: ");

        if (foundAddress == 0) {
            Serial.println("ES8311 NOT FOUND on I2C (expected 0x18 or 0x19) - no sound");

            return;
        }

        Serial.print("ES8311 at 0x");
        Serial.print(foundAddress, HEX);
        Serial.print(", chip id 0x");
        Serial.print(chipId1, HEX);
        Serial.print(chipId2, HEX);
        Serial.print(", ");
        Serial.print(configured ? "configured" : "NOT configured");
        Serial.print(", volume 0x");
        Serial.print(currentVolume, HEX);
        Serial.print(" (");
        Serial.print((static_cast<int>(currentVolume) - 0xBF) / 2.0f, 1);
        Serial.print(" dB), amp ");
        Serial.println(ampOn ? "enabled" : "shut down");

        if (writeFailures > 0) {
            Serial.print("codec: ");
            Serial.print(writeFailures);
            Serial.println(" register writes were not acknowledged");
        }
    }

    void dumpRegisters() {
        if (foundAddress == 0) {
            Serial.println("codec: not found, nothing to dump");

            return;
        }

        /**
         * The registers begin() sets, with the value it asks for. Anything
         * reading back differently is the interesting result - either the
         * codec rejected it, or something else is writing the bus.
         *
         * 0x31 and 0x06 are deliberately absent: both are read-modify-write,
         * so their final value depends on reset defaults rather than on a
         * number chosen here.
         */
        struct Expected {
                uint8_t reg;
                uint8_t value;
                const char* what;
        };

        static const Expected expected[] = {
            {0x00, 0x80, "reset/mode    (0x80 = slave)"},
            {0x01, 0x3F, "clock manager (MCLK from pin)"},
            {0x02, 0x00, "pre-div/mult  (both 1)"},
            {0x03, 0x10, "ADC osr"},
            {0x04, 0x10, "DAC osr"},
            {0x05, 0x00, "ADC/DAC div   (both 1)"},
            {0x07, 0x00, "LRCK div high"},
            {0x08, 0xFF, "LRCK div low  (-> 256)"},
            {0x09, 0x0C, "DAC serial    (16-bit I2S)"},
            {0x0A, 0x0C, "ADC serial    (16-bit I2S)"},
            {0x0D, 0x01, "power up"},
            {0x0E, 0x02, "analogue power"},
            {0x12, 0x00, "DAC enable"},
            {0x14, 0x1A, "analogue PGA"},
            {0x32, currentVolume, "DAC volume"},
            {0x37, 0x08, "DAC ramp rate"},
            {0x44, 0x08, "GPIO/DAC2ADC off"},
        };

        Serial.print("codec registers at 0x");
        Serial.println(foundAddress, HEX);

        int mismatches = 0;

        for (const auto& e : expected) {
            uint8_t actual = 0;

            Serial.print("  0x");

            if (e.reg < 0x10) {
                Serial.print('0');
            }

            Serial.print(e.reg, HEX);
            Serial.print("  ");

            if (!readReg(e.reg, &actual)) {
                Serial.println("READ FAILED");
                ++mismatches;

                continue;
            }

            Serial.print("= 0x");

            if (actual < 0x10) {
                Serial.print('0');
            }

            Serial.print(actual, HEX);
            Serial.print("  ");
            Serial.print(e.what);

            if (actual != e.value) {
                Serial.print("   <-- MISMATCH, wrote 0x");
                Serial.print(e.value, HEX);
                ++mismatches;
            }

            Serial.println();
        }

        // The mute bits, read-modify-write so shown separately.
        uint8_t mute = 0;

        if (readReg(0x31, &mute)) {
            Serial.print("  0x31  = 0x");
            Serial.print(mute, HEX);
            Serial.print("  DAC mute bits ");
            Serial.println((mute & 0x60) != 0 ? "SET (muted)" : "clear (unmuted)");
        }

        Serial.print("codec: ");
        Serial.print(mismatches);
        Serial.println(" register(s) did not read back as written");
    }

} // namespace es8311
