/**
 * @file uiBind.cpp
 *
 * @brief Telemetry into the exported widgets. See uiBind.h for the rules.
 */

#include "uiBind.h"

#include <Arduino.h>
#include <lvgl.h>
#include <stdio.h>
#include <string.h>

#include <Preferences.h>

#include "audio.h"
#include "display.h"
#include "link.h"
#include "net.h"
#include "uiContract.h"

namespace uibind {

    namespace {

        /**
         * Chart sampling.
         *
         * One sample every two seconds across 230 points is a window of 460
         * seconds, about seven and a half minutes - a heat-up from cold with
         * room to spare. Matches what the hand-drawn chart did, so the trace
         * looks the same after the change.
         */
        constexpr uint32_t kSampleIntervalMs = 2000;
        constexpr uint16_t kChartPoints = 230;

        /**
         * Chart values are stored as TENTHS of a degree.
         *
         * lv_chart works in int32, and a boiler range of 20 to 130 C has only
         * 110 integer steps across 84 pixels - visibly steppy. Tenths give
         * 1100 and the arithmetic stays exact.
         */
        constexpr int32_t kTenths = 10;

        /**
         * The accent for the reading, dim grey for the setpoint.
         *
         * 0x278ADE is John's pick, 2026-11-13, replacing the old cyan.
         * SQUARELINE_UI.md records it under his layout decisions and names
         * this exact case: the boiler reading and the chart trace are coloured
         * at RUNTIME because the colour carries meaning (healthy against
         * faulty), so a value set anywhere else would be overwritten within a
         * second of boot. It must match `kAccent` in uiScreens.cpp.
         */
        const lv_color_t kTraceColour = lv_color_hex(0x278ADE);
        const lv_color_t kSetpointColour = lv_color_hex(0x404040);

        /** Colours for values whose meaning changes with their state. */
        const lv_color_t kValue = lv_color_hex(0xE0E0E0);
        const lv_color_t kDim = lv_color_hex(0x404040);
        const lv_color_t kReady = lv_color_hex(0x00FF00);
        const lv_color_t kHeat = lv_color_hex(0xFFA400);
        const lv_color_t kAlarm = lv_color_hex(0xFF0000);
        const lv_color_t kLabelGrey = lv_color_hex(0x808080);

        lv_chart_series_t* traceSeries = nullptr;
        lv_chart_series_t* setpointSeries = nullptr;

        /**
         * The nine tiles and their sub-labels, gathered once.
         *
         * The widgets are nine separate globals because that is how SquareLine
         * exports them - one global per widget, no arrays. Everything here
         * wants to address them by index, so they are collected in begin() and
         * used from these two tables afterwards.
         */
        lv_obj_t* tiles[9] = {};
        lv_obj_t* tileSubs[9] = {};

        void gatherTiles() {
            lv_obj_t* const t[9] = {ui_HomeTile0, ui_HomeTile1, ui_HomeTile2,
                                    ui_HomeTile3, ui_HomeTile4, ui_HomeTile5,
                                    ui_HomeTile6, ui_HomeTile7, ui_HomeTile8};
            lv_obj_t* const s[9] = {ui_HomeTileSub0, ui_HomeTileSub1, ui_HomeTileSub2,
                                    ui_HomeTileSub3, ui_HomeTileSub4, ui_HomeTileSub5,
                                    ui_HomeTileSub6, ui_HomeTileSub7, ui_HomeTileSub8};

            for (int i = 0; i < 9; i++) {
                tiles[i] = t[i];
                tileSubs[i] = s[i];
            }
        }

        uint32_t lastSampleMs = 0;

        /** @brief Set a label's text, skipping a null widget. */
        void setText(lv_obj_t* label, const char* text) {
            if (label == nullptr) {
                return;
            }

            lv_label_set_text(label, text);
        }

        /** @brief Set a label's colour, skipping a null widget. */
        void setColour(lv_obj_t* label, const lv_color_t colour) {
            if (label == nullptr) {
                return;
            }

            lv_obj_set_style_text_color(label, colour, 0);
        }

