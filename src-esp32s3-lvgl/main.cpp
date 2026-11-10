/**
 * @file main.cpp
 *
 * @brief ESP32-S3 head, LVGL TRIAL build. Not the shipping firmware.
 *
 * One page of the UI on LVGL instead of hand-drawn LovyanGFX, so the choice
 * between the two can be made on measurements. See trialPage.h for the why and
 * platformio.ini's `[env:esp32s3_lvgl]` for what is and is not compiled in.
 *
 * Everything that is not the UI is the SAME CODE as the real head: link.cpp,
 * net.cpp and headConfig.cpp are compiled in unchanged. So WiFi, the web
 * server, the parameter registry and the Teensy link are all running
 * underneath, which is the only way the measurement means anything - LVGL on
 * an otherwise idle board would prove nothing about this board.
 *
 * STILL NO ACTUATORS AND NO SAFETY, exactly as the real head. This board
 * cannot drive anything, and this build does not even send commands: the
 * trial page has no touch actions, so a throwaway UI has no way to ask the
 * Teensy for a shot.
 */

#include <Arduino.h>
#include <lvgl.h>

#include "display.h" // the LGFX board description, reused rather than copied
#include "headConfig.h"
#include "link.h"
#include "net.h"
#include "trialPage.h"

namespace display {

    /**
     * The panel instance.
     *
     * `display.h` declares this `extern` and `display.cpp` normally defines
     * it, but `display.cpp` is the hand-drawn UI and is excluded from this
     * build. Defining it here reuses the board description - pins, SPI
     * frequency, touch - without duplicating it, so the two builds cannot
     * drift apart on what the hardware is.
     */
    LGFX lcd;

    /**
     * @brief Stand-in for the real `showMessage()`.
     *
     * `net.cpp` calls this to put the WiFi portal's network name and address
     * on screen, and it is compiled in unchanged. Rather than stub it out to
     * nothing - which would make the trial quietly worse than the real head at
     * the one moment the screen matters most - it is drawn with LVGL.
     *
     * Deliberately NOT routed through trialPage: it has to work before the
     * page is built, and during the portal there is no telemetry to show
     * anyway.
     */
    void showMessage(const char* title, const char* line1, const char* line2) {
        lv_obj_t* scr = lv_screen_active();

        // Clear whatever is there. On the real head this is a fillScreen and
        // some text; here the page may already exist, so its objects go.
        lv_obj_clean(scr);

        lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

        const struct {
                const char* text;
                const lv_font_t* font;
                uint32_t colour;
                int y;
        } lines[] = {
            {title, &lv_font_montserrat_20, 0xFFFFFF, -60},
            {line1, &lv_font_montserrat_16, 0xE0E0E0, 0},
            {line2, &lv_font_montserrat_16, 0x00FFFF, 40},
        };

        for (const auto& l : lines) {
            if (l.text == nullptr || l.text[0] == '\0') {
                continue;
            }

            lv_obj_t* label = lv_label_create(scr);

            lv_label_set_text(label, l.text);
            lv_obj_set_style_text_font(label, l.font, 0);
            lv_obj_set_style_text_color(label, lv_color_hex(l.colour), 0);
            lv_obj_align(label, LV_ALIGN_CENTER, 0, l.y);
        }

        lv_refr_now(nullptr);
    }

} // namespace display

namespace {

    constexpr uint32_t kHeartbeatMs = 5000;
    uint32_t lastHeartbeat = 0;

    bool lcdReady = false;
    bool pageBuilt = false;

    uint32_t lastDrawnFrame = 0;

    /**
     * How much of the screen LVGL renders at a time.
     *
     * LVGL draws into a buffer and hands finished rectangles to the panel,
     * rather than writing pixels at the panel directly the way the hand-drawn
     * code does. A tenth of the screen is the size the LVGL documentation
     * recommends for a partial buffer, and it is what the ESP32 examples use.
     *
     * Two of them, so LVGL can fill one while the other is going out over SPI.
     *
     * 480 x 320 / 10 x 2 bytes = 30720 bytes each. In INTERNAL RAM, not
     * PSRAM, and that choice matters: the brief's note (9.7) measured that
     * PSRAM blits more slowly, and this buffer is touched on every repaint. At
     * 30 KB each the board can afford both internally - it was using 58 KB of
     * 320 KB before this.
     */
    constexpr uint32_t kDrawBufPixels = (display::kPanelWidth * display::kPanelHeight) / 10;
    constexpr uint32_t kDrawBufBytes = kDrawBufPixels * 2; // RGB565

