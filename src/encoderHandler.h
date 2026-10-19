/**
 * @file encoderHandler.h
 *
 * @brief Duppa I2C rotary encoder (i2cEncoderLibV2): init, event logging, and
 * RGB status-LED color driven by the machine's current state.
 *
 * firmware-plan.md item 9. Rotation/press/release/double-press events are
 * logged only, not yet wired to any action - gesture-mapped brew-profile
 * switching and manual/auto pump control depend on features that don't exist
 * yet (profiles, variable-pump control) and are deliberately out of scope
 * here.
 *
 * Everything hardware-specific (whether the encoder is fitted at all, its
 * I2C address, whether it's the RGB-LED variant, whether status colors are
 * wanted, and what each status color is) is read from the web config at
 * startup - see "Encoder" section - so none of it needs a recompile.
 */
#pragma once

#include <i2cEncoderLibV2.h>
#include <vector>

inline i2cEncoderLibV2* duppaEncoder = nullptr;

// On-demand I2C bus scan, so the web page can show what's actually out there instead of
// the address having to be found with a logic analyzer or worked out from a jumper chart.
// Triggered by the web UI (sets i2cScanRequested) and run from encoderLoop() - I2C access
// has to stay on the main loop's task, not the web server's, since nothing else on the bus
// (the encoder itself, the PCF8574 button board) is protected by a lock.
inline bool i2cScanRequested = false;
inline bool i2cScanInProgress = false;
inline bool i2cScanHasRun = false;
inline std::vector<uint8_t> i2cScanResults;

inline void performI2CScan() {
    i2cScanInProgress = true;
    i2cScanResults.clear();

    for (uint8_t addr = 1; addr < 127; addr++) {
        Wire.beginTransmission(addr);

        if (Wire.endTransmission() == 0) {
            i2cScanResults.push_back(addr);
        }
    }

    i2cScanHasRun = true;
    i2cScanInProgress = false;
    i2cScanRequested = false;

    LOGF(INFO, "I2C scan complete, %d device(s) found", i2cScanResults.size());
}

inline bool encoderColorEffectsEnabled = false;
inline uint32_t encoderColorIdle = 0x101010;
inline uint32_t encoderColorError = 0xFF0000;
inline uint32_t encoderColorHeating = 0xFF4000;
inline uint32_t encoderColorReady = 0x00FF00;
inline uint32_t encoderColorActive = 0x0000FF;

// Sentinel so the very first call in encoderLoop() always writes a color, even if the
// computed status happens to be 0x000000.
inline uint32_t encoderLastColor = 0xFFFFFFFF;

inline void i2cBusRecovery() {
    // Classic I2C bus-recovery sequence: if a slave or the ESP32's own I2C peripheral is
    // wedged from a prior failed transaction, manually clocking SCL a few times with SDA
    // released forces it to reset. Must run BEFORE Wire.begin() claims these pins for
    // hardware I2C. Cheap and harmless to always run.
    pinMode(PIN_I2CSDA, INPUT_PULLUP);
    pinMode(PIN_I2CSCL, OUTPUT);

    for (int i = 0; i < 9; i++) {
        digitalWrite(PIN_I2CSCL, LOW);
        delayMicroseconds(5);
        digitalWrite(PIN_I2CSCL, HIGH);
        delayMicroseconds(5);
    }

    // Generate a STOP condition (SDA low->high while SCL is high) to leave the bus idle-clean.
    pinMode(PIN_I2CSDA, OUTPUT);
    digitalWrite(PIN_I2CSDA, LOW);
    delayMicroseconds(5);
    digitalWrite(PIN_I2CSCL, HIGH);
    delayMicroseconds(5);
    digitalWrite(PIN_I2CSDA, HIGH);
    delayMicroseconds(5);
}

/**
 * @brief Map the machine's current state to an RGB status color.
 */
