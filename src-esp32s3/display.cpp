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
        lcd.print("PUMP");

        lcd.setTextSize(2);
        lcd.setTextColor(live ? TFT_GREENYELLOW : TFT_DARKGREY, TFT_BLACK);
        lcd.setCursor(12, 154);
        lcd.printf("%4.1f ml/s", st.flowMlPerSec);
        lcd.setCursor(130, 154);
        lcd.printf("%5.1f ml", st.volumeMl);
        lcd.setCursor(260, 154);
        lcd.printf("%3.0f %%   ", st.pumpPercent);

        // Brew weight. Big, because during a shot it is the number being
        // watched - but labelled honestly: the unit is "g" only when the scale
        // has actually been calibrated, and "raw" otherwise. Showing raw ADC
        // counts next to a "g" would invite someone to trust a dose that is
        // nonsense, which is worse than showing nothing.
        lcd.setTextSize(1);
        lcd.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
        lcd.setCursor(12, 190);
        lcd.print("WEIGHT");

        // Left-aligned at a fixed left edge, with the unit immediately after
        // the digits. Getting here took three wrong turns worth recording,
        // because each one looked reasonable:
        //
        //   1. Unit at a fixed column sized for six characters. An eight
        //      character raw count overran it and "raw" sat on the digits.
        //   2. Unit positioned from getCursorX(), then the row tidied with a
        //      fillRect afterwards. The tidy-up clipped the label.
        //   3. Fixed-width RIGHT-aligned number. Nothing overlapped, but a
        //      short value floated off to the right, dragging the unit with it.
        //
        // The actual requirement is: number pinned left, unit adjacent, no
        // leftovers from a wider previous value. That needs the row cleared
        // BEFORE drawing rather than patched afterwards - and to avoid the
        // flicker a 4 Hz clear-and-repaint would cause, the whole thing is
        // skipped unless the text has changed since the last frame.
        constexpr int kWeightX = 12;
        constexpr int kWeightY = 204;
        /**
         * Deliberately size 3, between the two scales already on this screen.
         *
         * At size 4 it matched the boiler temperature and competed with it, and
         * the boiler is the number that actually matters when the machine is
         * idle. At size 2 it would disappear into the flow/volume/encoder row,
         * and brew weight is more important than any of those once a shot is
         * running. Three is the honest middle.
         */
        constexpr int kWeightSize = 3;
        constexpr int kWeightRowHeight = 8 * kWeightSize; // glyph is 8 px tall

        char weightText[16];
        const char* weightUnit = "g";
        uint16_t weightColour = TFT_YELLOW;

        if (live && st.weightOk) {
            if (st.weightCalibrated) {
                snprintf(weightText, sizeof(weightText), "%.1f", st.weightG);
            }
            else {
                // Raw ADC counts, printed whole - a tenth of a count is not a
                // real quantity. Orange and spelled out, so an uncalibrated
                // reading cannot be mistaken for a dose.
                snprintf(weightText, sizeof(weightText), "%.0f", st.weightG);
                weightUnit = "raw";
                weightColour = TFT_ORANGE;
            }
        }
        else {
            snprintf(weightText, sizeof(weightText), "--.-");
            weightColour = TFT_DARKGREY;
        }

        static char lastWeightText[16] = "";
        static char lastWeightUnit[8] = "";

        // The clear below only covers the current font's row height, so a
        // change in text SIZE between builds could leave taller glyphs behind.
        // One full-height wipe on the first pass deals with that.
        static bool weightRowWiped = false;

        if (!weightRowWiped) {
            weightRowWiped = true;
            lcd.fillRect(kWeightX, kWeightY, lcd.width() - kWeightX, 8 * 4, TFT_BLACK);
        }

        if (strcmp(weightText, lastWeightText) != 0 || strcmp(weightUnit, lastWeightUnit) != 0) {
            snprintf(lastWeightText, sizeof(lastWeightText), "%s", weightText);
            snprintf(lastWeightUnit, sizeof(lastWeightUnit), "%s", weightUnit);

            lcd.fillRect(kWeightX, kWeightY, lcd.width() - kWeightX, kWeightRowHeight, TFT_BLACK);

            lcd.setTextSize(kWeightSize);
            lcd.setTextColor(weightColour, TFT_BLACK);
            lcd.setCursor(kWeightX, kWeightY);
            lcd.print(weightText);

            // Immediately after the digits, sitting on their baseline.
            const int unitX = lcd.getCursorX() + 8;

            lcd.setTextSize(2);
            lcd.setTextColor(weightColour == TFT_ORANGE ? TFT_ORANGE : TFT_LIGHTGREY, TFT_BLACK);
            lcd.setCursor(unitX, kWeightY + kWeightRowHeight - 16);
            lcd.print(weightUnit);
        }

        // Machine state and any latched fault.
        lcd.setTextSize(2);
        lcd.setCursor(12, 260);

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
