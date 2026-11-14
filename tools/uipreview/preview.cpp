/**
 * @file preview.cpp
 *
 * @brief Render the panel's screens on a PC, so a layout can be SEEN.
 *
 * The head's UI is written in firmware (`src-esp32s3/uiScreens.cpp`), which
 * means every layout change used to need a flash and a pair of eyes on a 4
 * inch panel across the room. This builds THE SAME FILE against LVGL on this
 * machine and writes one image per screen, so a wrong position or a label that
 * does not fit is visible in seconds.
 *
 * WHAT THIS IS AND IS NOT
 *
 * It is a LAYOUT tool. It compiles `uiScreens.cpp` - what exists, where, what
 * size and colour - and nothing else.
 *
 * It is NOT the machine. `uiBind.cpp` is deliberately absent, so no telemetry
 * arrives and every live value shows the placeholder text `uiScreens.cpp`
 * gives it: "--.-" for a temperature, "-" for a firmware version, "100%" for
 * the volume. That is honest rather than limiting - the alternative would be
 * inventing readings, and a preview showing numbers the machine never
 * reported is worse than one showing none.
 *
 * It also cannot tell you about the panel itself: backlight, viewing angle, or
 * the byte order the real display wants. See the colour note below.
 *
 * THE ONE THING THAT MUST NOT DRIFT
 *
 * It uses the head's own `lv_conf.h` (via `-I src-esp32s3`) and the same
 * pinned LVGL 9.6 the firmware links, taken straight out of
 * `.pio/libdeps/esp32s3/lvgl`. Point it at a different config or a different
 * version and it starts showing you something the panel does not draw, which
 * is the only way a tool like this can lie. `build.sh` enforces both.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <lvgl.h>

#include "uiContract.h"

namespace {

    constexpr int kW = 480;
    constexpr int kH = 320;

    /** The rendered frame, RGB565, filled by the flush callback. */
    uint16_t frame[kW * kH];

    /**
     * LVGL needs a clock. A counter is enough here: nothing animates, and a
     * real clock would only make the output differ between runs.
     */
    uint32_t fakeTicks = 0;

    uint32_t tickCb() {
        return fakeTicks;
    }

    void flushCb(lv_display_t* disp, const lv_area_t* area, uint8_t* px) {
        const int32_t w = area->x2 - area->x1 + 1;
        const auto* src = reinterpret_cast<const uint16_t*>(px);

        for (int32_t y = area->y1; y <= area->y2; y++) {
            memcpy(&frame[y * kW + area->x1], &src[(y - area->y1) * w],
                   static_cast<size_t>(w) * 2);
        }

        lv_display_flush_ready(disp);
    }

    /** @brief Write the frame as a binary PPM - no image library needed. */
    bool writePpm(const char* path) {
        FILE* f = fopen(path, "wb");

        if (f == nullptr) {
            fprintf(stderr, "cannot write %s\n", path);

            return false;
        }

        fprintf(f, "P6\n%d %d\n255\n", kW, kH);

        for (int i = 0; i < kW * kH; i++) {
            const uint16_t c = frame[i];

            // RGB565 to RGB888, replicating the high bits into the low ones so
            // full-scale stays full-scale (0x1F -> 0xFF, not 0xF8).
            const uint8_t r = static_cast<uint8_t>(((c >> 11) & 0x1F) * 255 / 31);
            const uint8_t g = static_cast<uint8_t>(((c >> 5) & 0x3F) * 255 / 63);
            const uint8_t b = static_cast<uint8_t>((c & 0x1F) * 255 / 31);

            fputc(r, f);
            fputc(g, f);
            fputc(b, f);
        }

        fclose(f);

        return true;
    }

    void shoot(lv_display_t* disp, lv_obj_t* screen, const char* name) {
        if (screen == nullptr) {
            printf("  %-14s SKIPPED - the symbol is null\n", name);

            return;
        }

        lv_screen_load(screen);

        // Twice, with the clock moved on between: the first pass loads the
        // screen and the second paints it settled. One pass can catch a
        // transition and produce a half-drawn image.
        for (int i = 0; i < 2; i++) {
            fakeTicks += 100;
            lv_timer_handler();
            lv_refr_now(disp);
        }

        char path[256];
        snprintf(path, sizeof(path), "out/%s.ppm", name);

        if (writePpm(path)) {
            printf("  %-14s -> %s\n", name, path);
        }
    }

} // namespace

int main() {
    lv_init();

    // LVGL's own messages. Without a print callback they go nowhere, which is
    // how a crash in here first looked like a crash with no explanation.
    lv_log_register_print_cb([](lv_log_level_t, const char* msg) {
        fputs(msg, stderr);
    });

    lv_tick_set_cb(tickCb);

    lv_display_t* disp = lv_display_create(kW, kH);

    static uint8_t drawBuf[kW * kH * 2];
    lv_display_set_buffers(disp, drawBuf, nullptr, sizeof(drawBuf),
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, flushCb);

    // PLAIN RGB565, not the RGB565_SWAPPED the real panel is set to. That is a
    // deliberate difference: the swap exists because the ST7796 wants the high
    // byte first, and applying it here would only put the bytes back the wrong
    // way round for a PPM. Colours in these images are what the firmware asked
    // for; whether the panel shows them correctly is a hardware question, and
    // getting the swap wrong there reads as blurry fonts rather than wrong
    // colours. See TEENSY-PORT-BRIEF.md 9.15.
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);

    printf("rendering %dx%d from uiScreens.cpp\n", kW, kH);

    ui_init();

    // Every screen the contract declares. ui_init() assigns ScreenReadout from
    // whatever was active when it ran, so it is captured like the rest.
    shoot(disp, ui_ScreenHome, "1-home");
    shoot(disp, ui_ScreenReadout, "2-readout");
    shoot(disp, ui_ScreenSetup, "3-setup");
    shoot(disp, ui_ScreenDisplay, "4-display");
    shoot(disp, ui_ScreenBrew, "5-brew");
    shoot(disp, ui_ScreenWifi, "6-wifi");
    shoot(disp, ui_ScreenAbout, "7-about");

    printf("done\n");

    return 0;
}
