/**
 * @file display.cpp
 *
 * @brief LovyanGFX bring-up for the 4.0" ESP32-S3 board
 */

#include "display.h"

#include "link.h"

namespace display {

    LGFX lcd;

    namespace {

        int32_t lastTouchX = -1;
        int32_t lastTouchY = -1;
        uint32_t touchCount = 0;

        uint8_t currentRotation = 1; // 1 = landscape
        bool inverted = true;

        // Layout, computed from the panel's CURRENT dimensions so the screen is
        // correct in any rotation rather than only the one it was written in.
        int statusY = 0;
        int boxTop = 0;
        int boxBottom = 0;

    } // namespace

    bool init() {
        if (!lcd.init()) {
            return false;
        }

        lcd.setRotation(currentRotation);
        lcd.setBrightness(200);
        lcd.fillScreen(TFT_BLACK);

        return true;
    }

    void showBringupScreen() {
        const int w = lcd.width();
        const int h = lcd.height();

        // Vertical layout, derived rather than hand-placed, so nothing collides
        // when the rotation changes the available height.
        constexpr int kBarsTop = 98;
        constexpr int kBarsHeight = 34;

        statusY = h - 18;
        boxTop = kBarsTop + kBarsHeight + 20; // clear of the bars AND the caption
        boxBottom = statusY - 8;

        lcd.fillScreen(TFT_BLACK);

        lcd.setTextColor(TFT_WHITE, TFT_BLACK);
        lcd.setTextSize(2);
        lcd.setCursor(8, 8);
        lcd.println("CleverCoffee");

        lcd.setTextSize(1);
        lcd.setTextColor(TFT_CYAN, TFT_BLACK);
        lcd.setCursor(8, 34);
        lcd.println("ESP32-S3 display head - bring-up");

        lcd.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
        lcd.setCursor(8, 54);
        lcd.printf("panel  %dx%d  ST7796S  rot=%d  inv=%s", w, h, currentRotation, inverted ? "on" : "off");
        lcd.setCursor(8, 66);
        lcd.printf("touch  FT6336U  SDA%d SCL%d", kPinTouchSda, kPinTouchScl);
        lcd.setCursor(8, 78);
        lcd.printf("link   UART0  RX%d TX%d -> Teensy", kPinLinkRx, kPinLinkTx);

        // Colour bars, so a wrong colour order or a stuck bit is obvious at a
        // glance rather than needing a test pattern later.
        const uint16_t bars[] = {TFT_RED, TFT_GREEN, TFT_BLUE, TFT_YELLOW, TFT_MAGENTA, TFT_CYAN, TFT_WHITE};
        const int barW = w / 7;

        for (int i = 0; i < 7; i++) {
            lcd.fillRect(i * barW, kBarsTop, barW, kBarsHeight, bars[i]);
        }

        lcd.setTextColor(TFT_WHITE, TFT_BLACK);
        lcd.setCursor(8, boxTop - 14);
        lcd.println("Touch inside the box - a dot follows your finger.");

        lcd.drawRect(4, boxTop, w - 8, boxBottom - boxTop, TFT_DARKGREY);

        lcd.setCursor(8, statusY);
        lcd.setTextColor(TFT_GREENYELLOW, TFT_BLACK);
        lcd.println("touch: waiting...");
    }

    void loop() {
        int32_t x = 0;
        int32_t y = 0;

        if (!lcd.getTouch(&x, &y)) {
            return;
        }

        touchCount++;

        // Only redraw the readout when the position actually changes, so a
        // resting finger does not flicker the text.
        if (x != lastTouchX || y != lastTouchY) {
            lastTouchX = x;
            lastTouchY = y;

            lcd.fillRect(0, statusY, lcd.width(), 14, TFT_BLACK);
            lcd.setCursor(8, statusY);
            lcd.setTextColor(TFT_GREENYELLOW, TFT_BLACK);
            lcd.printf("touch: x=%3d y=%3d   count=%lu", static_cast<int>(x), static_cast<int>(y), static_cast<unsigned long>(touchCount));

            Serial.printf("touch x=%d y=%d\n", static_cast<int>(x), static_cast<int>(y));
        }

        // Dot inside the box only, so the readout and header stay legible.
        if (y > (boxTop + 2) && y < (boxBottom - 2)) {
            lcd.fillCircle(x, y, 3, TFT_ORANGE);
        }
    }

