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
         * The flow trace, on the readout's second chart.
         *
         * Half a second a sample across 120 points is a sixty second window,
         * which covers a long shot. The boiler chart's two seconds would be
         * far too coarse here - the interesting part of a shot is the first
         * fifteen seconds.
         *
         * Values are stored in TENTHS of a ml/s. lv_chart works in int32 and
         * a range of 0 to 20 ml/s has only 20 integer steps across 100
         * pixels, which would look like stairs.
         */
        constexpr uint32_t kFlowSampleMs = 500;
        constexpr uint16_t kFlowPoints = 120;
        constexpr int32_t kFlowTenths = 10;

        /** Never scale tighter than this, so a trickle is not magnified. */
        constexpr int32_t kFlowMinTop = 2 * kFlowTenths;

        lv_chart_series_t* flowSeries = nullptr;
        uint32_t lastFlowSampleMs = 0;

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

        /**
         * The last shot's result, held on the readout after it ends.
         *
         * The controller drops a finished shot to idle after three seconds
         * (shotHandler's kFinishedLingerMs), which took the numbers off the
         * screen before anyone had read them. John asked for them to stay up
         * until he leaves the page, 2026-11-16.
         *
         * Held HERE rather than in the controller because it is a display
         * decision and nothing in the control path cares. The controller is
         * free to go idle, arm the next shot, or do anything else; this is
         * just what the screen last had to say.
         *
         * Volume is the SHOT's own, worked out from a baseline taken when the
         * shot started, because telemetry carries the running total since the
         * last flow reset rather than a per-shot figure.
         */
        bool shotActive = false;
        bool extracting = false;
        bool holdingResult = false;
        float shotBaselineMl = 0.0f;
        uint32_t shotStartedMs = 0;

        float heldMl = 0.0f;
        uint32_t heldSeconds = 0;
        float heldWeight = 0.0f;
        bool heldWeightUsable = false;

        /**
         * Status words that mean COFFEE IS BEING MADE - the part of a shot
         * whose water and whose seconds belong to the drink.
         *
         * Deliberately excludes "coolflush" and "recovery". The controller
         * takes its own volume and time baselines in beginExtraction(), which
         * runs after both of those, so counting them here would have the
         * panel reporting a bigger shot than the machine thinks it poured.
         */
        bool statusIsExtracting(const char* status) {
            return strcmp(status, "brewing") == 0 || strcmp(status, "preinfuse") == 0
                   || strcmp(status, "soak") == 0;
        }

        /** Status words that mean a shot is in progress, flush stages included. */
        bool statusIsShot(const char* status) {
            return statusIsExtracting(status) || strcmp(status, "coolflush") == 0
                   || strcmp(status, "recovery") == 0;
        }

        /**
         * Status words that mean a sequence the STOP button can actually stop
         * is running.
         *
         * The controller's `stop=1` stops a shot, a manual flush OR a
         * backflush, whichever is going, so all three belong here.
         */
        bool statusIsSequence(const char* status) {
            return statusIsShot(status) || strcmp(status, "flushing") == 0
                   || strcmp(status, "backflush") == 0;
        }

        /** Called when the readout is left, and when a new shot starts. */
        void clearHeldResult() {
            holdingResult = false;
        }

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


        /**
         * @brief Append a flow sample and rescale.
         *
         * FROZEN while a finished shot is being held on screen, so the trace
         * you are looking at is the shot you just pulled rather than sixty
         * seconds of nothing sliding in behind it. It resumes when the result
         * is released - a new shot, or leaving the readout.
         *
         * No link means no reading, so the trace BREAKS rather than drawing a
         * line along zero. Same rule as the boiler chart and the control
         * firmware: a stale reading is not a reading.
         */
        void sampleFlowChart(const machinelink::State& st, const bool live) {
            if (ui_FlowChart == nullptr || flowSeries == nullptr || holdingResult) {
                return;
            }

            const uint32_t now = millis();

            if ((now - lastFlowSampleMs) < kFlowSampleMs) {
                return;
            }

            lastFlowSampleMs = now;

            lv_chart_set_next_value(ui_FlowChart, flowSeries,
                                    live ? static_cast<int32_t>(st.flowMlPerSec * kFlowTenths)
                                         : LV_CHART_POINT_NONE);

            const int32_t* ys = lv_chart_get_y_array(ui_FlowChart, flowSeries);
            int32_t hi = 0;

            for (uint16_t i = 0; i < kFlowPoints; i++) {
                if (ys[i] != LV_CHART_POINT_NONE && ys[i] > hi) {
                    hi = ys[i];
                }
            }

            if (hi < kFlowMinTop) {
                hi = kFlowMinTop;
            }

            // Always anchored at zero. Flow is a quantity, not a deviation,
            // and a chart that floats its baseline makes a steady trickle
            // look like a torrent.
            lv_chart_set_range(ui_FlowChart, LV_CHART_AXIS_PRIMARY_Y, 0, hi + (hi / 10));
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
            // Leaving the readout is what releases a held shot result. That is
            // the whole rule: it stays until you look away.
            clearHeldResult();

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
        constexpr char kPrefSleep[] = "slp";

        /**
         * Screen sleep, in minutes. 0 means never.
         *
         * The steps are coarse on purpose: this is a two-button control on a
         * panel, and nobody needs 7 minutes rather than 5.
         */
        constexpr uint8_t kSleepSteps[] = {0, 1, 2, 5, 10, 15, 30, 60};
        constexpr uint8_t kSleepStepCount = sizeof(kSleepSteps) / sizeof(kSleepSteps[0]);

        uint8_t sleepMinutes = 0;
        uint32_t lastActivityMs = 0;



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

        void showSleep() {
            if (ui_SleepValue == nullptr) {
                return;
            }

            if (sleepMinutes == 0) {
                lv_label_set_text(ui_SleepValue, "never");

                return;
            }

            char buf[12];
            snprintf(buf, sizeof(buf), "%u min", static_cast<unsigned>(sleepMinutes));
            lv_label_set_text(ui_SleepValue, buf);
        }

        void sleepClicked(lv_event_t* e) {
            const bool up = lv_event_get_user_data(e) != nullptr;

            uint8_t index = 0;

            for (uint8_t i = 0; i < kSleepStepCount; i++) {
                if (kSleepSteps[i] == sleepMinutes) {
                    index = i;
                    break;
                }
            }

            if (up && index + 1 < kSleepStepCount) {
                index++;
            }
            else if (!up && index > 0) {
                index--;
            }

            sleepMinutes = kSleepSteps[index];
            saveByte(kPrefSleep, sleepMinutes);
            showSleep();

            // Changing it is itself activity, so the screen does not go dark
            // a second after someone sets a one-minute timeout.
            lastActivityMs = millis();
        }

        /**
         * @brief Blank the screen when idle, and only when idle.
         *
         * NEVER sleeps while the machine is doing something. A dark panel
         * during a shot would hide the shot timer, the weight and the STOP
         * button, which is the opposite of useful. A latched fault keeps it
         * awake too - that is the one message nobody should have to find by
         * touching the screen.
         *
         * Waking is not handled here: the touch layer in uiLvgl does it, so
         * that the touch which wakes the screen cannot also press whatever is
         * underneath it.
         */
        void serviceSleep() {
            const auto& st = machinelink::state();
            const bool live = machinelink::isConnected();
            const uint32_t now = millis();

            // WAKING COUNTS AS ACTIVITY. The touch that wakes the screen is
            // deliberately swallowed by the touch layer, so LVGL never sees
            // it and its idle clock does not reset. Without this the screen
            // woke, the timer was still expired, and it went dark again about
            // a second later.
            static bool wasAsleep = false;
            const bool asleepNow = display::screenAsleep();

            if (wasAsleep && !asleepNow) {
                lastActivityMs = now;
            }

            wasAsleep = asleepNow;

            // "Busy" is the machine DOING something, which a boiler holding
            // temperature is not. The heater output cycles with the PID, so
            // counting "heating" as busy would reset this timer every time
            // the SSR came on and the screen would never sleep on a live
            // machine. Only a shot, a flush, a backflush, the pump turning,
            // or a fault keeps it awake.
            const bool busy = live
                              && (st.pumpPercent > 0.0f || strcmp(st.fault, "none") != 0
                                  || (strcmp(st.status, "idle") != 0
                                      && strcmp(st.status, "heating") != 0));

            if (busy || sleepMinutes == 0) {
                lastActivityMs = now;

                if (display::screenAsleep()) {
                    display::setScreenAsleep(false);
                }

                return;
            }

            if (display::screenAsleep()) {
                return;
            }

            if ((now - lastActivityMs) >= (static_cast<uint32_t>(sleepMinutes) * 60000UL)) {
                display::setScreenAsleep(true);
            }
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
         * BOTH WIFI BUTTONS ASK BEFORE THEY ACT. John's call, 2026-11-17.
         *
         * Both of these restart the board, so a mis-tap costs the web page for
         * a minute at best and the stored network at worst, and neither used
         * to say so. SET UP WIFI acted on a single tap with nothing in the way
         * at all; FORGET used the two-tap "TAP AGAIN" arming that BACKFLUSH
         * uses, which is fine for a tile you are standing in front of but does
         * not tell anyone WHAT is about to happen.
         *
         * A popup does, so the arming is gone from FORGET and both now put the
         * consequence on the screen in words.
         *
         * BUILT AT RUNTIME, which CLAUDE.md otherwise warns against. The rule
         * there is about controls placed on a SCREEN at coordinates SquareLine
         * does not know about, where a later layout change can land on top of
         * them. This is a modal on LVGL's top layer: it belongs to no screen,
         * it centres itself, and it is gone again a moment later, so there is
         * no layout for it to collide with and no export needed.
         */
        void confirmDialog(const char* title, const char* text, const char* confirmLabel,
                           const lv_event_cb_t onConfirm) {
            // Parent nullptr makes it modal on the top layer: the rest of the
            // page cannot be tapped while it is up, which is the whole point.
            lv_obj_t* box = lv_msgbox_create(nullptr);

            if (box == nullptr) {
                return;
            }

            lv_msgbox_add_title(box, title);
            lv_msgbox_add_text(box, text);
            lv_obj_set_width(box, 360);

            // CANCEL first and on the left, so the harmless choice is the one
            // under a thumb that has already started moving.
            lv_obj_t* cancel = lv_msgbox_add_footer_button(box, "CANCEL");
            lv_obj_t* confirm = lv_msgbox_add_footer_button(box, confirmLabel);

            lv_obj_add_event_cb(
                cancel,
                [](lv_event_t* e) {
                    lv_msgbox_close(static_cast<lv_obj_t*>(lv_event_get_user_data(e)));
                },
                LV_EVENT_CLICKED, box);

            lv_obj_add_event_cb(confirm, onConfirm, LV_EVENT_CLICKED, box);
        }

        /**
         * SET UP WIFI KEEPS THE STORED NETWORK. It is not a reset.
         *
         * This called forgetNetworkAndRestart() until 2026-11-17, so one tap
         * - no arming, no confirmation - threw away the saved credentials.
         * That is more destructive than the FORGET button beside it, and it is
         * the exact fault net.h's comment on requestPortalOnNextBoot() says had
         * already happened once. The safe function was written for this button
         * and then never wired to it.
         */
        void wifiSetupClicked(lv_event_t*) {
            confirmDialog("Set up WiFi?",
                          "The machine restarts into its setup screen.\n\n"
                          "Your saved network is kept - cancel the setup screen, "
                          "or leave it, and it rejoins on its own.",
                          "RESTART",
                          [](lv_event_t* e) {
                              lv_msgbox_close(static_cast<lv_obj_t*>(lv_event_get_user_data(e)));
                              net::requestPortalOnNextBoot();
                          });
        }

        void wifiForgetClicked(lv_event_t*) {
            confirmDialog("Forget this network?",
                          "The saved WiFi name and password are ERASED and the "
                          "machine restarts into its setup screen.\n\n"
                          "You will have to enter them again. Coffee is unaffected.",
                          "ERASE",
                          [](lv_event_t* e) {
                              lv_msgbox_close(static_cast<lv_obj_t*>(lv_event_get_user_data(e)));
                              net::forgetNetworkAndRestart();
                          });
        }

        void showBrewPage(lv_event_t*) {
            if (ui_ScreenBrew != nullptr) {
                lv_screen_load(ui_ScreenBrew);
            }
        }

        void showWifiPage(lv_event_t*) {
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
            showSleep();
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
            {ui_SleepDown, sleepClicked, nullptr},
            {ui_SleepUp, sleepClicked, reinterpret_cast<void*>(1)},
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

            // Default never. A machine that blanks its screen unasked, the
            // first time it is switched on, looks broken.
            sleepMinutes = prefs.getUChar(kPrefSleep, 0);
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
        showSleep();

        lastActivityMs = millis();

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

        if (ui_FlowChart != nullptr) {
            lv_chart_set_type(ui_FlowChart, LV_CHART_TYPE_LINE);
            lv_chart_set_point_count(ui_FlowChart, kFlowPoints);
            lv_chart_set_update_mode(ui_FlowChart, LV_CHART_UPDATE_MODE_SHIFT);

            // No dots - at 120 points across 175 pixels they merge into a
            // dotted band and hide the shape of the trace.
            lv_obj_set_style_size(ui_FlowChart, 0, 0, LV_PART_INDICATOR);

            flowSeries = lv_chart_add_series(ui_FlowChart, kTraceColour, LV_CHART_AXIS_PRIMARY_Y);

            for (uint16_t i = 0; i < kFlowPoints; i++) {
                lv_chart_set_value_by_id(ui_FlowChart, flowSeries, i, LV_CHART_POINT_NONE);
            }

            lastFlowSampleMs = millis();
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

        // --- Shot tracking, for the held result ----------------------
        const bool shotNow = live && statusIsShot(st.status);
        const bool extractingNow = live && statusIsExtracting(st.status);

        if (shotNow && !shotActive) {
            shotBaselineMl = st.volumeMl;
            shotStartedMs = millis();
            holdingResult = false;
        }
        else if (!shotNow && shotActive) {
            // Just finished. Capture what it delivered before the controller
            // goes idle and the live numbers move on.
            heldMl = st.volumeMl - shotBaselineMl;
            heldSeconds = (millis() - shotStartedMs) / 1000;
            heldWeight = st.weightG;
            heldWeightUsable = st.weightOk && st.weightCalibrated;
            holdingResult = true;
        }

        // RE-BASELINE THE MOMENT EXTRACTION ACTUALLY STARTS, so a cooling
        // flush's water and its recovery pause are not counted as espresso.
        //
        // The block above starts counting when the sequence does, which is
        // right for the cooling flush itself: something is happening and the
        // readout should show it rather than sit at zero. But the figure the
        // operator reads as "the shot" has to mean the same thing the
        // controller means, and shotHandler.cpp takes its own baselines in
        // beginExtraction(), after both of those stages. Without this the
        // panel would report a larger volume and a longer time than the
        // machine thinks it poured.
        //
        // With no cooling flush configured both blocks fire on the same pass
        // and write the same values, which costs nothing.
        if (extractingNow && !extracting) {
            shotBaselineMl = st.volumeMl;
            shotStartedMs = millis();
        }

        shotActive = shotNow;
        extracting = extractingNow;

        if (shotNow) {
            // During the shot, show what THIS shot has delivered and how long
            // it has been going, not the running total since the last reset.
            snprintf(buf, sizeof(buf), "%.0fml  %lus", st.volumeMl - shotBaselineMl,
                     static_cast<unsigned long>((millis() - shotStartedMs) / 1000));
        }
        else if (holdingResult) {
            // Kept short on purpose: this label starts at x=184 and the flow
            // chart's caption starts at 288, so there are 104 pixels. The
            // first version read "last: 35 ml in 59s" and ran under the chart.
            snprintf(buf, sizeof(buf), "last %.0fml %lus", heldMl,
                     static_cast<unsigned long>(heldSeconds));
        }
        else {
            snprintf(buf, sizeof(buf), "ml/s   %.0f ml", live ? st.volumeMl : 0.0f);
        }

        setText(ui_FlowSub, buf);
        setColour(ui_FlowSub, holdingResult && !shotNow ? kValue : kLabelGrey);

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
        // THE PUMP ALONE IS NOT THE TEST, and using it was a real fault.
        // Every sequence here has stages where the pump is deliberately
        // stopped while the sequence is very much still running: the soak
        // (pump::setSpeed(0.0)), the cooling flush's recovery pause
        // (allOff()), and the drain phase of every backflush cycle
        // (allOff()), which is five to twenty seconds each. Through all of
        // those the button vanished, so the machine looked finished moments
        // before it drove the pump again.
        //
        // Worse than invisible: the readout's background is clickable and
        // returns to the home grid, so a finger reaching for STOP as it
        // disappeared landed on the background and navigated away while the
        // sequence carried on. For a backflush that removed the only way to
        // stop it from the panel.
        //
        // So the status word decides, and the pump is kept as well - that
        // covers a bare pump run, which no sequence word describes.
        if (ui_StopButton != nullptr) {
            const bool running = live && (statusIsSequence(st.status) || st.pumpPercent > 0.0f);

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

        sampleChart(st, live);
        sampleFlowChart(st, live);

        // Any touch counts as activity. lv_display_get_inactive_time is
        // LVGL's own idle clock and already covers every widget, so there is
        // no need to hook each button.
        if (lv_display_get_inactive_time(nullptr) < 1000) {
            lastActivityMs = millis();
        }

        serviceSleep();
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