inline uint32_t encoderStatusColor() {
    switch (machineState) {
        case kSensorError:
        case kEmergencyStop:
        case kWaterTankEmpty:
            return encoderColorError;

        case kInit:
        case kPidDisabled:
        case kStandby:
            return encoderColorIdle;

        case kBrew:
        case kManualFlush:
        case kHotWater:
        case kSteam:
        case kBackflush:
            return encoderColorActive;

        case kPidNormal:
        default:
            return (setpoint - temperature > 5.) ? encoderColorHeating : encoderColorReady;
    }
}

/**
 * @brief Push the current status color to the encoder's RGB LED, but only on change -
 * avoids hammering the I2C bus with a write every single loop iteration. No-op if the
 * encoder isn't the RGB variant, or status colors are turned off in config.
 */
inline void encoderUpdateStatusColor() {
    if (!encoderColorEffectsEnabled) {
        return;
    }

    const uint32_t color = encoderStatusColor();

    if (color != encoderLastColor) {
        duppaEncoder->writeRGBCode(color);
        encoderLastColor = color;
    }
}

inline void encoderSetup() {
    if (!config.get<bool>("hardware.encoder.enabled")) {
        LOG(INFO, "Rotary encoder disabled in config, skipping init");
        return;
    }

    const int i2cAddress = config.get<int>("hardware.encoder.i2c_address");
    const bool rgbLed = config.get<bool>("hardware.encoder.rgb_led");

    duppaEncoder = new i2cEncoderLibV2(i2cAddress);

    uint16_t initFlags = i2cEncoderLibV2::INT_DATA | i2cEncoderLibV2::WRAP_DISABLE | i2cEncoderLibV2::DIRE_RIGHT | i2cEncoderLibV2::IPUP_ENABLE | i2cEncoderLibV2::RMOD_X1;

    if (rgbLed) {
        initFlags |= i2cEncoderLibV2::RGB_ENCODER;
    }

    duppaEncoder->begin(initFlags);

    if (rgbLed) {
        encoderColorEffectsEnabled = config.get<bool>("hardware.encoder.color_effects.enabled");
        encoderColorIdle = config.get<int>("hardware.encoder.color.idle");
        encoderColorError = config.get<int>("hardware.encoder.color.error");
        encoderColorHeating = config.get<int>("hardware.encoder.color.heating");
        encoderColorReady = config.get<int>("hardware.encoder.color.ready");
        encoderColorActive = config.get<int>("hardware.encoder.color.active");

        const uint32_t bootColor = encoderColorEffectsEnabled ? encoderColorIdle : 0x000000;
        duppaEncoder->writeRGBCode(bootColor);
        encoderLastColor = bootColor;
    }

    LOGF(INFO, "Duppa encoder initialized at I2C address 0x%02X (RGB LED: %s, status colors: %s)", i2cAddress, rgbLed ? "yes" : "no", encoderColorEffectsEnabled ? "on" : "off");
}

inline void encoderLoop() {
    if (i2cScanRequested) {
        performI2CScan();
    }

    if (duppaEncoder == nullptr) {
        return;
    }

    duppaEncoder->updateStatus();
    encoderUpdateStatusColor();

    if (duppaEncoder->readStatus(i2cEncoderLibV2::RINC)) {
        LOGF(DEBUG, "Encoder: rotate +, counter = %d", duppaEncoder->readCounterByte());
    }

    if (duppaEncoder->readStatus(i2cEncoderLibV2::RDEC)) {
        LOGF(DEBUG, "Encoder: rotate -, counter = %d", duppaEncoder->readCounterByte());
    }

    if (duppaEncoder->readStatus(i2cEncoderLibV2::PUSHP)) {
        LOG(DEBUG, "Encoder: button pressed");
    }

    if (duppaEncoder->readStatus(i2cEncoderLibV2::PUSHR)) {
        LOG(DEBUG, "Encoder: button released");
    }

    if (duppaEncoder->readStatus(i2cEncoderLibV2::PUSHD)) {
        LOG(DEBUG, "Encoder: double-press detected");
    }
}