    uint8_t* drawBuf1 = nullptr;
    uint8_t* drawBuf2 = nullptr;

    lv_display_t* lvDisplay = nullptr;
    lv_indev_t* lvTouch = nullptr;

    /**
     * @brief Hand a finished rectangle to the panel.
     *
     * The one piece of glue between LVGL and LovyanGFX. LVGL has rendered
     * `area` into `px_map` and this pushes it at the display; `pushImageDMA`
     * returns as soon as the transfer is queued, and LovyanGFX blocks on the
     * next bus access, which is what lets the second buffer be useful.
     *
     * BYTE ORDER, which cost a visibly wrong first run. RGB565 is two bytes a
     * pixel and the ST7796 wants the high byte first, while an ESP32 stores a
     * uint16_t low byte first. Handing LovyanGFX a raw uint16_t* pushes it in
     * memory order, so without a swap every colour is wrong: cyan came out
     * yellowish, the dim greys went blue, and anti-aliased text picked up
     * coloured fringes that read as BLUR rather than as a colour fault.
     *
     * Fixed by rendering in the panel's order in the first place - see the
     * lv_display_set_color_format() call in startLvgl() - rather than calling
     * lv_draw_sw_rgb565_swap() here. Both work; rendering swapped costs
     * nothing per frame, while swapping in the callback is an extra pass over
     * 30 KB on every flush. Do not "fix" the colours by adding a swap here as
     * well, which would put them back the way they were.
     */
    void flushCb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
        const int32_t w = area->x2 - area->x1 + 1;
        const int32_t h = area->y2 - area->y1 + 1;

        display::lcd.startWrite();
        display::lcd.pushImageDMA(area->x1, area->y1, w, h,
                                  reinterpret_cast<uint16_t*>(px_map));
        display::lcd.endWrite();

