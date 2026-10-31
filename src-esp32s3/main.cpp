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
#include "link.h"

namespace {

    /**
     * Link to the Teensy. Serial1 is remapped onto the board's UART0 header
     * pins; the USB console is a separate device and is unaffected.
     */
    constexpr uint32_t kLinkBaud = 115200;

    constexpr uint32_t kHeartbeatMs = 2000;
    uint32_t lastHeartbeat = 0;

    bool lcdReady = false;

    /** Redraw rate. The panel is on SPI, telemetry arrives at 5 Hz, and
     *  repainting faster than the data changes only causes flicker. */
    constexpr uint32_t kRedrawMs = 250;
    uint32_t lastRedraw = 0;

    /**
     * @brief Tiny serial console, so display orientation and inversion can be
     *        tuned interactively instead of through a reflash each time.
     */
    void handleCommand(const String& line) {
        if (line.startsWith("rot")) {
            const int r = line.substring(3).toInt();
            display::setRotation(static_cast<uint8_t>(r));
            Serial.printf("rotation %d  (%dx%d)\n", display::rotation(), display::lcd.width(), display::lcd.height());
        }
        else if (line == "inv") {
            display::toggleInvert();
            Serial.println("inversion toggled");
        }
        else if (line.startsWith("bl")) {
            const int b = line.substring(2).toInt();
            display::setBrightness(static_cast<uint8_t>(b));
            Serial.printf("brightness %d\n", b);
        }
        else if (line.startsWith("cmd ")) {
            const String body = line.substring(4);
            machinelink::sendCommand(body.c_str());
            Serial.printf("sent: %s\n", body.c_str());
        }
        else if (line == "redraw") {
            display::showBringupScreen();
            Serial.println("redrawn");
        }
        else if (line == "help" || line.isEmpty()) {
            Serial.println("commands: rot <0-3> | inv | bl <0-255> | redraw | cmd <body>");
            Serial.println("  cmd ping=1        ask the Teensy to reply");
            Serial.println("  cmd flowreset=1   zero the flow accumulator");
            Serial.println("  rot 0,2 = portrait 320x480   rot 1,3 = landscape 480x320");
        }
        else {
            Serial.printf("unknown: %s\n", line.c_str());
        }
    }

    void serviceConsole() {
        static String line;

        while (Serial.available() > 0) {
            const char c = static_cast<char>(Serial.read());

            if (c == '\r') {
                continue;
            }

            if (c == '\n') {
                line.trim();
                handleCommand(line);
                line = "";

                continue;
            }

            if (line.length() < 32) {
                line += c;
            }
        }
    }

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
    machinelink::init();

    if (display::init()) {
        Serial.println("display: ST7796S initialised");
        lcdReady = true;
        display::lcd.fillScreen(TFT_BLACK);
        Serial.printf("display: %dx%d, rotation %d\n", display::lcd.width(), display::lcd.height(), display::rotation());
        Serial.println("commands: rot <0-3> | inv | bl <0-255> | redraw");
    }
    else {
        Serial.println("display: INIT FAILED - check the SPI pins against the LCDwiki page");
    }
}

void loop() {
    serviceConsole();
    machinelink::loop();

    if (lcdReady) {
        if (const uint32_t now = millis(); now - lastRedraw >= kRedrawMs) {
            lastRedraw = now;
            display::drawMachineScreen();
        }
    }

    if (const uint32_t now = millis(); now - lastHeartbeat >= kHeartbeatMs) {
        lastHeartbeat = now;

        Serial.printf("alive, uptime %lus  link=%s frames=%lu badsum=%lu\n",
                      static_cast<unsigned long>(now / 1000),
                      machinelink::isConnected() ? "OK" : "stale",
                      static_cast<unsigned long>(machinelink::framesReceived()),
                      static_cast<unsigned long>(machinelink::badChecksums()));
    }
}
