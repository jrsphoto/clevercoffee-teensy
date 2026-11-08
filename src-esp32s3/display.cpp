/**
 * @file display.cpp
 *
 * @brief LovyanGFX bring-up for the 4.0" ESP32-S3 board
 */

#include "audio.h"
#include "display.h"

#include "iconCup.h"
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

        Page currentPage = Page::Home;
        bool chromeDrawn = false;

        /**
         * Is the MANUAL tile's pump run going?
         *
         * Tracked here only so the tile knows whether the next tap means
         * start or stop. It is NOT a belief about the machine: the Teensy
         * stops the run on its own cap regardless, and the readout's status
         * strip is what actually says whether the pump is on.
         */
        bool manualRunning = false;

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

        /** Is the STOP button currently painted? -1 = not yet decided. */
        int lastStopShown = -1;

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
            lastStopShown = -1;
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

        // Home-grid tiles. FreeSansBold9pt is about 18 px tall and wide with
        // it: RISTRETTO and BACKFLUSH both ran past their tile borders in it,
        // and stepping down to the REGULAR weight of the same size was not
        // enough - those two differ in weight, not in size, so the saving was
        // a few pixels when tens were needed. DejaVu12 is genuinely smaller
        // at 12 px and leaves real margin. DejaVu9 for the sub-label keeps
        // the hierarchy visible.
        const lgfx::IFont* const kFontTile = &fonts::DejaVu12;
        const lgfx::IFont* const kFontTileSub = &fonts::DejaVu9;

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

        // --- Main readout: STOP ---------------------------------------------
        //
        // In the gap to the right of the TARGET and GROUP readings. Checked
        // against that page's own geometry, which is the mistake this project
        // has made four times before:
        //
        //   boiler value      60..~100   clears x=20..250
        //   gear hit box      0..40      (header, well clear above)
        //   TARGET value      60..84     270..370, moved left from 300
        //   GROUP value       116..140   same
        //   heat bar          150..166   so nothing may extend past y=148
        //
        // The temperature column moved 30 px left - there is 20 px of clear
        // air between it and the boiler reading - which makes room for a
        // 94x92 button rather than 68x92. Worth doing: this is the one
        // control that has to be hittable without looking at it.
        constexpr Hit kBtnStop = {380, 56, 94, 92};

        // --- HOME: the 3x3 grid -------------------------------------------
        //
        // Adapted from the DK Espresso machine John brought as a reference.
        // See TEENSY-PORT-BRIEF.md 9.9, including why hot water and steam are
        // NOT here: they are manual valves on this machine and the board has
        // no output for either, so those tiles could only ever do nothing.
        //
        // GEOMETRY, worked out rather than estimated - the trap this project
        // has already fallen into four times is computing against the wrong
        // page's numbers.
        //
        // NO HEADER ON THIS PAGE. It carried the machine name and the gear,
        // and neither earns 30 px here: the name is written on the machine
        // you are standing at, and SETTINGS is already a tile. Dropping it
        // gives every row 18 px more, which is the difference between a
        // cramped tile and a comfortable one.
        //
        //   grid rows         4..96, 102..194, 200..292
        //   footer rule       at kH - kFooterH = 294
        //   footer strip      294..320   (boiler temp and state)
        //
        // 3 columns of 152 with 8 px gutters inside a 480 wide screen:
        //   8 + 152 + 8 + 152 + 8 + 152 = 480. Rows are 92 tall with 6 between.
        constexpr int kGridX0 = 8;
        constexpr int kGridY0 = 4;
        constexpr int kTileW = 152;
        constexpr int kTileH = 92;
        constexpr int kTileGapX = 8;
        constexpr int kTileGapY = 6;

        constexpr Hit homeTile(const int col, const int row) {
            return {kGridX0 + col * (kTileW + kTileGapX),
                    kGridY0 + row * (kTileH + kTileGapY),
                    kTileW, kTileH};
        }

        /** What a home tile does. Order matches the grid, reading across. */
        enum class HomeAction : uint8_t {
            Ristretto, // profile 0, stops on weight
            Espresso,  // profile 1, stops on weight
            Lungo,     // profile 2, stops on volume
            Manual,    // pump until tapped again, capped
            Flush,     // purge the shower screen
            Backflush, // the multi-cycle routine
            Scale,     // tare
            Status,    // temps, link, faults, clear
            Settings,  // the existing setup menu
            Count,
        };

        /** Icon style for a tile. Drinks use the artwork; the rest are drawn. */
        enum class TileIcon : uint8_t {
            Cup,
            Hand,   // manual
            Drop,   // flush
            Cycle,  // backflush
            Scales, // tare
            Info,   // status
            Gear,   // settings
        };

        struct HomeTile {
                const char* label;
                const char* sub; // what it stops on, or what it does
                TileIcon icon;
        };

        // Labels say what a shot STOPS ON, never a pressure figure: there is
        // no brew pressure sensor installed and no profiling loop, so "9 bar"
        // would be describing something that does not exist. When the sensor
        // and the loops land (brief 9.8) these can say more.
        constexpr HomeTile kHomeTiles[static_cast<int>(HomeAction::Count)] = {
            {"RISTRETTO", "by weight", TileIcon::Cup},
            {"ESPRESSO", "by weight", TileIcon::Cup},
            {"LUNGO", "by volume", TileIcon::Cup},
            {"MANUAL", "tap to stop", TileIcon::Hand},
            {"FLUSH", "purge", TileIcon::Drop},
            {"BACKFLUSH", "clean cycle", TileIcon::Cycle},
            {"SCALE", "tare", TileIcon::Scales},
            {"STATUS", "live readout", TileIcon::Info},
            {"SETTINGS", "", TileIcon::Gear},
        };

        Hit homeHit(const HomeAction a) {
            const int i = static_cast<int>(a);

            return homeTile(i % 3, i / 3);
        }


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
            drawCaption(270, 40, "TARGET");
            drawCaption(270, 96, "GROUP");

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
         * @brief Draw one tile icon, centred on (cx, cy).
         *
         * The drink glass is the artwork from assets/coffee-drink.png, reduced
         * to 38x38 (iconCup.h). The rest are drawn with primitives rather than
         * bitmaps: they are simple shapes, they cost no flash, and they stay
         * crisp if the tile size ever changes. The reference machine mixes a
         * pictorial cup with line symbols the same way.
         */
        void drawTileIcon(const TileIcon icon, const int cx, const int cy, const uint16_t col) {
            switch (icon) {
                case TileIcon::Cup: {
                    // CAST TO rgb565_t, and do not remove it. LovyanGFX's
                    // pushImage overload for a bare uint16_t* treats the data
                    // as swap565_t - big-endian, the old TFT_eSPI convention -
                    // so the bytes of every pixel come out the wrong way
                    // round. The first attempt did exactly that and the glass
                    // rendered as blue and purple noise with its shape just
                    // visible through it. Solid fills were unaffected because
                    // they take a colour value, not an image buffer.
                    //
                    // No transparency argument: the artwork is already
                    // composited onto the tile colour. See iconCup.h for why
                    // the colour-key version was abandoned.
                    const auto* img = reinterpret_cast<const lgfx::rgb565_t*>(icons::kCup);

                    lcd.pushImage(cx - icons::kCupW / 2, cy - icons::kCupH / 2,
                                  icons::kCupW, icons::kCupH, img);
                    break;
                }

                case TileIcon::Hand:
                    // A paddle: lever on a pivot.
                    lcd.fillCircle(cx - 8, cy + 8, 4, col);
                    lcd.drawLine(cx - 8, cy + 8, cx + 10, cy - 10, col);
                    lcd.drawLine(cx - 7, cy + 9, cx + 11, cy - 9, col);
                    lcd.fillCircle(cx + 10, cy - 10, 3, col);
                    break;

                case TileIcon::Drop:
                    // A falling droplet: triangular top, round bottom.
                    lcd.fillCircle(cx, cy + 4, 8, col);
                    lcd.fillTriangle(cx, cy - 12, cx - 8, cy + 5, cx + 8, cy + 5, col);
                    break;

                case TileIcon::Cycle:
                    // Two arcs chasing each other, with arrow heads.
                    lcd.drawArc(cx, cy, 11, 9, 200, 340, col);
                    lcd.drawArc(cx, cy, 11, 9, 20, 160, col);
                    lcd.fillTriangle(cx + 11, cy - 7, cx + 16, cy - 3, cx + 7, cy - 2, col);
                    lcd.fillTriangle(cx - 11, cy + 7, cx - 16, cy + 3, cx - 7, cy + 2, col);
                    break;

                case TileIcon::Scales:
                    // A balance: platform on a column on a base.
                    lcd.drawFastHLine(cx - 12, cy - 8, 24, col);
                    lcd.drawFastHLine(cx - 12, cy - 7, 24, col);
                    lcd.drawFastVLine(cx, cy - 7, 14, col);
                    lcd.drawFastHLine(cx - 9, cy + 8, 18, col);
                    lcd.drawFastHLine(cx - 9, cy + 9, 18, col);
                    break;

                case TileIcon::Info:
                    lcd.drawCircle(cx, cy, 12, col);
                    lcd.drawCircle(cx, cy, 11, col);
                    lcd.fillRect(cx - 1, cy - 8, 3, 3, col);
                    lcd.fillRect(cx - 1, cy - 2, 3, 10, col);
                    break;

                case TileIcon::Gear:
                    lcd.drawCircle(cx, cy, 9, col);
                    lcd.drawCircle(cx, cy, 4, col);

                    for (int a = 0; a < 360; a += 60) {
                        const float r = a * 3.14159f / 180.0f;
                        lcd.drawLine(cx + static_cast<int>(9 * cosf(r)), cy + static_cast<int>(9 * sinf(r)),
                                     cx + static_cast<int>(13 * cosf(r)), cy + static_cast<int>(13 * sinf(r)), col);
                    }

                    break;
            }
        }

        /**
         * @brief One home-grid tile: icon left, label and sub-label right.
         *
         * 152x74 is not enough for an icon ABOVE a label at a readable size,
         * so they sit side by side. The icon column is 46 px, leaving ~100 for
         * text, which kFontButton fits comfortably.
         */
        void drawHomeTile(const HomeAction a, const uint16_t edge, const bool enabled) {
            const Hit h = homeHit(a);
            const HomeTile& t = kHomeTiles[static_cast<int>(a)];
            const uint16_t textCol = enabled ? kValue : kDim;
            const uint16_t iconCol = enabled ? edge : kDim;

            lcd.fillRoundRect(h.x, h.y, h.w, h.h, 8, kPanel);
            lcd.drawRoundRect(h.x, h.y, h.w, h.h, 8, enabled ? edge : kRule);

            drawTileIcon(t.icon, h.x + 26, h.y + h.h / 2, iconCol);

            // WRAPPING OFF, and this is the important one. A label wider than
            // the screen does not get truncated by default - LovyanGFX wraps
            // the overflow to x=0 on the next line. "BACKFLUSH" in the
            // right-hand column did exactly that: the tile showed "BACKFLUS"
            // and a stray "H" appeared floating in the left margin, which
            // looked like two unrelated faults and was one.
            lcd.setTextWrap(false);

            // The cup artwork is 38 wide and centred on h.x+26, so it reaches
            // h.x+45; the drawn icons are narrower and reach about h.x+42.
            // Long labels get those few pixels back rather than being shrunk
            // unnecessarily.
            const int textX = h.x + (t.icon == TileIcon::Cup ? 50 : 46);
            const int labelW = (h.x + h.w - 6) - textX;

            // Backstop only. DejaVu12 fits every label here with room to
            // spare; this catches a future one that does not, so it shrinks
            // rather than running over the border. Measured, not assumed -
            // with a proportional font a character count is a bad estimate.
            lcd.setFont(kFontTile);
            lcd.setTextSize(1);

            if (lcd.textWidth(t.label) > labelW) {
                lcd.setFont(kFontTileSub);
            }

            // Two lines centred as a block in a 92 px tile: label 12 px, gap
            // 6, sub 9 px, so the pair starts about 32 from the top. A single
            // line centres on its own.
            lcd.setTextColor(textCol, kPanel);
            lcd.setCursor(textX, h.y + (t.sub[0] != '\0' ? 32 : 40));
            lcd.print(t.label);

            if (t.sub[0] != '\0') {
                lcd.setFont(kFontTileSub);
                lcd.setTextColor(enabled ? kLabel : kDim, kPanel);
                lcd.setCursor(textX, h.y + 52);
                lcd.print(t.sub);
            }
        }

        /**
         * The HOME grid.
         *
         * Drawn as chrome because the tiles themselves do not change; what
         * changes is the footer strip, which drawHomeValues() repaints.
         */
        void drawHomeChrome() {
            lcd.fillScreen(kBg);

            for (int i = 0; i < static_cast<int>(HomeAction::Count); i++) {
                const auto a = static_cast<HomeAction>(i);
                const bool drink = i < 3;

                drawHomeTile(a, drink ? kAccent : kValue, true);
            }

            lcd.drawFastHLine(0, kH - kFooterH, kW, kRule);
        }

        /**
         * The home grid's footer strip.
         *
         * Boiler temperature, shot timer and machine state, on the one page
         * where none of the big readouts are visible. Same idea as the
         * reference machine's bottom line ("91.9C / 0.0s"): whatever page you
         * are on, the two numbers that matter are still there.
         *
         * Occupies kH - kFooterH + 1 .. kH. Nothing else may draw there.
         */
        void drawHomeValues() {
            const auto& st = machinelink::state();
            const bool live = machinelink::isConnected();

            char buf[40];
            uint16_t col = kDim;
            const char* text = statusText(st, live, &col);

            if (live) {
                snprintf(buf, sizeof(buf), "%.1fC  %s", st.boilerC, text);
            }
            else {
                snprintf(buf, sizeof(buf), "%s", text);
            }

            if (strncmp(buf, lastStatus, sizeof(lastStatus)) == 0) {
                return;
            }

            snprintf(lastStatus, sizeof(lastStatus), "%s", buf);

            lcd.fillRect(0, kH - kFooterH + 1, kW, kFooterH - 1, kBg);
            lcd.setFont(kFontMid);
            lcd.setTextSize(1);
            lcd.setTextColor(col, kBg);
            lcd.setCursor(12, kH - kFooterH + 4);
            lcd.print(buf);
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

            // 100 wide, not 170. These used to clear all the way to x=470,
            // which is why the space to their right only LOOKED empty - the
            // reading is short, but the clear behind it was not, and anything
            // drawn there was wiped on the next update. STOP lives there now.
            snprintf(buf, sizeof(buf), "%.1f", live ? st.setpointC : 0.0f);
            drawField(270, 60, 100, kFontValue, kValue, buf, lastSetpoint, sizeof(lastSetpoint));

            if (live && st.groupOk) {
                snprintf(buf, sizeof(buf), "%.1f", st.groupC);
            }
            else {
                snprintf(buf, sizeof(buf), "--.-");
            }

            drawField(270, 116, 100, kFontValue, (live && st.groupOk) ? kValue : kDim, buf, lastGroup, sizeof(lastGroup));

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

            // STOP, shown only while something is actually running.
            //
            // A stop control that is always there is a control you cannot
            // read: when it is live matters as much as where it is. Driven
            // off the controller's own state word, so it reflects the machine
            // rather than what this board last asked for.
            const bool running = live
                                 && strcmp(st.status, "idle") != 0
                                 && strcmp(st.status, "heating") != 0
                                 && strcmp(st.status, "fault") != 0;

            if (running != (lastStopShown == 1)) {
                lastStopShown = running ? 1 : 0;

                if (running) {
                    lcd.fillRoundRect(kBtnStop.x, kBtnStop.y, kBtnStop.w, kBtnStop.h, 8, kAlarm);
                    lcd.drawRoundRect(kBtnStop.x, kBtnStop.y, kBtnStop.w, kBtnStop.h, 8, kValue);

                    lcd.setFont(kFontButton);
                    lcd.setTextSize(1);
                    lcd.setTextColor(kValue, kAlarm);

                    const int tw = lcd.textWidth("STOP");
                    lcd.setCursor(kBtnStop.x + (kBtnStop.w - tw) / 2,
                                  kBtnStop.y + (kBtnStop.h - lcd.fontHeight()) / 2);
                    lcd.print("STOP");
                }
                else {
                    lcd.fillRect(kBtnStop.x, kBtnStop.y, kBtnStop.w, kBtnStop.h, kBg);
                }
            }

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
            case Page::Main:
                return Page::Home;

            case Page::Home:
                // Its own parent, so Back at the top does nothing rather than
                // navigating off the end of the tree.
                return Page::Home;
        }

        return Page::Home;
    }

    bool handleTouch(const int x, const int y) {
        if (currentPage == Page::Home) {
            // NO GEAR TEST HERE. This page has no header, so the old gear box
            // in the top-right corner now sits squarely on the LUNGO tile -
            // testing it first would mean a tap meant for a lungo opened the
            // settings menu instead. SETTINGS has its own tile.
            for (int i = 0; i < static_cast<int>(HomeAction::Count); i++) {
                const auto a = static_cast<HomeAction>(i);

                if (!inside(homeHit(a), x, y)) {
                    continue;
                }

                // EVERY ONE OF THESE IS A REQUEST. The head cannot drive an
                // output; it asks the Teensy, which runs the same gates the
                // brew switch and the console go through and is free to say
                // no. Nothing here assumes it worked - the next telemetry
                // frame says what actually happened.
                switch (a) {
                    case HomeAction::Ristretto:
                    case HomeAction::Espresso:
                    case HomeAction::Lungo: {
                        char cmd[24];
                        snprintf(cmd, sizeof(cmd), "shot=%d", i);
                        machinelink::sendCommand(cmd);

                        // Straight to the readout, because that is what you
                        // want to be looking at for the next thirty seconds.
                        // If the controller refuses, the readout says so in
                        // its status strip rather than this page guessing.
                        setPage(Page::Main);

                        break;
                    }

                    case HomeAction::Manual:
                        // A toggle: the pump runs until tapped again. Its cap
                        // (flush.max_time) cannot be disabled, so a panel that
                        // dies mid-run does not leave the pump going.
                        machinelink::sendCommand(manualRunning ? "flush=0" : "flush=1");
                        manualRunning = !manualRunning;
                        setPage(Page::Main);

                        break;

                    case HomeAction::Flush:
                        machinelink::sendCommand("flush=1");
                        setPage(Page::Main);

                        break;

                    case HomeAction::Backflush:
                        machinelink::sendCommand("backflush=1");
                        setPage(Page::Main);

                        break;

                    case HomeAction::Scale:
                        machinelink::sendCommand("tare=1");

                        break;

                    case HomeAction::Status:
                        // The readout IS the status panel. It was the top page
                        // before this grid existed and is kept unchanged, so
                        // there is no second page showing the same numbers.
                        setPage(Page::Main);

                        break;

                    case HomeAction::Settings:
                        setPage(Page::Setup);

                        break;

                    case HomeAction::Count:
                        break;
                }

                return true;
            }

            return false;
        }

        if (currentPage == Page::Main) {
            // STOP FIRST, before the gear and before tap-to-go-home. It is
            // the one control on this page that must never be swallowed by
            // something else, and it is only live while the machine is doing
            // something - lastStopShown says whether it is painted, so a tap
            // on blank space cannot stop a machine that is already idle.
            if (lastStopShown == 1 && inside(kBtnStop, x, y)) {
                // One command; the CONTROLLER decides what to stop. Telemetry
                // is up to a frame old, so a panel choosing between shotstop,
                // flush and backflush could pick the wrong one exactly as a
                // sequence changes phase.
                machinelink::sendCommand("stop=1");
                manualRunning = false;

                return true;
            }

            if (inside(kGear, x, y)) {
                setPage(Page::Setup);

                return true;
            }

            // Anywhere else goes back to the grid. No BACK button, because
            // this page is a dense readout with no spare room for one and
            // every pixel of it is already spoken for - see drawMainChrome().
            // Navigating away does NOT affect a running shot: the Teensy owns
            // that, and the grid's footer keeps showing the state.
            setPage(Page::Home);

            return true;
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
            setPage(Page::Home);

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
        // Hold the SPI bus open across the whole redraw.
        //
        // Without this, every primitive acquires and releases the bus on its
        // own, and a page is dozens of them. The pixels come out identical
        // either way; what changes is the per-call transaction overhead.
        //
        // Safe to hold here because everything between these two calls is
        // local drawing. No touch read - that is a separate I2C bus - no link
        // service, nothing that blocks or yields. Do not put anything of that
        // kind inside this block.
        lcd.startWrite();

        if (!chromeDrawn) {
            chromeDrawn = true;

            switch (currentPage) {
                case Page::Home: drawHomeChrome(); break;
                case Page::Main: drawMainChrome(); break;
                case Page::Setup: drawSetupChrome(); break;
                case Page::SetupBrew: drawBrewChrome(); break;
                case Page::SetupDisplay: drawDisplayChrome(); break;
                case Page::SetupWifi: drawWifiChrome(); break;
            }
        }

        switch (currentPage) {
            case Page::Home: drawHomeValues(); break;
            case Page::Main: drawMainValues(); break;
            case Page::Setup: drawSetupValues(); break;
            case Page::SetupBrew: drawBrewValues(); break;
            case Page::SetupDisplay: drawDisplayValues(); break;
            case Page::SetupWifi: drawWifiValues(); break;
        }

        lcd.endWrite();
    }

    uint32_t benchmarkRedraw(const uint8_t iterations) {
        // Forces a FULL repaint each pass - chrome and every value - so the
        // number is the worst case rather than the usual change-detected one.
        const uint32_t start = micros();

        for (uint8_t i = 0; i < iterations; i++) {
            invalidate();
            drawMachineScreen();
        }

        return (micros() - start) / iterations;
    }

} // namespace display