        lv_display_flush_ready(disp);
    }

    /**
     * @brief Report the touch panel's state to LVGL.
     *
     * The FT6336U is on its own I2C bus and LovyanGFX owns it, so the touch
     * read is the same call the hand-drawn build makes. Only the reporting
     * differs: LVGL is told where the finger is and works out which object
     * that was, instead of the UI comparing coordinates against a table of
     * rectangles kept in step with the drawing by hand.
     */
    void touchCb(lv_indev_t* indev, lv_indev_data_t* data) {
        uint16_t x = 0;
        uint16_t y = 0;

        if (display::lcd.getTouch(&x, &y)) {
            data->point.x = x;
            data->point.y = y;
            data->state = LV_INDEV_STATE_PRESSED;
        }
        else {
            data->state = LV_INDEV_STATE_RELEASED;
        }
    }

    /**
     * @brief Start LVGL on this panel.
     *
     * @return false if the draw buffers could not be allocated, in which case
     *         the trial says so rather than showing a blank screen.
     */
    bool startLvgl() {
        lv_init();

        // LVGL needs to know what time it is. The alternative is calling
        // lv_tick_inc() from a timer; handing it millis() is fewer moving
        // parts and cannot drift from the clock everything else uses.
        lv_tick_set_cb([]() -> uint32_t { return millis(); });

        drawBuf1 = static_cast<uint8_t*>(heap_caps_malloc(kDrawBufBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
        drawBuf2 = static_cast<uint8_t*>(heap_caps_malloc(kDrawBufBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));

        if (drawBuf1 == nullptr || drawBuf2 == nullptr) {
            Serial.printf("lvgl: could not allocate two %u byte DMA draw buffers\n",
                          static_cast<unsigned>(kDrawBufBytes));

            return false;
        }

        lvDisplay = lv_display_create(display::lcd.width(), display::lcd.height());

        if (lvDisplay == nullptr) {
            Serial.println("lvgl: lv_display_create failed");

            return false;
        }

        // Render in the panel's byte order. See flushCb's comment for what
        // getting this wrong looks like.
        //
        // A RUNTIME call rather than the lv_conf.h key, ON PURPOSE. 9.6 has
        // LV_COLOR_FORMAT_DEFAULT; 9.5 has no such key and sets only the
        // depth in the config. This call exists in both and overrides either,
        // so the LVGL version can move - it already has, twice, while
        // settling which one SquareLine wanted - without the colours silently
        // reverting to the blurry-looking wrong ones.
        lv_display_set_color_format(lvDisplay, LV_COLOR_FORMAT_RGB565_SWAPPED);

        lv_display_set_flush_cb(lvDisplay, flushCb);
        lv_display_set_buffers(lvDisplay, drawBuf1, drawBuf2, kDrawBufBytes,
                               LV_DISPLAY_RENDER_MODE_PARTIAL);

        lvTouch = lv_indev_create();
        lv_indev_set_type(lvTouch, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(lvTouch, touchCb);
        lv_indev_set_display(lvTouch, lvDisplay);

        Serial.printf("lvgl: %d.%d.%d, %dx%d, two %u byte internal DMA buffers\n",
                      LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH,
                      display::lcd.width(), display::lcd.height(),
                      static_cast<unsigned>(kDrawBufBytes));

        return true;
    }

    void reportMemory(const char* when) {
        Serial.printf("%s: internal heap %u free (largest block %u), PSRAM %u free\n",
                      when,
                      static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                      static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                      static_cast<unsigned>(ESP.getFreePsram()));
    }

    /** @brief The measurements this build exists to take. */
    void printBenchmarks() {
        if (!pageBuilt) {
            Serial.println("page not built yet");

            return;
        }

        Serial.println();
        Serial.println("--- LVGL trial, measured on this board ---");

        const uint32_t full = trialpage::benchmarkRedraw(20);
        const uint32_t sample = trialpage::benchmarkChartSample(20);

        Serial.printf("full page repaint : %lu us  (hand-drawn build: 'drawbench' for its number)\n",
                      static_cast<unsigned long>(full));
        Serial.printf("one chart sample  : %lu us  (append + rescale + draw)\n",
                      static_cast<unsigned long>(sample));

        lv_mem_monitor_t mon;
        lv_mem_monitor(&mon);

        Serial.printf("LVGL pool         : %u of %u bytes used, %u%% fragmented, largest free %u\n",
                      static_cast<unsigned>(mon.total_size - mon.free_size),
                      static_cast<unsigned>(mon.total_size),
                      static_cast<unsigned>(mon.frag_pct),
                      static_cast<unsigned>(mon.free_biggest_size));

        reportMemory("after benchmarks  ");
        Serial.println("------------------------------------------");
        Serial.println();
    }

    void handleCommand(const String& line) {
        if (line == "bench") {
            printBenchmarks();
        }
        else if (line == "mem") {
            reportMemory("memory");

            lv_mem_monitor_t mon;
            lv_mem_monitor(&mon);
            Serial.printf("LVGL pool: %u of %u used, %u%% fragmented\n",
                          static_cast<unsigned>(mon.total_size - mon.free_size),
                          static_cast<unsigned>(mon.total_size),
                          static_cast<unsigned>(mon.frag_pct));
        }
        else if (line.startsWith("bl")) {
            const int b = line.substring(2).toInt();
            display::lcd.setBrightness(static_cast<uint8_t>(b));
            Serial.printf("brightness %d\n", b);
        }
        else if (line == "net" || line == "wifi") {
            net::printStatus();
        }
        else if (line == "wifireset") {
            Serial.println("forgetting the network and restarting...");
            net::forgetNetworkAndRestart();
        }
        else if (line.startsWith("cmd ")) {
            machinelink::sendCommand(line.substring(4).c_str());
            Serial.printf("sent: %s\n", line.substring(4).c_str());
        }
        else if (line == "help" || line.isEmpty()) {
            Serial.println("LVGL TRIAL build - one page only, no touch actions");
            Serial.println("commands: bench | mem | bl <0-255> | net | wifireset | cmd <body>");
            Serial.println("  bench   the numbers this build exists to produce");
            Serial.println("  mem     internal heap, PSRAM and the LVGL pool");
        }
        else {
            Serial.printf("unknown: %s  (this is the TRIAL build, not the head)\n", line.c_str());
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

            if (line.length() < 160) {
                line += c;
            }
        }
    }

} // namespace

void setup() {
    Serial.begin(115200);

    const uint32_t waitStart = millis();

    while (!Serial && (millis() - waitStart) < 1500) {
        // Courtesy pause for a bench console; never block a real boot.
    }

    Serial.println();
    Serial.println("========================================");
    Serial.println("CleverCoffee ESP32-S3 head - LVGL TRIAL");
    Serial.println("one page, for measurement. not the head.");
    Serial.println("========================================");

    reportMemory("at boot          ");

    // The link first, same order as the real head, so a Teensy already
    // running has somewhere to send while this board is still starting up.
    machinelink::init();

    if (!display::lcd.init()) {
        Serial.println("display: INIT FAILED - check the SPI pins against the LCDwiki page");
    }
    else {
        // Landscape. The hand-drawn build keeps rotation in a variable because
        // it is adjustable from its console; this trial has one page laid out
        // for 480x320 and no reason to rotate.
        display::lcd.setRotation(1);
        display::lcd.setBrightness(200);
        display::lcd.fillScreen(TFT_BLACK);

        if (startLvgl()) {
            lcdReady = true;

            trialpage::build();
            pageBuilt = true;

            lv_refr_now(nullptr);

            Serial.printf("display: ST7796S %dx%d, LVGL page up\n",
                          display::lcd.width(), display::lcd.height());
            reportMemory("after LVGL + page");
        }
    }

    net::mountFilesystem();
    headconfig::begin();
    net::begin();

    // The head holds the master configuration, so it tells the controller what
    // the settings are. Unchanged from the real head: this trial is not
    // allowed to leave the Teensy running on a different idea of the config
    // than a normal boot would.
    headconfig::pushAllToController();

    // showMessage() during the WiFi portal cleans the screen, so the page may
    // have been destroyed by now. Rebuild it if so.
    if (lcdReady) {
        trialpage::build();
        pageBuilt = true;
        trialpage::update();
        lv_refr_now(nullptr);
    }

    Serial.println("type 'bench' for the measurements, 'help' for commands");
}

void loop() {
    serviceConsole();
    machinelink::loop();
    net::loop();
    headconfig::loop();

    if (lcdReady) {
        // LVGL's own clock and redraw. Must be called regularly; everything
        // above it is non-blocking, which is what makes that possible.
        lv_timer_handler();
    }

    // Repaint on a new telemetry frame, the same trigger the real head uses -
    // a value pushed early by the Teensy reaches the screen promptly instead
    // of waiting out a fixed timer.
    //
    // No kRedrawMs floor here, deliberately: LVGL repaints only what changed,
    // so there is no full-screen cost to pace. Whether that holds is one of
    // the things this build is for.
    if (lcdReady && pageBuilt) {
        if (const uint32_t frames = machinelink::framesReceived(); frames != lastDrawnFrame) {
            lastDrawnFrame = frames;

            trialpage::update();
        }
    }

    static uint32_t lastHello = 0;

    if (const uint32_t hellos = machinelink::helloCount(); hellos != lastHello) {
        lastHello = hellos;

        Serial.println("controller restarted");

        headconfig::pushAllToController();
    }

    if (const uint32_t now = millis(); now - lastHeartbeat >= kHeartbeatMs) {
        lastHeartbeat = now;

        Serial.printf("alive %lus  link=%s frames=%lu badsum=%lu  heap=%u psram=%u\n",
                      static_cast<unsigned long>(now / 1000),
                      machinelink::isConnected() ? "OK" : "stale",
                      static_cast<unsigned long>(machinelink::framesReceived()),
                      static_cast<unsigned long>(machinelink::badChecksums()),
                      static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                      static_cast<unsigned>(ESP.getFreePsram()));
    }
}
