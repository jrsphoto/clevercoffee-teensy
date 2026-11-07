/**
 * @file display.cpp
 *
 * @brief LovyanGFX bring-up for the 4.0" ESP32-S3 board
 */

#include "audio.h"
#include "display.h"

#include "link.h"
#include "net.h"

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

    void showMessage(const char* title, const char* line1, const char* line2) {
        lcd.fillScreen(TFT_BLACK);

        lcd.setTextColor(TFT_WHITE, TFT_BLACK);
        lcd.setTextSize(3);
        lcd.setCursor(12, 28);
        lcd.println(title);

        // Size 3 for the body too: these lines are an SSID and an IP address
        // that somebody is about to type into a phone, so they have to be
        // readable from where a person actually stands, not just legible if
        // you lean in.
        lcd.setTextColor(TFT_CYAN, TFT_BLACK);
        lcd.setCursor(12, 96);
        lcd.println(line1);

        lcd.setTextColor(TFT_YELLOW, TFT_BLACK);
        lcd.setCursor(12, 144);
        lcd.println(line2);

        lcd.setTextSize(1);
        lcd.setTextColor(TFT_DARKGREY, TFT_BLACK);
        lcd.setCursor(12, 210);
        lcd.println("The machine runs normally without a network.");

        invalidate();
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

        const bool touching = lcd.getTouch(&x, &y);

        // Act on the RELEASE-to-press edge only. Without this a finger resting
        // on "+" would repeat at the loop rate, which for a setpoint button
        // means several degrees before you lift off.
        static bool wasTouching = false;

        if (touching && !wasTouching) {
            touchCount++;

            // Logged because a touch that hits nothing is indistinguishable
            // from a touch that was not registered at all, and those need
            // opposite fixes.
            Serial.printf("touch x=%d y=%d\n", static_cast<int>(x), static_cast<int>(y));

            if (handleTouch(static_cast<int>(x), static_cast<int>(y))) {
                // A consumed touch usually changes something the next frame
                // will show, so repaint promptly rather than waiting.
                drawMachineScreen();
            }
        }

        wasTouching = touching;
    }

    // ======================= UI =====================================
    //
    // Two pages: a shot readout and a settings page, switched by the gear in
    // the header. 480x320 landscape.
    //
    // Everything is drawn in two parts. The CHROME - headings, rules, button
    // outlines - is painted once when a page is entered, because it never
    // changes and repainting it every frame is what makes a display flicker.
    // Only the VALUES are repainted, and only when they differ from what is
    // already on screen. That is why each one keeps a "last drawn" copy.

    namespace {

        constexpr int kW = 480;
        constexpr int kH = 320;

        constexpr int kHeaderH = 30;
        constexpr int kFooterH = 26;

        // Palette. Dark, because this sits on a machine in a kitchen and a
        // white screen at 6am is unkind.
        constexpr uint16_t kBg = 0x0000;         // black
        constexpr uint16_t kPanel = 0x18E3;      // very dark grey
        constexpr uint16_t kRule = 0x39E7;       // divider grey
        constexpr uint16_t kLabel = 0x8410;      // dim grey for captions
        constexpr uint16_t kValue = 0xFFFF;      // white
        constexpr uint16_t kAccent = 0x07FF;     // cyan, the boiler
        constexpr uint16_t kReady = 0x07E0;      // green
        constexpr uint16_t kHeat = 0xFD20;       // amber
        constexpr uint16_t kAlarm = 0xF800;      // red
        constexpr uint16_t kDim = 0x4208;        // disabled

        Page currentPage = Page::Main;
        bool chromeDrawn = false;

        // Last-drawn values, so a field is only repainted when it changes.
        char lastBoiler[12] = "";
        char lastGroup[12] = "";
        char lastSetpoint[12] = "";
        char lastWeight[14] = "";
        char lastFlow[14] = "";
        char lastPump[10] = "";
        char lastStatus[28] = "";
        int lastHeatBar = -1;
        int lastLink = -1;
        int lastHeaterOn = -1;

        // WiFi sub-page. Up here with the rest rather than next to the code
        // that draws them, so forgetValues() below can reach them.
        /** Two-tap confirm for FORGET. Times out so it cannot stay armed. */
        bool forgetArmed = false;
        int lastForgetArmed = -1;
        uint32_t forgetArmedAtMs = 0;
        constexpr uint32_t kForgetArmedMs = 5000;

        char lastNetState[20] = "";
        char lastNetName[40] = "";
        char lastNetAddr[24] = "";

        /** Reset every cached value so the next pass repaints the lot. */
        void forgetValues() {
            lastBoiler[0] = lastGroup[0] = lastSetpoint[0] = '\0';
            lastWeight[0] = lastFlow[0] = lastPump[0] = lastStatus[0] = '\0';
            lastHeatBar = -1;
            lastLink = -1;
            lastHeaterOn = -1;

            // The WiFi page's fields too. Missing these meant entering the
            // page drew the chrome and left the three values blank until one
            // of them happened to change.
            lastNetState[0] = lastNetName[0] = lastNetAddr[0] = '\0';
            lastForgetArmed = -1;
        }

        // Proper fonts rather than the built-in 5x7 bitmap scaled up, which is
        // what made the first version look like a debug screen. Font7 is a
        // seven-segment face and is digits-only, which suits a temperature and
        // suits nothing else.
        const lgfx::IFont* const kFontBig = &fonts::Font7;            // boiler
        const lgfx::IFont* const kFontValue = &fonts::FreeSansBold18pt7b;
        const lgfx::IFont* const kFontMid = &fonts::FreeSansBold12pt7b;
        const lgfx::IFont* const kFontCaption = &fonts::FreeSans9pt7b;
        const lgfx::IFont* const kFontButton = &fonts::FreeSansBold9pt7b;

        void drawCaption(const int x, const int y, const char* text) {
            lcd.setFont(kFontCaption);
            lcd.setTextSize(1);
            lcd.setTextColor(kLabel, kBg);
            lcd.setCursor(x, y);
            lcd.print(text);
        }

        /**
         * @brief Draw a value in a fixed-width cell, only if it changed.
         *
         * The cell is cleared first rather than relying on the text background,
         * because a shorter value would otherwise leave the tail of the longer
         * one behind.
         */
        /** Draw into a cleared cell. The caller has already decided it changed. */
        void drawFieldForced(const int x, const int y, const int w, const lgfx::IFont* font,
                             const uint16_t colour, const char* text) {
            lcd.setFont(font);
            lcd.setTextSize(1);
            lcd.fillRect(x, y, w, lcd.fontHeight(), kBg);
            lcd.setTextColor(colour, kBg);
            lcd.setCursor(x, y);
            lcd.print(text);
        }

        void drawField(const int x, const int y, const int w, const lgfx::IFont* font,
                       const uint16_t colour, const char* text, char* last, const size_t lastLen) {
            if (strncmp(text, last, lastLen) == 0) {
                return;
            }

            snprintf(last, lastLen, "%s", text);

            lcd.setFont(font);
            lcd.setTextSize(1);

            // Clear with the FONT's height rather than a guess. Proportional
            // fonts also mean a shorter value does not cover a longer one, so
            // the cell is wiped rather than overwritten.
            lcd.fillRect(x, y, w, lcd.fontHeight(), kBg);

            lcd.setTextColor(colour, kBg);
            lcd.setCursor(x, y);
            lcd.print(text);
        }

        /** @brief A rounded button with a label, used on the setup page. */
        void drawButton(const int x, const int y, const int w, const int h,
                        const char* label, const uint16_t edge, const uint16_t textCol) {
            lcd.fillRoundRect(x, y, w, h, 6, kPanel);
            lcd.drawRoundRect(x, y, w, h, 6, edge);

            // Smaller than the value font. Button labels are read once to
            // learn the layout and then hit by position, so they do not need
            // to compete with the numbers.
            lcd.setFont(kFontButton);
            lcd.setTextSize(1);
            lcd.setTextColor(textCol, kPanel);

            // Centre properly: proportional fonts make a character count a bad
            // estimate of width.
            const int tw = lcd.textWidth(label);
            lcd.setCursor(x + (w - tw) / 2, y + (h - lcd.fontHeight()) / 2);
            lcd.print(label);
        }

        struct Hit {
                int x, y, w, h;
        };

        // LAYOUT, 480x320 landscape, header occupies 0..30.
        //
        // Setup is a MENU of sub-pages. Everything does NOT fit on one page -
        // trying to make it fit is what produced a cramped, overlapping mess -
        // so each group of controls gets its own page with space around it.
        //
        // Every page keeps BACK in the same place, so it is muscle memory
        // rather than a hunt.
        constexpr Hit kBtnBack = {324, 252, 136, 46};

        // --- Setup menu: big tiles, two per row -------------------------
        constexpr Hit kTileBrew = {20, 54, 210, 84};
        constexpr Hit kTileDisplay = {250, 54, 210, 84};
        constexpr Hit kTileWifi = {20, 152, 210, 84};

        // --- Setup > Brew -----------------------------------------------
        constexpr Hit kBtnSetMinus = {20, 70, 70, 60};
        constexpr Hit kBtnSetPlus = {270, 70, 70, 60};
        constexpr Hit kBtnHeater = {360, 70, 100, 60};
        constexpr Hit kBtnTare = {20, 160, 136, 56};
        constexpr Hit kBtnFlowZero = {172, 160, 136, 56};
        constexpr Hit kBtnClearFault = {324, 160, 136, 56};

        // --- Setup > Display ---------------------------------------------
        constexpr Hit kBtnDimmer = {20, 80, 80, 64};
        constexpr Hit kBtnBrighter = {116, 80, 80, 64};
        constexpr Hit kBtnVolDown = {20, 176, 80, 64};
        constexpr Hit kBtnVolUp = {116, 176, 80, 64};

        // --- Setup > WiFi --------------------------------------------------
        // WiFi page, worked out rather than estimated. kFontMid is ~24 px and
        // drawField/drawFieldForced treat y as the TOP, clearing downward:
        //
        //   header                 0..30
        //   STATUS caption         46      value 62..86
        //   NETWORK caption        100     value 116..140
        //   ADDRESS caption        154     value 170..194
        //   SET UP / FORGET        206..250
        //   BACK (shared)          252..298
        //
        // The previous version put these buttons at 186, straight through the
        // ADDRESS value.
        constexpr Hit kBtnWifiSetup = {20, 206, 200, 44};
        constexpr Hit kBtnWifiForget = {240, 206, 200, 44};

        // Generous: the whole right end of the header, and a little below it.
        // A small box in the extreme corner is exactly where a capacitive
        // panel is least accurate, and there is nothing else up there to hit.
        constexpr Hit kGear = {kW - 110, 0, 110, kHeaderH + 10};

        bool inside(const Hit& h, const int x, const int y) {
            return x >= h.x && x < (h.x + h.w) && y >= h.y && y < (h.y + h.h);
        }

        uint8_t brightness = 200;
        bool invertOn = true; // this panel needs inversion; see the LGFX config

        void drawHeader(const char* title) {
            lcd.fillRect(0, 0, kW, kHeaderH, kPanel);
            lcd.drawFastHLine(0, kHeaderH, kW, kRule);

            lcd.setFont(kFontMid);
            lcd.setTextSize(1);
            lcd.setTextColor(kValue, kPanel);
            lcd.setCursor(10, 5);
            lcd.print(title);

            // Gear, drawn as a simple cog so it reads at a glance.
            //
            // Its position is FIXED in the corner and deliberately not derived
            // from the hit box. The touch target is much larger than the icon,
            // because the extreme corner is where a capacitive panel is least
            // accurate - but enlarging the box should not drag the icon out of
            // the corner with it, which is exactly what happened when the two
            // shared a coordinate.
            const int cx = kW - 21;
            const int cy = kHeaderH / 2;
            lcd.drawCircle(cx, cy, 9, kLabel);
            lcd.drawCircle(cx, cy, 4, kLabel);

            for (int a = 0; a < 360; a += 60) {
                const float r = a * 3.14159f / 180.0f;
                lcd.drawLine(cx + static_cast<int>(9 * cosf(r)), cy + static_cast<int>(9 * sinf(r)),
                             cx + static_cast<int>(13 * cosf(r)), cy + static_cast<int>(13 * sinf(r)), kLabel);
            }
        }

        void drawMainChrome() {
            lcd.fillScreen(kBg);
            drawHeader("ARGENTA SME");

            // Captions sit roughly 20 px above their value. The caption font
            // is about 14 px tall, so anything less than that and the two
            // collide - which is exactly what 38/52 did.
            drawCaption(20, 40, "BOILER");
            drawCaption(300, 40, "TARGET");
            drawCaption(300, 96, "GROUP");

            // Heat bar below the right-hand column rather than underneath it.
            // An earlier version put the bar at y=132 with the group value at
            // y=118, so the value was drawn on top of the bar.
            lcd.drawRect(20, 150, 440, 16, kRule);
            drawCaption(20, 172, "HEATER");

            lcd.drawFastHLine(0, 192, kW, kRule);

            drawCaption(24, 200, "WEIGHT");
            drawCaption(184, 200, "FLOW");
            drawCaption(344, 200, "PUMP");

            lcd.drawFastVLine(160, 196, 70, kRule);
            lcd.drawFastVLine(320, 196, 70, kRule);

            lcd.drawFastHLine(0, kH - kFooterH, kW, kRule);
        }

        const char* statusText(const machinelink::State& st, const bool live, uint16_t* colour) {
            if (!live) {
                *colour = kHeat;

                return "NO LINK TO CONTROLLER";
            }

            if (strcmp(st.fault, "none") != 0) {
                *colour = kAlarm;

                return st.fault;
            }

            if (!st.boilerOk) {
                *colour = kAlarm;

                return "BOILER SENSOR FAULT";
            }

            if (!st.heaterEnabled) {
                *colour = kDim;

                return "HEATER OFF";
            }

            if (st.setpointC > 0.0f && st.boilerC >= (st.setpointC - 1.0f)) {
                *colour = kReady;

                return "READY";
            }

            *colour = kHeat;

            return "HEATING";
        }

        /** @brief A large menu tile with a label. */
        void drawTile(const Hit& h, const char* label, const uint16_t edge) {
            lcd.fillRoundRect(h.x, h.y, h.w, h.h, 8, kPanel);
            lcd.drawRoundRect(h.x, h.y, h.w, h.h, 8, edge);

            lcd.setFont(kFontMid);
            lcd.setTextSize(1);
            lcd.setTextColor(edge, kPanel);

            const int tw = lcd.textWidth(label);
            lcd.setCursor(h.x + (h.w - tw) / 2, h.y + (h.h - lcd.fontHeight()) / 2);
            lcd.print(label);
        }

        /**
         * The setup MENU.
         *
         * Deliberately almost empty. Everything used to be crammed onto this
         * one page and it looked it; the controls now live behind these tiles
         * with room around them. Adding a group means adding a tile and a
         * page, not squeezing another row into space that does not exist.
         */
        void drawSetupChrome() {
            lcd.fillScreen(kBg);
            drawHeader("SETUP");

            drawTile(kTileBrew, "BREW", kAccent);
            drawTile(kTileDisplay, "DISPLAY", kValue);
            drawTile(kTileWifi, "WIFI", kAccent);

            drawButton(kBtnBack.x, kBtnBack.y, kBtnBack.w, kBtnBack.h, "BACK", kReady, kReady);
        }

        void drawSetupValues() {
            // Nothing live on the menu itself.
        }

        void drawBrewChrome() {
            lcd.fillScreen(kBg);
            drawHeader("BREW");

            drawCaption(20, 52, "BREW TEMPERATURE");
            drawButton(kBtnSetMinus.x, kBtnSetMinus.y, kBtnSetMinus.w, kBtnSetMinus.h, "-", kAccent, kAccent);
            drawButton(kBtnSetPlus.x, kBtnSetPlus.y, kBtnSetPlus.w, kBtnSetPlus.h, "+", kAccent, kAccent);

            drawButton(kBtnTare.x, kBtnTare.y, kBtnTare.w, kBtnTare.h, "TARE", kRule, kValue);
            drawButton(kBtnFlowZero.x, kBtnFlowZero.y, kBtnFlowZero.w, kBtnFlowZero.h, "ZERO VOL", kRule, kValue);
            drawButton(kBtnClearFault.x, kBtnClearFault.y, kBtnClearFault.w, kBtnClearFault.h, "CLEAR", kAlarm, kAlarm);

            drawButton(kBtnBack.x, kBtnBack.y, kBtnBack.w, kBtnBack.h, "BACK", kReady, kReady);
        }

        void drawBrewValues() {
            const auto& st = machinelink::state();
            char buf[24];

            snprintf(buf, sizeof(buf), "%.1f C", st.setpointC);
            drawField(104, 82, 160, kFontValue, kAccent, buf, lastSetpoint, sizeof(lastSetpoint));

            const int heaterOn = st.heaterEnabled ? 1 : 0;

            if (heaterOn != lastHeaterOn) {
                lastHeaterOn = heaterOn;
                drawButton(kBtnHeater.x, kBtnHeater.y, kBtnHeater.w, kBtnHeater.h,
                           heaterOn ? "HEAT ON" : "HEAT OFF", heaterOn ? kHeat : kRule,
                           heaterOn ? kHeat : kValue);
            }

            // What the boiler is actually doing, so the effect of a setpoint
            // change is visible without going back to the main page. Below
            // the buttons (160..216), clear of BACK (252..298).
            uint16_t col = kDim;
            const char* text = statusText(st, machinelink::isConnected(), &col);

            snprintf(buf, sizeof(buf), "%.1f C  %s", st.boilerC, text);
            drawField(20, 228, 290, kFontMid, col, buf, lastStatus, sizeof(lastStatus));
        }

        void drawDisplayChrome() {
            lcd.fillScreen(kBg);
            drawHeader("DISPLAY");

            drawCaption(20, 62, "BRIGHTNESS");
            drawButton(kBtnDimmer.x, kBtnDimmer.y, kBtnDimmer.w, kBtnDimmer.h, "-", kRule, kValue);
            drawButton(kBtnBrighter.x, kBtnBrighter.y, kBtnBrighter.w, kBtnBrighter.h, "+", kRule, kValue);

            drawCaption(20, 158, "VOLUME");
            drawButton(kBtnVolDown.x, kBtnVolDown.y, kBtnVolDown.w, kBtnVolDown.h, "-", kRule, kValue);
            drawButton(kBtnVolUp.x, kBtnVolUp.y, kBtnVolUp.w, kBtnVolUp.h, "+", kRule, kValue);

            drawButton(kBtnBack.x, kBtnBack.y, kBtnBack.w, kBtnBack.h, "BACK", kReady, kReady);
        }

        void drawDisplayValues() {
            char buf[16];

            snprintf(buf, sizeof(buf), "%d", brightness);
            drawField(216, 96, 120, kFontValue, kValue, buf, lastPump, sizeof(lastPump));

            snprintf(buf, sizeof(buf), "%d%%", audio::volumePercent());
            drawField(216, 192, 120, kFontValue, kValue, buf, lastFlow, sizeof(lastFlow));
        }

        /**
         * The WiFi sub-page.
         *
         * Reports state and offers the two actions that make sense here. No
         * on-screen keyboard: see the note in handleTouch.
         */
        void drawWifiChrome() {
            lcd.fillScreen(kBg);
            drawHeader("WIFI");

            drawCaption(20, 46, "STATUS");
            drawCaption(20, 100, "NETWORK");
            drawCaption(20, 154, "ADDRESS");

            drawButton(kBtnWifiSetup.x, kBtnWifiSetup.y, kBtnWifiSetup.w, kBtnWifiSetup.h,
                       "SET UP WIFI", kAccent, kAccent);

            drawButton(kBtnBack.x, kBtnBack.y, kBtnBack.w, kBtnBack.h, "BACK", kReady, kReady);
        }

        void drawWifiValues() {
            // Let an armed forget lapse rather than sit waiting indefinitely.
            if (forgetArmed && (millis() - forgetArmedAtMs) > kForgetArmedMs) {
                forgetArmed = false;
                lastForgetArmed = -1;
            }

            if (lastForgetArmed != (forgetArmed ? 1 : 0)) {
                lastForgetArmed = forgetArmed ? 1 : 0;

                drawButton(kBtnWifiForget.x, kBtnWifiForget.y, kBtnWifiForget.w, kBtnWifiForget.h,
                           forgetArmed ? "TAP AGAIN" : "FORGET", kAlarm, kAlarm);
            }

            if (strcmp(lastNetState, net::stateName()) != 0) {
                snprintf(lastNetState, sizeof(lastNetState), "%s", net::stateName());

                const uint16_t col = (net::state() == net::State::Online)   ? kReady
                                     : (net::state() == net::State::Portal) ? kHeat
                                                                            : kLabel;

                drawFieldForced(20, 62, 440, kFontMid, col, lastNetState);
            }

            if (strcmp(lastNetName, net::networkName()) != 0) {
                snprintf(lastNetName, sizeof(lastNetName), "%s", net::networkName());
                drawFieldForced(20, 116, 440, kFontMid, kValue, lastNetName);
            }

            if (strcmp(lastNetAddr, net::addressText()) != 0) {
                snprintf(lastNetAddr, sizeof(lastNetAddr), "%s", net::addressText());
                drawFieldForced(20, 170, 440, kFontMid, kAccent, lastNetAddr);
            }
        }

        void drawMainValues() {
            const auto& st = machinelink::state();
            const bool live = machinelink::isConnected();

            char buf[24];

            // Boiler, the number you actually watch.
            if (live && st.boilerOk) {
                snprintf(buf, sizeof(buf), "%.1f", st.boilerC);
            }
            else {
                snprintf(buf, sizeof(buf), "--.-");
            }

            drawField(20, 60, 230, kFontBig, (live && st.boilerOk) ? kAccent : kAlarm, buf, lastBoiler, sizeof(lastBoiler));

            // Unit immediately after the digits. It was at a fixed x=236,
            // which left it marooned a hundred pixels clear of a four-glyph
            // reading.
            lcd.setFont(kFontValue);
            lcd.setTextSize(1);
            lcd.setTextColor(kLabel, kBg);
            lcd.setCursor(152, 82);
            lcd.print("C");

            snprintf(buf, sizeof(buf), "%.1f", live ? st.setpointC : 0.0f);
            drawField(300, 60, 170, kFontValue, kValue, buf, lastSetpoint, sizeof(lastSetpoint));

            if (live && st.groupOk) {
                snprintf(buf, sizeof(buf), "%.1f", st.groupC);
            }
            else {
                snprintf(buf, sizeof(buf), "--.-");
            }

            drawField(300, 116, 170, kFontValue, (live && st.groupOk) ? kValue : kDim, buf, lastGroup, sizeof(lastGroup));

            // Heater bar. Width is the duty, colour says whether it is allowed
            // to be doing anything.
            const int duty = live ? static_cast<int>(st.heaterDuty) : 0;
            const int barW = (duty * 436) / 100;

            if (barW != lastHeatBar) {
                lastHeatBar = barW;

                lcd.fillRect(22, 152, 436, 12, kBg);

                if (barW > 0) {
                    lcd.fillRect(22, 152, barW, 12, st.heaterEnabled ? kHeat : kDim);
                }

                lcd.fillRect(100, 170, 90, 18, kBg);
                lcd.setFont(kFontCaption);
                lcd.setTextSize(1);
                lcd.setTextColor(st.heaterEnabled ? kHeat : kDim, kBg);
                lcd.setCursor(100, 170);
                lcd.printf("%d%%", duty);
            }

            // Shot row.
            if (live && st.weightOk) {
                snprintf(buf, sizeof(buf), st.weightCalibrated ? "%.1f" : "%.0f", st.weightG);
            }
            else {
                snprintf(buf, sizeof(buf), "--");
            }

            drawField(24, 220, 134, kFontValue, (live && st.weightOk) ? (st.weightCalibrated ? kValue : kHeat) : kDim,
                      buf, lastWeight, sizeof(lastWeight));

            lcd.setFont(kFontCaption);
            lcd.setTextSize(1);
            lcd.setTextColor(kLabel, kBg);
            lcd.fillRect(24, 252, 130, 16, kBg);
            lcd.setCursor(24, 252);
            lcd.print(st.weightCalibrated ? "grams" : "raw counts");

            snprintf(buf, sizeof(buf), "%.1f", live ? st.flowMlPerSec : 0.0f);
            drawField(184, 220, 134, kFontValue, live ? kValue : kDim, buf, lastFlow, sizeof(lastFlow));
            lcd.setFont(kFontCaption);
            lcd.setTextSize(1);
            lcd.setTextColor(kLabel, kBg);
            lcd.fillRect(184, 252, 130, 16, kBg);
            lcd.setCursor(184, 252);
            lcd.printf("ml/s   %.0f ml", live ? st.volumeMl : 0.0f);

            snprintf(buf, sizeof(buf), "%.0f%%", live ? st.pumpPercent : 0.0f);
            drawField(344, 220, 130, kFontValue, (live && st.pumpPercent > 0.0f) ? kReady : kDim, buf, lastPump, sizeof(lastPump));

            // Footer status strip.
            uint16_t col = kDim;
            const char* text = statusText(st, live, &col);

            if (strncmp(text, lastStatus, sizeof(lastStatus)) != 0) {
                snprintf(lastStatus, sizeof(lastStatus), "%s", text);

                lcd.fillRect(0, kH - kFooterH + 1, kW, kFooterH - 1, kBg);
                lcd.setFont(kFontMid);
                lcd.setTextSize(1);
                lcd.setTextColor(col, kBg);
                lcd.setCursor(12, kH - kFooterH + 4);
                lcd.print(text);
            }

            // Link pip in the header, so a stale screen is obvious.
            const int linkState = live ? 1 : 0;

            if (linkState != lastLink) {
                lastLink = linkState;
                lcd.fillCircle(kW - 60, kHeaderH / 2, 5, live ? kReady : kAlarm);
            }
        }


    } // namespace

    uint8_t rotation() {
        return currentRotation;
    }

    void setRotation(const uint8_t r) {
        currentRotation = r & 3;
        lcd.setRotation(currentRotation);

        // Geometry changed under everything, so nothing on screen is valid.
        invalidate();
        drawMachineScreen();
    }

    void toggleInvert() {
        invertOn = !invertOn;
        lcd.invertDisplay(invertOn);
    }

    void setBrightness(const uint8_t level) {
        brightness = level;
        lcd.setBrightness(level);
    }

    Page page() {
        return currentPage;
    }

    void invalidate() {
        chromeDrawn = false;
        forgetValues();
    }

    void setPage(const Page p) {
        currentPage = p;
        invalidate();
    }

    Page parentOf(const Page p) {
        switch (p) {
            case Page::SetupWifi:
            case Page::SetupBrew:
            case Page::SetupDisplay:
                return Page::Setup;

            case Page::Setup:
                return Page::Main;

            case Page::Main:
                // Its own parent, so Back at the top does nothing rather than
                // navigating off the end of the tree.
                return Page::Main;
        }

        return Page::Main;
    }

    bool handleTouch(const int x, const int y) {
        if (currentPage == Page::Main) {
            if (inside(kGear, x, y)) {
                setPage(Page::Setup);

                return true;
            }

            return false;
        }

        // From here down we are somewhere in the setup tree. Every action is
        // a REQUEST sent to the Teensy, which is free to refuse it - the
        // screen never assumes it worked, it waits for the next telemetry
        // frame to say so.

        // BACK follows the declared parent, so this one line serves every
        // page including ones not written yet. The gear always jumps straight
        // out to Main, because it is the way in and should be the way out.
        if (inside(kBtnBack, x, y)) {
            forgetArmed = false;
            setPage(parentOf(currentPage));

            return true;
        }

        if (inside(kGear, x, y)) {
            forgetArmed = false;
            setPage(Page::Main);

            return true;
        }

        // --- The setup MENU: tiles into sub-pages ------------------------
        if (currentPage == Page::Setup) {
            if (inside(kTileBrew, x, y)) {
                setPage(Page::SetupBrew);

                return true;
            }

            if (inside(kTileDisplay, x, y)) {
                setPage(Page::SetupDisplay);

                return true;
            }

            if (inside(kTileWifi, x, y)) {
                setPage(Page::SetupWifi);

                return true;
            }

            return false;
        }

        // --- Setup > Brew -------------------------------------------------
        if (currentPage == Page::SetupBrew) {
            const auto& st = machinelink::state();
            char cmd[48];

            if (inside(kBtnSetMinus, x, y)) {
                snprintf(cmd, sizeof(cmd), "setpoint=%.1f", st.setpointC - 0.5f);
                machinelink::sendCommand(cmd);

                return true;
            }

            if (inside(kBtnSetPlus, x, y)) {
                snprintf(cmd, sizeof(cmd), "setpoint=%.1f", st.setpointC + 0.5f);
                machinelink::sendCommand(cmd);

                return true;
            }

            if (inside(kBtnHeater, x, y)) {
                snprintf(cmd, sizeof(cmd), "heater=%d", st.heaterEnabled ? 0 : 1);
                machinelink::sendCommand(cmd);

                return true;
            }

            if (inside(kBtnTare, x, y)) {
                machinelink::sendCommand("tare=1");

                return true;
            }

            if (inside(kBtnFlowZero, x, y)) {
                machinelink::sendCommand("flowreset=1");

                return true;
            }

            if (inside(kBtnClearFault, x, y)) {
                machinelink::sendCommand("clearfault=1");

                return true;
            }

            return false;
        }

        // --- Setup > Display ----------------------------------------------
        if (currentPage == Page::SetupDisplay) {
            if (inside(kBtnDimmer, x, y)) {
                brightness = (brightness > 40) ? (brightness - 40) : 10;
                lcd.setBrightness(brightness);
                lastPump[0] = '\0';

                return true;
            }

            if (inside(kBtnBrighter, x, y)) {
                brightness = (brightness < 215) ? (brightness + 40) : 255;
                lcd.setBrightness(brightness);
                lastPump[0] = '\0';

                return true;
            }

            if (inside(kBtnVolDown, x, y)) {
                const int v = audio::volumePercent();
                audio::setVolumePercent(static_cast<uint8_t>(v > 10 ? v - 10 : 0));
                lastFlow[0] = '\0';

                return true;
            }

            if (inside(kBtnVolUp, x, y)) {
                const int v = audio::volumePercent();
                audio::setVolumePercent(static_cast<uint8_t>(v < 90 ? v + 10 : 100));
                lastFlow[0] = '\0';

                return true;
            }

            return false;
        }

        // --- Setup > WiFi ---------------------------------------------------
        //
        // NO on-screen keyboard here, and that is a decision rather than an
        // omission. Typing a WiFi password on a 4 inch panel is miserable, and
        // WiFiManager already serves a credential page far better suited to
        // the phone already in the owner's hand. So this page reports state
        // and starts the portal; the typing happens there.
        //
        // It also keeps credentials out of this firmware entirely: they go
        // from the person straight into the ESP32's flash, never through a
        // config file or a repository.
        if (currentPage == Page::SetupWifi) {
            if (inside(kBtnWifiSetup, x, y)) {
                // NON-DESTRUCTIVE. Asks for the portal on the next boot and
                // leaves the stored credentials alone, so cancelling puts the
                // machine back on the network it was already using.
                //
                // This used to call forgetNetworkAndRestart(), so tapping it
                // to find out what it did wiped the network. It did exactly
                // that the first time the button worked.
                display::showMessage("WiFi setup", "restarting", "into setup mode");
                net::requestPortalOnNextBoot();

                return true;
            }

            if (inside(kBtnWifiForget, x, y)) {
                // DESTRUCTIVE, so it takes two taps: the first arms it and
                // relabels the button, the second does it.
                if (!forgetArmed) {
                    forgetArmed = true;
                    forgetArmedAtMs = millis();

                    return true;
                }

                forgetArmed = false;
                display::showMessage("WiFi", "forgetting", "network...");
                net::forgetNetworkAndRestart();

                return true;
            }

            // A tap anywhere else on this page cancels a pending forget.
            forgetArmed = false;

            return false;
        }

        return false;
    }

    void drawMachineScreen() {
        if (!chromeDrawn) {
            chromeDrawn = true;

            switch (currentPage) {
                case Page::Main: drawMainChrome(); break;
                case Page::Setup: drawSetupChrome(); break;
                case Page::SetupBrew: drawBrewChrome(); break;
                case Page::SetupDisplay: drawDisplayChrome(); break;
                case Page::SetupWifi: drawWifiChrome(); break;
            }
        }

        switch (currentPage) {
            case Page::Main: drawMainValues(); break;
            case Page::Setup: drawSetupValues(); break;
            case Page::SetupBrew: drawBrewValues(); break;
            case Page::SetupDisplay: drawDisplayValues(); break;
            case Page::SetupWifi: drawWifiValues(); break;
        }
    }

} // namespace display