        /**
         * @brief The status strip.
         *
         * Precedence copied deliberately from the hand-drawn version: no link,
         * then a declared fault, then a sensor fault, then heater off, then
         * ready, then heating. Reordering this changes what the machine
         * appears to be doing, so it is not to be "tidied".
         */
        const char* statusText(const machinelink::State& st, const bool live, lv_color_t* colour) {
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

        /**
         * @brief Append a chart sample, and rescale to keep the setpoint shown.
         *
         * An unhealthy reading is appended as LV_CHART_POINT_NONE so the trace
         * BREAKS rather than running a straight line across the gap. Both
         * series shift together, so the setpoint line stays aligned with the
         * data underneath it.
         */
        void sampleChart(const machinelink::State& st, const bool live) {
            if (ui_BoilerChart == nullptr || traceSeries == nullptr) {
                return;
            }

            const uint32_t now = millis();

            if ((now - lastSampleMs) < kSampleIntervalMs) {
                return;
            }

            lastSampleMs = now;

            const bool healthy = live && st.boilerOk;
            const bool haveSetpoint = live && st.setpointC > 0.0f;

            lv_chart_set_next_value(ui_BoilerChart, traceSeries,
                                    healthy ? static_cast<int32_t>(st.boilerC * kTenths)
                                            : LV_CHART_POINT_NONE);

            lv_chart_set_next_value(ui_BoilerChart, setpointSeries,
                                    haveSetpoint ? static_cast<int32_t>(st.setpointC * kTenths)
                                                 : LV_CHART_POINT_NONE);

            // Scale to the data, always keeping the setpoint in view - the
            // distance to it is the thing being read.
            const int32_t* ys = lv_chart_get_y_array(ui_BoilerChart, traceSeries);
            int32_t lo = INT32_MAX;
            int32_t hi = INT32_MIN;

            for (uint16_t i = 0; i < kChartPoints; i++) {
                if (ys[i] == LV_CHART_POINT_NONE) {
                    continue;
                }

                if (ys[i] < lo) lo = ys[i];
                if (ys[i] > hi) hi = ys[i];
            }

            if (lo == INT32_MAX) {
                return; // nothing plotted yet, leave the range alone
            }

            if (haveSetpoint) {
                const auto sp = static_cast<int32_t>(st.setpointC * kTenths);

                if (sp < lo) lo = sp;
                if (sp > hi) hi = sp;
            }

            // A floor on the span, so a settled boiler is a flat line rather
            // than a magnified picture of sensor noise.
            if ((hi - lo) < (10 * kTenths)) {
                const int32_t mid = (hi + lo) / 2;
                lo = mid - (5 * kTenths);
                hi = mid + (5 * kTenths);
            }

            const int32_t pad = (hi - lo) / 10;

            lv_chart_set_range(ui_BoilerChart, LV_CHART_AXIS_PRIMARY_Y, lo - pad, hi + pad);
        }


        // --- Tile actions ------------------------------------------------
        //
        // EVERY ONE OF THESE IS A REQUEST. The head cannot drive an output; it
        // asks the Teensy, which runs the same gates the brew switch and the
        // console go through and is free to refuse. Nothing here assumes it
        // worked - the next telemetry frame says what actually happened, which
        // is why none of these handlers change anything on screen except which
        // page is showing.

        /** Tile indices, matching the grid order in uiScreens.cpp. */
        enum TileId : int {
            Ristretto = 0,
            Espresso = 1,
            Lungo = 2,
            Manual = 3,
            Flush = 4,
            Backflush = 5,
            Scale = 6,
            Status = 7,
            Settings = 8,
        };

        void showReadout() {
            if (ui_ScreenReadout != nullptr) {
                lv_screen_load(ui_ScreenReadout);
            }
        }

        void showHome() {
            if (ui_ScreenHome != nullptr) {
                lv_screen_load(ui_ScreenHome);
            }
        }

        void tileClicked(lv_event_t* e) {
            const auto id = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
            const auto& st = machinelink::state();

            switch (id) {
                case Ristretto:
                case Espresso:
                case Lungo: {
                    char cmd[24];
                    snprintf(cmd, sizeof(cmd), "shot=%d", id);
                    machinelink::sendCommand(cmd);

                    // Straight to the readout, because that is what you want
                    // to be looking at for the next thirty seconds. If the
                    // controller refuses, the readout's status strip says so
                    // rather than this page guessing.
                    showReadout();

                    break;
                }

                case Manual:
                    // NOT a toggle. Tapping it switches to the readout, so the
                    // tile is not on screen for a second tap - an earlier
                    // version said "tap to stop" and there was nothing to tap.
                    // STOP on the readout is the way out, and it appears by
                    // itself because the machine is running.
                    machinelink::sendCommand("flush=1");
                    showReadout();

                    break;

                case Flush:
                    machinelink::sendCommand("flush=1");
                    showReadout();

                    break;

                case Backflush:
                    // FIRST TAP ARMS, second runs - the same two steps the
                    // console requires, and for the same reason: this drives
                    // the pump against a blind basket and the MG304 has no
                    // internal relief. One tap to start it is not a
                    // convenience worth having.
                    //
                    // The armed state comes from TELEMETRY, never from what
                    // this board last asked for: the controller disarms itself
                    // after a run and on reboot, and a tile claiming ARMED
                    // when the machine is not would be lying about exactly the
                    // wrong control.
                    if (machinelink::isConnected() && st.backflushArmed) {
                        machinelink::sendCommand("backflush=1");
                        showReadout();
                    }
                    else {
                        machinelink::sendCommand("backflusharm=1");
                    }

                    break;

                case Scale:
                    machinelink::sendCommand("tare=1");

                    break;

                case Status:
                    showReadout();

                    break;

                case Settings:
                    if (ui_ScreenSetup != nullptr) {
                        lv_screen_load(ui_ScreenSetup);
                    }

                    break;

                default:
                    break;
            }
        }

        // --- Setup pages -------------------------------------------------

        /**
         * Head-local settings, in the ESP32's own NVS rather than the shared
         * config.
         *
         * Deliberately NOT a config key. The shared config is pushed to the
         * controller at every handshake and has already caused three bugs by
         * one side holding a different default from the other - once
         * destructively, wiping a measured scale calibration. Which way up
         * this panel is mounted is nothing the Teensy needs to know, so it
         * does not go anywhere near that path.
         */
        constexpr char kPrefNamespace[] = "ccui";
        constexpr char kPrefRotation[] = "rot";
        constexpr char kPrefBrightness[] = "bl";
        constexpr char kPrefVolume[] = "vol";

        constexpr uint8_t kRotNormal = 1;  // landscape
        constexpr uint8_t kRotFlipped = 3; // landscape, 180 round

        uint8_t brightness = 200;

        void saveByte(const char* key, const uint8_t value) {
            Preferences prefs;

            if (prefs.begin(kPrefNamespace, false)) {
                prefs.putUChar(key, value);
                prefs.end();
            }
        }

        void showFlipState() {
            if (ui_FlipValue != nullptr) {
                lv_label_set_text(ui_FlipValue,
                                  display::rotation() == kRotFlipped ? "flipped 180" : "normal");
            }
        }

        void showBrightness() {
            if (ui_BrightnessValue != nullptr) {
                char buf[8];
                snprintf(buf, sizeof(buf), "%d", brightness);
                lv_label_set_text(ui_BrightnessValue, buf);
            }
        }

        void flipClicked(lv_event_t*) {
            const uint8_t next = (display::rotation() == kRotFlipped) ? kRotNormal : kRotFlipped;

            display::setRotation(next);

            // REMEMBERED. The panel can be mounted either way round in the
            // machine, so this is a property of the installation, not a
            // preference for this session. A flip that had to be redone on
            // every power-up would be worse than no button.
            saveByte(kPrefRotation, next);

            showFlipState();
        }

        void brightnessClicked(lv_event_t* e) {
            const bool up = lv_event_get_user_data(e) != nullptr;

            if (up) {
                brightness = (brightness < 215) ? static_cast<uint8_t>(brightness + 40) : 255;
            }
            else {
                brightness = (brightness > 40) ? static_cast<uint8_t>(brightness - 40) : 10;
            }

            display::setBrightness(brightness);
            saveByte(kPrefBrightness, brightness);
            showBrightness();
        }

        void showVolume() {
            if (ui_VolumeValue != nullptr) {
                char buf[8];
                snprintf(buf, sizeof(buf), "%d%%", audio::volumePercent());
                lv_label_set_text(ui_VolumeValue, buf);
            }
        }

        /**
         * Sound volume, in tens, matching the steps the hand-drawn page used.
         *
         * It PERSISTS, which the hand-drawn one did not. The codec comes up at
         * 100% every boot, so without this, turning the machine down had to be
         * done again at every power-up - the same argument as the FLIP button
         * above, and the same one-line mechanism. Head-local NVS, not the
         * shared config: what this panel sounds like is nothing the controller
         * needs to know.
         */
        void volumeClicked(lv_event_t* e) {
            const bool up = lv_event_get_user_data(e) != nullptr;
            const int v = audio::volumePercent();
            const auto next = static_cast<uint8_t>(up ? (v < 90 ? v + 10 : 100)
                                                      : (v > 10 ? v - 10 : 0));

            audio::setVolumePercent(next);
            saveByte(kPrefVolume, next);
            showVolume();
        }

        // --- Setup > BREW -------------------------------------------------

        void setpointClicked(lv_event_t* e) {
            const bool up = lv_event_get_user_data(e) != nullptr;
            const auto& st = machinelink::state();

            // Adjusts from the CONTROLLER's current value, not from a copy
            // kept here. The Teensy validates and may refuse, so a local copy
            // would drift away from the machine on the first refusal and then
            // every later nudge would be from a wrong starting point.
            char cmd[40];
            snprintf(cmd, sizeof(cmd), "setpoint=%.1f", st.setpointC + (up ? 0.5f : -0.5f));
            machinelink::sendCommand(cmd);
        }

        void heaterToggleClicked(lv_event_t*) {
            const auto& st = machinelink::state();
            char cmd[24];

            snprintf(cmd, sizeof(cmd), "heater=%d", st.heaterEnabled ? 0 : 1);
            machinelink::sendCommand(cmd);
        }

        void tareClicked(lv_event_t*) {
            machinelink::sendCommand("tare=1");
        }

        void zeroVolClicked(lv_event_t*) {
            machinelink::sendCommand("flowreset=1");
        }

        void clearFaultClicked(lv_event_t*) {
            machinelink::sendCommand("clearfault=1");
        }

        // --- Setup > WIFI ---------------------------------------------------

        /**
         * FORGET is armed by the first tap and acts on the second, and the
         * arming LAPSES.
         *
         * Same shape as BACKFLUSH. Dropping the stored network reboots the
         * machine into a setup portal, which is a long way back from a mis-tap,
         * and an arming that waited for ever would leave a loaded gun on the
         * page for the next person who touched it.
         */
        constexpr uint32_t kForgetArmedMs = 5000;

        bool forgetArmed = false;
        uint32_t forgetArmedAtMs = 0;

        void showForgetState() {
            if (ui_WifiForgetLbl != nullptr) {
                lv_label_set_text(ui_WifiForgetLbl, forgetArmed ? "TAP AGAIN" : "FORGET");
            }
        }

        void wifiSetupClicked(lv_event_t*) {
            net::forgetNetworkAndRestart();
        }

        void wifiForgetClicked(lv_event_t*) {
            if (forgetArmed) {
                forgetArmed = false;
                showForgetState();
                net::forgetNetworkAndRestart();

                return;
            }

            forgetArmed = true;
            forgetArmedAtMs = millis();
            showForgetState();
        }

        void showBrewPage(lv_event_t*) {
            if (ui_ScreenBrew != nullptr) {
                lv_screen_load(ui_ScreenBrew);
            }
        }

        void showWifiPage(lv_event_t*) {
            forgetArmed = false;
            showForgetState();

            if (ui_ScreenWifi != nullptr) {
                lv_screen_load(ui_ScreenWifi);
            }
        }

        void showSetup(lv_event_t*) {
            if (ui_ScreenSetup != nullptr) {
                lv_screen_load(ui_ScreenSetup);
            }
        }

        void showDisplayPage(lv_event_t*) {
            showBrightness();
            showVolume();
            showFlipState();

            if (ui_ScreenDisplay != nullptr) {
                lv_screen_load(ui_ScreenDisplay);
            }
        }

        // --- Setup > ABOUT ------------------------------------------------

        /**
         * @brief What the controller is running, or WHY that is not known.
         *
         * Two different situations and only one of them is a fault: the link
         * being down, and the link being up but the Teensy not having
         * announced itself since this board started. The second is ordinary -
         * the controller announces at ITS boot - and must not read like a
         * problem. Same wording as the web About page in net.cpp, deliberately.
         */
        const char* controllerVersionText() {
            const char* fw = machinelink::controllerFirmware();

            if (fw[0] == '\0' || (fw[0] == '-' && fw[1] == '\0')) {
                return machinelink::isConnected() ? "not announced since this board started"
                                                  : "unknown - no link to the controller";
            }

            return fw;
        }

        void showAboutPage(lv_event_t*) {
            if (ui_ScreenAbout != nullptr) {
                lv_screen_load(ui_ScreenAbout);
            }
        }

        void backToHome(lv_event_t*) {
            showHome();
        }

        void backToSetup(lv_event_t*) {
            if (ui_ScreenSetup != nullptr) {
                lv_screen_load(ui_ScreenSetup);
            }
        }

        void stopClicked(lv_event_t*) {
            machinelink::sendCommand("stop=1");
        }

        /** @brief Tapping the readout anywhere but STOP goes back to the grid. */
        void readoutClicked(lv_event_t*) {
            showHome();
        }

    } // namespace