    void setRotation(const uint8_t rot) {
        currentRotation = rot & 3;
        lcd.setRotation(currentRotation);
        lastTouchX = -1;
        lastTouchY = -1;
        showBringupScreen();
    }

    void toggleInvert() {
        inverted = !inverted;
        lcd.invertDisplay(inverted);
        showBringupScreen();
    }

    void setBrightness(const uint8_t level) {
        lcd.setBrightness(level);
    }

    uint8_t rotation() {
        return currentRotation;
    }

    void drawMachineScreen() {
        const auto& st = machinelink::state();
        const bool live = machinelink::isConnected();

        const int w = lcd.width();

        lcd.setTextSize(2);
        lcd.setTextColor(TFT_WHITE, TFT_BLACK);
        lcd.setCursor(8, 8);
        lcd.print("CleverCoffee   ");

        // Link state. Said plainly, because a stale readout that looks live is
        // the one genuinely dangerous thing a display can do here.
        lcd.setTextSize(1);
        lcd.setCursor(w - 110, 14);

        if (live) {
            lcd.setTextColor(TFT_GREEN, TFT_BLACK);
            lcd.print("LINK OK     ");
        }
        else {
            lcd.setTextColor(TFT_ORANGE, TFT_BLACK);
            lcd.print("LINK STALE  ");
        }

        // Boiler temperature, the number that matters most.
        lcd.setTextSize(1);
        lcd.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
        lcd.setCursor(12, 56);
        lcd.print("BOILER");

        lcd.setTextSize(4);
        lcd.setCursor(12, 70);

        if (live && st.boilerOk) {
            lcd.setTextColor(TFT_CYAN, TFT_BLACK);
            lcd.printf("%5.1f", st.boilerC);
        }
        else {
            lcd.setTextColor(TFT_RED, TFT_BLACK);
            lcd.print(" --.-");
        }

        lcd.setTextSize(2);
        lcd.setCursor(150, 84);
        lcd.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
        lcd.print("C");

        // Group temperature, smaller - it is a hint, not a control value.
        lcd.setTextSize(1);
        lcd.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
        lcd.setCursor(250, 56);
        lcd.print("GROUP");

        lcd.setTextSize(2);
        lcd.setCursor(250, 70);

        if (live && st.groupOk) {
            lcd.setTextColor(TFT_WHITE, TFT_BLACK);
            lcd.printf("%5.1f C", st.groupC);
        }
        else {
            lcd.setTextColor(TFT_DARKGREY, TFT_BLACK);
            lcd.print(" --.- C");
        }

        // Flow and delivered volume.
        lcd.setTextSize(1);
        lcd.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
        lcd.setCursor(12, 140);
        lcd.print("FLOW");
        lcd.setCursor(130, 140);
        lcd.print("VOLUME");
        lcd.setCursor(260, 140);
        lcd.print("ENCODER");

        lcd.setTextSize(2);
        lcd.setTextColor(live ? TFT_GREENYELLOW : TFT_DARKGREY, TFT_BLACK);
        lcd.setCursor(12, 154);
        lcd.printf("%4.1f ml/s", st.flowMlPerSec);
        lcd.setCursor(130, 154);
        lcd.printf("%5.1f ml", st.volumeMl);
        lcd.setCursor(260, 154);
        lcd.printf("%6ld  ", st.encoder);

        // Machine state and any latched fault.
        lcd.setTextSize(2);
        lcd.setCursor(12, 200);

        if (!live) {
            lcd.setTextColor(TFT_ORANGE, TFT_BLACK);
            lcd.printf("no telemetry (%lus)   ", static_cast<unsigned long>(machinelink::msSinceLastFrame() / 1000));
        }
        else if (strcmp(st.fault, "none") != 0) {
            lcd.setTextColor(TFT_RED, TFT_BLACK);
            lcd.printf("FAULT: %-16s", st.fault);
        }
        else {
            lcd.setTextColor(TFT_GREEN, TFT_BLACK);
            lcd.printf("state: %-16s", st.status);
        }

        // Frame counters, small, for bring-up confidence.
        lcd.setTextSize(1);
        lcd.setTextColor(TFT_DARKGREY, TFT_BLACK);
        lcd.setCursor(12, lcd.height() - 14);
        lcd.printf("frames %lu  badsum %lu        ",
                   static_cast<unsigned long>(machinelink::framesReceived()),
                   static_cast<unsigned long>(machinelink::badChecksums()));
    }

} // namespace display
