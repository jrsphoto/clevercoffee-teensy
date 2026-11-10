/**
 * @file uiLvgl.cpp
 *
 * @brief LVGL on this panel, implementing the existing `display::` interface
 *
 * This REPLACES display.cpp rather than sitting beside it. The two are
 * alternative implementations of the same namespace, chosen by the build env,
 * which is why `main.cpp` and `net.cpp` need no changes at all: they go on
 * calling display::init(), display::loop(), display::drawMachineScreen() and
 * display::showMessage(), and something different happens underneath.
 *
 * That was a deliberate choice over introducing a new interface. Keeping the
 * seam where it already is means the two UIs can be compared at the same entry
 * points, and the switch-over is a build filter rather than a rewrite of the
 * boot sequence.
 *
 * WHAT LIVES WHERE
 *
 *   uiLvgl.cpp     this file. LVGL bring-up, the panel glue, the display::
 *                  functions the rest of the head calls.
 *   uiContract.h   the names SquareLine must export.
 *   uiBind.cpp     telemetry into widgets, and every rule about honesty.
 *   ui/            the SquareLine export. GENERATED. Never hand-edited.
 *
 * NAVIGATION IS NOT HERE YET. The hand-drawn build has a page tree and a
 * table of touch rectangles; with LVGL a page is a screen and a button knows
 * its own area, so that all moves into the export plus event handlers. Phase
 * 1 is the readout page only - see brief 9.15.
 */

#include <Arduino.h>
#include <lvgl.h>

#include "display.h"
#include "uiBind.h"
#include "uiContract.h"

namespace display {

    /** The one panel instance. Declared extern in display.h. */
    LGFX lcd;

    namespace {

        /**
         * How much of the screen LVGL renders at a time.
         *
         * LVGL draws into a buffer and hands finished rectangles to the panel,
         * rather than writing at the panel directly the way the hand-drawn
         * code did. A tenth of the screen is what LVGL's own documentation
         * recommends for a partial buffer.
         *
         * Two of them, so one can be filled while the other goes out over SPI.
         *
         * 480 x 320 / 10 x 2 bytes = 30720 bytes each, in INTERNAL RAM rather
         * than PSRAM. That choice is measured, not assumed: brief 9.7 found
         * PSRAM blits more slowly, and this buffer is touched on every single
         * repaint. Measured cost of both: about 61 KB of the board's 320 KB,
         * leaving ~91 KB free with WiFi, the web server and the link running.
         */
        constexpr uint32_t kDrawBufPixels = (kPanelWidth * kPanelHeight) / 10;
        constexpr uint32_t kDrawBufBytes = kDrawBufPixels * 2; // RGB565

        uint8_t* drawBuf1 = nullptr;
        uint8_t* drawBuf2 = nullptr;

        lv_display_t* lvDisplay = nullptr;
        lv_indev_t* lvTouch = nullptr;

        bool ready = false;
        uint8_t currentRotation = 1; // landscape

        /**
         * The full-screen message used during WiFi setup.
         *
         * Named here rather than hidden in a static inside showMessage()
         * because drawMachineScreen() has to be able to ask "are we showing
         * THIS?" - see the note there.
         */
        lv_obj_t* messageScreen = nullptr;

        /**
         * @brief Hand a finished rectangle to the panel.
         *
         * The one piece of glue between LVGL and LovyanGFX.
         *
         * BYTE ORDER, which cost a visibly wrong first run and does not look
         * like a colour fault. RGB565 is two bytes a pixel; the ST7796 wants
         * the high byte first and an ESP32 stores a uint16_t low byte first.
         * Pushing a raw uint16_t* sends memory order, so every colour comes
         * out wrong - and because anti-aliased text then picks up coloured
         * fringes, what a person SAYS is "the fonts look blurry".
         *
         * Fixed by rendering in the panel's order from the start, in
         * startLvgl(). Do NOT also call lv_draw_sw_rgb565_swap() here: two
         * swaps put the colours back exactly where they were.
         */
        void flushCb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
            const int32_t w = area->x2 - area->x1 + 1;
            const int32_t h = area->y2 - area->y1 + 1;

            lcd.startWrite();
            lcd.pushImageDMA(area->x1, area->y1, w, h, reinterpret_cast<uint16_t*>(px_map));
            lcd.endWrite();

