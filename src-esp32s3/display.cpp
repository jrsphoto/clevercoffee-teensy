/**
 * @file display.cpp
 *
 * @brief LovyanGFX bring-up for the 4.0" ESP32-S3 board
 */

#include "display.h"

namespace display {

    LGFX lcd;

    namespace {

        int32_t lastTouchX = -1;
        int32_t lastTouchY = -1;
        uint32_t touchCount = 0;

        constexpr int kStatusY = 300;

    } // namespace

    bool init() {
        if (!lcd.init()) {
            return false;
        }

        lcd.setRotation(0);
        lcd.setBrightness(200);
        lcd.fillScreen(TFT_BLACK);

        return true;
    }

    void showBringupScreen() {
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
        lcd.printf("panel  %dx%d  ST7796S 4-line SPI", lcd.width(), lcd.height());
        lcd.setCursor(8, 66);
        lcd.printf("touch  FT6336U  SDA%d SCL%d", kPinTouchSda, kPinTouchScl);
        lcd.setCursor(8, 78);
        lcd.printf("link   UART0  RX%d TX%d -> Teensy", kPinLinkRx, kPinLinkTx);

        // Colour bars, so a wrong colour order or a stuck bit is obvious at a
        // glance rather than needing a test pattern later.
        const uint16_t bars[] = {TFT_RED, TFT_GREEN, TFT_BLUE, TFT_YELLOW, TFT_MAGENTA, TFT_CYAN, TFT_WHITE};
        const int barW = lcd.width() / 7;

        for (int i = 0; i < 7; i++) {
            lcd.fillRect(i * barW, 100, barW, 40, bars[i]);
        }

        lcd.setTextColor(TFT_WHITE, TFT_BLACK);
        lcd.setCursor(8, 155);
        lcd.println("Touch anywhere - a dot follows your finger.");

        lcd.drawRect(4, 170, lcd.width() - 8, 110, TFT_DARKGREY);

        lcd.setCursor(8, kStatusY);
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

            lcd.fillRect(0, kStatusY, lcd.width(), 12, TFT_BLACK);
            lcd.setCursor(8, kStatusY);
            lcd.setTextColor(TFT_GREENYELLOW, TFT_BLACK);
            lcd.printf("touch: x=%3d y=%3d   count=%lu", static_cast<int>(x), static_cast<int>(y), static_cast<unsigned long>(touchCount));

            Serial.printf("touch x=%d y=%d\n", static_cast<int>(x), static_cast<int>(y));
        }

        // Dot inside the box only, so the readout and header stay legible.
        if (y > 172 && y < 278) {
            lcd.fillCircle(x, y, 3, TFT_ORANGE);
        }
    }

} // namespace display
