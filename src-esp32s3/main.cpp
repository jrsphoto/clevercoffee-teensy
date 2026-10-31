/**
 * @file main.cpp
 *
 * @brief ESP32-S3 touch-display HEAD for the Argenta SME espresso machine
 *
 * This board is a terminal, not a controller. It owns WiFi, the web UI and the
 * colour touch LCD. It owns NO actuators and NO safety. A touch on its screen is
 * a request sent to the Teensy, which decides what to do about it.
 *
 * The link is deliberately non-critical: if this board crashes, is unplugged, or
 * is never fitted at all, the Teensy runs the machine and finishes shots exactly
 * as normal. Nothing here may ever become something the machine depends on.
 *
 * Hardware: LCDwiki / Hosyond 4.0" ESP32-S3 Display
 *   ST7796S 320x480 IPS over 4-line SPI, FT6336U capacitive touch,
 *   ESP32-S3-N16R8 (16 MB flash, 8 MB OPI PSRAM).
 *
 * Serial arrangement, which matters:
 *   - USB-CDC is the console (ARDUINO_USB_CDC_ON_BOOT=1)
 *   - UART0 on IO43/IO44 is the Teensy link
 * The S3's native USB is what frees UART0 for the link. Without it, boot chatter
 * would be injected straight into the Teensy's RX1.
 */

#include <Arduino.h>

#include "display.h"

namespace {

    /**
     * Link to the Teensy. Serial1 is remapped onto the board's UART0 header
     * pins; the USB console is a separate device and is unaffected.
     */
    constexpr uint32_t kLinkBaud = 115200;

    constexpr uint32_t kHeartbeatMs = 2000;
    uint32_t lastHeartbeat = 0;

    void printBanner() {
        Serial.println();
        Serial.println("========================================");
        Serial.println("CleverCoffee ESP32-S3 display head");
        Serial.println("Argenta SME - head only, no actuators");
        Serial.println("========================================");
        Serial.printf("PSRAM: %u bytes free\n", static_cast<unsigned>(ESP.getFreePsram()));
        Serial.printf("Flash: %u bytes\n", static_cast<unsigned>(ESP.getFlashChipSize()));
        Serial.printf("Link : UART0 RX%d TX%d @ %lu baud\n", display::kPinLinkRx, display::kPinLinkTx, static_cast<unsigned long>(kLinkBaud));
    }

} // namespace

void setup() {
    Serial.begin(115200);

    const uint32_t waitStart = millis();

    while (!Serial && (millis() - waitStart) < 1500) {
        // Brief courtesy pause for a bench console; never block a real boot.
    }

    printBanner();

    // The link to the Teensy. Opened before the display so that, once the
    // protocol exists, a Teensy already running has somewhere to send to while
    // this board is still drawing its first frame.
    Serial1.begin(kLinkBaud, SERIAL_8N1, display::kPinLinkRx, display::kPinLinkTx);

    if (display::init()) {
        Serial.println("display: ST7796S initialised");
        display::showBringupScreen();
    }
    else {
        Serial.println("display: INIT FAILED - check the SPI pins against the LCDwiki page");
    }
}

void loop() {
    display::loop();

    // Anything arriving from the Teensy, echoed to the console. The real
    // protocol lands in the next phase; for now this proves the wire works in
    // one direction and gives something to watch while testing it.
    while (Serial1.available() > 0) {
        Serial.write(Serial1.read());
    }

    if (const uint32_t now = millis(); now - lastHeartbeat >= kHeartbeatMs) {
        lastHeartbeat = now;

        Serial.printf("alive, uptime %lus\n", static_cast<unsigned long>(now / 1000));
    }
}