            lv_display_flush_ready(disp);
        }

        /**
         * @brief Report the touch panel's state to LVGL.
         *
         * The touch read is the same call the hand-drawn build made; only the
         * reporting differs. LVGL is told where the finger is and works out
         * which widget that was, instead of the UI comparing coordinates
         * against a table of rectangles kept in step with the drawing by hand.
         * That table was two sources of truth for one button.
         */
        void touchCb(lv_indev_t*, lv_indev_data_t* data) {
            uint16_t x = 0;
            uint16_t y = 0;

            if (lcd.getTouch(&x, &y)) {
                data->point.x = x;
                data->point.y = y;
                data->state = LV_INDEV_STATE_PRESSED;
            }
            else {
                data->state = LV_INDEV_STATE_RELEASED;
            }
        }

        /** @brief Start LVGL on this panel. False if it could not be done. */
        bool startLvgl() {
            lv_init();

            // LVGL needs to know what time it is. Handing it millis() is fewer
            // moving parts than a separate tick timer, and cannot drift from
            // the clock the rest of the firmware uses.
            lv_tick_set_cb([]() -> uint32_t { return millis(); });

            drawBuf1 = static_cast<uint8_t*>(
                heap_caps_malloc(kDrawBufBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
            drawBuf2 = static_cast<uint8_t*>(
                heap_caps_malloc(kDrawBufBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));

            if (drawBuf1 == nullptr || drawBuf2 == nullptr) {
                Serial.printf("lvgl: could not allocate two %u byte DMA draw buffers\n",
                              static_cast<unsigned>(kDrawBufBytes));

                return false;
            }

            lvDisplay = lv_display_create(lcd.width(), lcd.height());

            if (lvDisplay == nullptr) {
                Serial.println("lvgl: lv_display_create failed");

                return false;
            }

            // Render in the panel's byte order. See flushCb above for what
            // getting this wrong looks like.
            //
            // A RUNTIME call rather than the lv_conf.h key, ON PURPOSE: 9.6
            // has LV_COLOR_FORMAT_DEFAULT and 9.5 has no such key, while this
            // call exists in both. The LVGL version is pinned to whatever the
            // SquareLine project selects, so it is expected to move.
            lv_display_set_color_format(lvDisplay, LV_COLOR_FORMAT_RGB565_SWAPPED);

            lv_display_set_flush_cb(lvDisplay, flushCb);
            lv_display_set_buffers(lvDisplay, drawBuf1, drawBuf2, kDrawBufBytes,
                                   LV_DISPLAY_RENDER_MODE_PARTIAL);

            lvTouch = lv_indev_create();
            lv_indev_set_type(lvTouch, LV_INDEV_TYPE_POINTER);
            lv_indev_set_read_cb(lvTouch, touchCb);
            lv_indev_set_display(lvTouch, lvDisplay);

            Serial.printf("lvgl: %d.%d.%d, %dx%d, two %u byte internal DMA buffers, UI %s\n",
                          LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH,
                          lcd.width(), lcd.height(), static_cast<unsigned>(kDrawBufBytes),
                          UI_EXPORT_PRESENT ? "from SquareLine export" : "PLACEHOLDER (no export yet)");

            return true;
        }

    } // namespace

    bool init() {
        if (!lcd.init()) {
            return false;
        }

        lcd.setRotation(currentRotation);
        lcd.setBrightness(200);
        lcd.fillScreen(TFT_BLACK);

        if (!startLvgl()) {
            return false;
        }

        // SquareLine's entry point, or the placeholder standing in for it.
        ui_init();

        uibind::begin();
        uibind::update();

        lv_refr_now(nullptr);

        ready = true;

        return true;
    }

    void loop() {
        if (!ready) {
            return;
        }

        // LVGL's clock, input polling and redraw. Everything else in the
        // head's loop is non-blocking, which is what makes calling this often
        // possible.
        lv_timer_handler();
    }

    void drawMachineScreen() {
        if (!ready) {
            return;
        }

        // COME BACK FROM A MESSAGE SCREEN. Without this the head shows the
        // WiFi message for ever: showMessage() loads a different LVGL screen,
        // and an LVGL screen stays loaded until something loads another one.
        // Every update below would then be applied to a screen nobody can
        // see, which is exactly what happened on the first run.
        //
        // The hand-drawn build could not have this bug - showMessage() there
        // painted over the same screen and the next repaint simply covered
        // it. display.h states the intended behaviour and it is still right:
        // a telemetry frame arriving means the machine has something more
        // useful to say than a message about setup. This makes it true again
        // under a framework where screens are objects rather than paint.
        // COME BACK FROM THE WIFI MESSAGE, and from nothing else.
        //
        // showMessage() loads a separate LVGL screen and an LVGL screen stays
        // loaded until something loads another, so without this the head sits
        // on the WiFi message for ever while the readout updates where nobody
        // can see it. display.h already states the intent: a telemetry frame
        // arriving means the machine has something more useful to say than a
        // message about setup.
        //
        // WRITTEN AS "is the message showing", NOT "is this a screen I know".
        // The first version listed the screens it would allow and sent
        // everything else home, which meant every screen added afterwards was
        // dragged away half a second after being opened - SETUP bounced
        // straight back to the grid the moment it existed. A rule that has to
        // be updated whenever a screen is added will eventually not be.
        if (messageScreen != nullptr && lv_screen_active() == messageScreen) {
            lv_screen_load(ui_ScreenHome != nullptr ? ui_ScreenHome : ui_ScreenReadout);
        }

        uibind::update();
    }

    void invalidate() {
        if (!ready) {
            return;
        }

        uibind::invalidate();
    }

    uint32_t benchmarkRedraw(const uint8_t iterations) {
        if (!ready) {
            return 0;
        }

        return uibind::benchmarkRedraw(iterations);
    }

    void showMessage(const char* title, const char* line1, const char* line2) {
        if (!ready) {
            return;
        }

        // A separate screen rather than clearing the active one, so the
        // readout and its chart history survive a trip through the WiFi
        // portal. The hand-drawn version had to repaint from scratch
        // afterwards; here the old screen is simply still there.
        static lv_obj_t* lines[3] = {nullptr, nullptr, nullptr};

        if (messageScreen == nullptr) {
            messageScreen = lv_obj_create(nullptr);

            lv_obj_set_style_bg_color(messageScreen, lv_color_hex(0x000000), 0);
            lv_obj_set_style_bg_opa(messageScreen, LV_OPA_COVER, 0);
            lv_obj_remove_flag(messageScreen, LV_OBJ_FLAG_SCROLLABLE);

            const struct {
                    const lv_font_t* font;
                    uint32_t colour;
                    int y;
            } spec[3] = {
                {&lv_font_montserrat_20, 0xFFFFFF, -60},
                {&lv_font_montserrat_16, 0xE0E0E0, 0},
                {&lv_font_montserrat_16, 0x00FFFF, 40},
            };

            for (int i = 0; i < 3; i++) {
                lines[i] = lv_label_create(messageScreen);

                lv_obj_set_style_text_font(lines[i], spec[i].font, 0);
                lv_obj_set_style_text_color(lines[i], lv_color_hex(spec[i].colour), 0);
                lv_obj_align(lines[i], LV_ALIGN_CENTER, 0, spec[i].y);
            }
        }

        const char* text[3] = {title, line1, line2};

        for (int i = 0; i < 3; i++) {
            lv_label_set_text(lines[i], text[i] != nullptr ? text[i] : "");
        }

        lv_screen_load(messageScreen);
        lv_refr_now(nullptr);
    }

    void showBringupScreen() {
        // The hand-drawn build's panel-info screen. Nothing here needs it:
        // the readout IS the bring-up screen now, and it says more.
        if (ready) {
            uibind::update();
            lv_refr_now(nullptr);
        }
    }

    void setRotation(const uint8_t rotation) {
        currentRotation = rotation & 3;
        lcd.setRotation(currentRotation);

        if (lvDisplay != nullptr) {
            lv_display_set_resolution(lvDisplay, lcd.width(), lcd.height());
            invalidate();
        }
    }

    void toggleInvert() {
        static bool inverted = false;

        inverted = !inverted;
        lcd.invertDisplay(inverted);
    }

    void setBrightness(const uint8_t level) {
        lcd.setBrightness(level);
    }

    uint8_t rotation() {
        return currentRotation;
    }

} // namespace display