    void begin() {
        lastSampleMs = millis();

        // One handler for all nine tiles, told which one it is. The hand-drawn
        // page compared touch coordinates against a table of rectangles that
        // had to be kept in step with the drawing; an LVGL button knows its
        // own area, so that table and its whole class of bug are gone.
        gatherTiles();

        for (int i = 0; i < 9; i++) {
            if (tiles[i] != nullptr) {
                lv_obj_add_event_cb(tiles[i], tileClicked, LV_EVENT_CLICKED,
                                    reinterpret_cast<void*>(static_cast<intptr_t>(i)));
            }
        }

        if (ui_StopButton != nullptr) {
            lv_obj_add_event_cb(ui_StopButton, stopClicked, LV_EVENT_CLICKED, nullptr);
        }

        // Tapping the readout returns to the grid. It has no BACK button
        // because every pixel of that page is spoken for.
        if (ui_ScreenReadout != nullptr) {
            lv_obj_add_flag(ui_ScreenReadout, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(ui_ScreenReadout, readoutClicked, LV_EVENT_CLICKED, nullptr);
        }

        // Setup navigation.
        const struct {
                lv_obj_t* widget;
                lv_event_cb_t handler;
                void* arg;
        } buttons[] = {
            {ui_SetupBack, backToHome, nullptr},
            {ui_SetupDisplayBtn, showDisplayPage, nullptr},
            {ui_DisplayBack, backToSetup, nullptr},
            {ui_FlipButton, flipClicked, nullptr},
            {ui_BrightnessDown, brightnessClicked, nullptr},
            {ui_BrightnessUp, brightnessClicked, reinterpret_cast<void*>(1)},
            {ui_VolumeDown, volumeClicked, nullptr},
            {ui_VolumeUp, volumeClicked, reinterpret_cast<void*>(1)},
        };

        for (const auto& b : buttons) {
            if (b.widget != nullptr) {
                lv_obj_add_event_cb(b.widget, b.handler, LV_EVENT_CLICKED, b.arg);
            }
        }

        const struct {
                lv_obj_t* widget;
                lv_event_cb_t handler;
                void* arg;
        } more[] = {
            {ui_SetupBrewBtn, showBrewPage, nullptr},
            {ui_SetupWifiBtn, showWifiPage, nullptr},
            {ui_BrewBack, backToSetup, nullptr},
            {ui_WifiBack, backToSetup, nullptr},
            {ui_SetpointDown, setpointClicked, nullptr},
            {ui_SetpointUp, setpointClicked, reinterpret_cast<void*>(1)},
            {ui_HeaterToggle, heaterToggleClicked, nullptr},
            {ui_TareButton, tareClicked, nullptr},
            {ui_ZeroVolButton, zeroVolClicked, nullptr},
            {ui_ClearFaultButton, clearFaultClicked, nullptr},
            {ui_WifiSetupBtn, wifiSetupClicked, nullptr},
            {ui_WifiForgetBtn, wifiForgetClicked, nullptr},
            {ui_SetupAboutBtn, showAboutPage, nullptr},
            {ui_AboutBack, backToSetup, nullptr},
        };

        for (const auto& b : more) {
            if (b.widget != nullptr) {
                lv_obj_add_event_cb(b.widget, b.handler, LV_EVENT_CLICKED, b.arg);
            }
        }

        // THE GRID IS THE ROOT PAGE, and that is decided here rather than in
        // the screens. SquareLine's generated ui_init() ends by loading
        // whichever screen is first in its project, which is the readout; the
        // hand-written screens ended by loading the grid. Setting it here
        // makes both behave the same, and navigation is this file's job
        // anyway.
        showHome();

        // Restore the head-local display settings. Rotation FIRST, because a
        // panel mounted upside down must come up the right way round without
        // anybody touching it.
        Preferences prefs;

        if (prefs.begin(kPrefNamespace, true)) {
            const uint8_t rot = prefs.getUChar(kPrefRotation, kRotNormal);
            brightness = prefs.getUChar(kPrefBrightness, 200);

            // The codec's own default is full volume, so that is the default
            // here too: a saved value is a deliberate change away from it.
            const uint8_t vol = prefs.getUChar(kPrefVolume, 100);
            prefs.end();

            if (rot != display::rotation()) {
                display::setRotation(rot);
            }

            display::setBrightness(brightness);
            audio::setVolumePercent(vol);
        }

        showFlipState();
        showBrightness();
        showVolume();

        // The head's own version never changes while it is running, so it is
        // written once. The controller's is live - see update().
        setText(ui_AboutHeadVersion, net::headVersion());
        setText(ui_AboutControllerVersion, controllerVersionText());

        if (ui_BoilerChart == nullptr) {
            Serial.println("ui: no chart widget in the export - graph disabled");

            return;
        }

        // The series are added HERE rather than in SquareLine because their
        // colours carry meaning (the reading against its target) and because
        // the gap handling below depends on owning them.
        lv_chart_set_type(ui_BoilerChart, LV_CHART_TYPE_LINE);
        lv_chart_set_point_count(ui_BoilerChart, kChartPoints);
        lv_chart_set_update_mode(ui_BoilerChart, LV_CHART_UPDATE_MODE_SHIFT);

        traceSeries = lv_chart_add_series(ui_BoilerChart, kTraceColour, LV_CHART_AXIS_PRIMARY_Y);
        setpointSeries = lv_chart_add_series(ui_BoilerChart, kSetpointColour, LV_CHART_AXIS_PRIMARY_Y);

        // Every point hidden to start with, so an empty chart is EMPTY rather
        // than a flat line along zero. Same value an unhealthy sample gets.
        for (uint16_t i = 0; i < kChartPoints; i++) {
            lv_chart_set_value_by_id(ui_BoilerChart, traceSeries, i, LV_CHART_POINT_NONE);
            lv_chart_set_value_by_id(ui_BoilerChart, setpointSeries, i, LV_CHART_POINT_NONE);
        }
    }

    void update() {
        const auto& st = machinelink::state();
        const bool live = machinelink::isConnected();

        char buf[32];

        // Boiler. "--.-" when there is no believable reading - never the last
        // good value, which is the rule the control firmware follows too.
        const bool boilerGood = live && st.boilerOk;

        snprintf(buf, sizeof(buf), boilerGood ? "%.1f" : "--.-", st.boilerC);
        setText(ui_BoilerValue, buf);
        setColour(ui_BoilerValue, boilerGood ? kTraceColour : kAlarm);

        snprintf(buf, sizeof(buf), "%.1f", live ? st.setpointC : 0.0f);
        setText(ui_SetpointValue, buf);

        const bool groupGood = live && st.groupOk;

        snprintf(buf, sizeof(buf), groupGood ? "%.1f" : "--.-", st.groupC);
        setText(ui_GroupValue, buf);
        setColour(ui_GroupValue, groupGood ? kValue : kDim);

        // Heater: the bar is the duty, the colour says whether it is allowed
        // to be doing anything at all.
        const int duty = live ? static_cast<int>(st.heaterDuty) : 0;

        if (ui_HeaterBar != nullptr) {
            lv_bar_set_value(ui_HeaterBar, duty, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(ui_HeaterBar, st.heaterEnabled ? kHeat : kDim,
                                      LV_PART_INDICATOR);
        }

        snprintf(buf, sizeof(buf), "%d%%", duty);
        setText(ui_HeaterPercent, buf);
        setColour(ui_HeaterPercent, st.heaterEnabled ? kHeat : kDim);

        // Weight, and the unit that goes with it. An uncalibrated scale
        // reports raw counts and MUST say so.
        const bool weightGood = live && st.weightOk;

        if (weightGood) {
            snprintf(buf, sizeof(buf), st.weightCalibrated ? "%.1f" : "%.0f", st.weightG);
        }
        else {
            snprintf(buf, sizeof(buf), "--");
        }

        setText(ui_WeightValue, buf);
        setColour(ui_WeightValue, weightGood ? (st.weightCalibrated ? kValue : kHeat) : kDim);
        setText(ui_WeightUnit, st.weightCalibrated ? "grams" : "raw counts");

        snprintf(buf, sizeof(buf), "%.1f", live ? st.flowMlPerSec : 0.0f);
        setText(ui_FlowValue, buf);
        setColour(ui_FlowValue, live ? kValue : kDim);

        snprintf(buf, sizeof(buf), "ml/s   %.0f ml", live ? st.volumeMl : 0.0f);
        setText(ui_FlowSub, buf);

        snprintf(buf, sizeof(buf), "%.0f%%", live ? st.pumpPercent : 0.0f);
        setText(ui_PumpValue, buf);
        setColour(ui_PumpValue, (live && st.pumpPercent > 0.0f) ? kReady : kDim);

        lv_color_t statusColour = kDim;
        const char* text = statusText(st, live, &statusColour);

        setText(ui_StatusLabel, text);
        setColour(ui_StatusLabel, statusColour);

        // --- The home screen -----------------------------------------
        //
        // Updated even while the readout is showing. It is a handful of label
        // writes and LVGL repaints only what changed, so there is nothing to
        // gain from tracking which page is in front - and plenty to lose if
        // that tracking is ever wrong.

        // BACKFLUSH says whether it is armed, because it REFUSES to run
        // otherwise and a tile that silently does nothing is the worst kind of
        // broken. Straight from telemetry: the controller disarms itself after
        // a run and on reboot.
        const bool armed = live && st.backflushArmed;

        if (tileSubs[5] != nullptr) {
            lv_label_set_text(tileSubs[5], armed ? "ARMED - tap to run" : "tap to arm");
            lv_obj_set_style_text_color(tileSubs[5], armed ? kAlarm : kDim, 0);
        }

        if (tiles[5] != nullptr) {
            lv_obj_set_style_border_color(tiles[5], armed ? kAlarm : kValue, 0);
        }

        if (live) {
            snprintf(buf, sizeof(buf), "%.1fC  %s", st.boilerC, text);
        }
        else {
            snprintf(buf, sizeof(buf), "%s", text);
        }

        setText(ui_HomeFooter, buf);
        setColour(ui_HomeFooter, statusColour);

        // STOP appears only while something is actually running, because a
        // STOP that does nothing teaches people the button does nothing.
        //
        // "Running" is taken from the PUMP, not from what the head last asked
        // for: the pump turning is the machine doing something, and it is
        // reported rather than assumed.
        if (ui_StopButton != nullptr) {
            const bool running = live && st.pumpPercent > 0.0f;

            if (running) {
                lv_obj_remove_flag(ui_StopButton, LV_OBJ_FLAG_HIDDEN);
            }
            else {
                lv_obj_add_flag(ui_StopButton, LV_OBJ_FLAG_HIDDEN);
            }
        }

        // --- Setup > BREW, kept live ---------------------------------
        snprintf(buf, sizeof(buf), live ? "%.1f C" : "--.- C", st.setpointC);
        setText(ui_SetpointShown, buf);

        if (ui_HeaterToggleLbl != nullptr) {
            lv_label_set_text(ui_HeaterToggleLbl, st.heaterEnabled ? "HEAT ON" : "HEAT OFF");
            lv_obj_set_style_text_color(ui_HeaterToggleLbl, st.heaterEnabled ? kHeat : kValue, 0);
        }

        if (ui_HeaterToggle != nullptr) {
            lv_obj_set_style_border_color(ui_HeaterToggle, st.heaterEnabled ? kHeat : kDim, 0);
        }

        if (live) {
            snprintf(buf, sizeof(buf), "%.1f C  %s", st.boilerC, text);
        }
        else {
            snprintf(buf, sizeof(buf), "%s", text);
        }

        setText(ui_BrewStatus, buf);
        setColour(ui_BrewStatus, statusColour);

        // --- Setup > WIFI --------------------------------------------
        setText(ui_WifiState, net::stateName());
        setColour(ui_WifiState, net::state() == net::State::Online   ? kReady
                                : net::state() == net::State::Portal ? kHeat
                                                                     : kLabelGrey);
        setText(ui_WifiNetwork, net::networkName());
        setText(ui_WifiAddress, net::addressText());

        // --- Setup > ABOUT -------------------------------------------
        //
        // The controller's version is kept live because it ARRIVES late: the
        // Teensy announces itself at its own boot, so a head that started
        // first learns it only when the link comes up, and a head reflashed
        // while the Teensy keeps running learns it from the Hello this board
        // sends at boot. Either way the page must not be stuck on whatever was
        // true when it was built.
        setText(ui_AboutControllerVersion, controllerVersionText());
        setColour(ui_AboutControllerVersion, live ? kValue : kDim);

        // Let an armed FORGET lapse rather than sit waiting indefinitely.
        if (forgetArmed && (millis() - forgetArmedAtMs) > kForgetArmedMs) {
            forgetArmed = false;
            showForgetState();
        }

        sampleChart(st, live);
    }

    void invalidate() {
        lv_obj_t* screen = lv_screen_active();

        if (screen != nullptr) {
            lv_obj_invalidate(screen);
        }
    }

    uint32_t benchmarkRedraw(const uint8_t iterations) {
        if (iterations == 0) {
            return 0;
        }

        const uint32_t start = micros();

        for (uint8_t i = 0; i < iterations; i++) {
            invalidate();

            // Drive the repaint to completion rather than waiting for LVGL's
            // timer, so one iteration really is one repaint. Without this the
            // loop would measure nothing but its own overhead.
            lv_refr_now(nullptr);
        }

        return (micros() - start) / iterations;
    }

} // namespace uibind
