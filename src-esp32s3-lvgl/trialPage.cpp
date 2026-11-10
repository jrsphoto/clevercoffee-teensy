/**
 * @file trialPage.cpp
 *
 * @brief The live readout and boiler chart in LVGL. See trialPage.h for why.
 */

#include "trialPage.h"

#include <Arduino.h>
#include <lvgl.h>
#include <math.h>
#include <stdio.h>

#include "link.h"

namespace trialpage {

    namespace {

        // --- Palette, lifted from display.cpp so the two pages look alike ---
        //
        // The original stores RGB565 literals because that is what it pushes
        // at the panel. LVGL takes 24-bit and converts, so the same colours
        // are written here in hex that a person can read. These are the
        // RGB565 values expanded, not new choices.
        const lv_color_t kBg = lv_color_hex(0x000000);
        const lv_color_t kPanel = lv_color_hex(0x1C1C1C);
        const lv_color_t kRule = lv_color_hex(0x3C3C3C);
        const lv_color_t kLabel = lv_color_hex(0x808080);
        const lv_color_t kValue = lv_color_hex(0xE0E0E0);
        const lv_color_t kAccent = lv_color_hex(0x00FFFF); // cyan, the boiler
        const lv_color_t kReady = lv_color_hex(0x00FF00);
        const lv_color_t kHeat = lv_color_hex(0xFFA400);
        const lv_color_t kAlarm = lv_color_hex(0xFF0000);
        const lv_color_t kDim = lv_color_hex(0x404040);

        // --- Chart geometry, matching the hand-drawn one -------------------
        //
        // Same 230x84 box at the same place, so the comparison is not
        // confounded by one of them simply being bigger. One sample every two
        // seconds as the original does, giving a window of 230 x 2 s = 460 s,
        // about seven and a half minutes - a heat-up from cold with room to
        // spare.
        constexpr int kGraphX = 20;
        constexpr int kGraphY = 34;
        constexpr int kGraphW = 230;
        constexpr int kGraphH = 84;
        constexpr uint32_t kGraphIntervalMs = 2000;
        constexpr uint16_t kGraphPoints = kGraphW;

        lv_obj_t* screen = nullptr;

        lv_obj_t* boilerValue = nullptr;
        lv_obj_t* boilerUnit = nullptr;
        lv_obj_t* setpointValue = nullptr;
        lv_obj_t* groupValue = nullptr;
        lv_obj_t* heaterBar = nullptr;
        lv_obj_t* heaterPercent = nullptr;
        lv_obj_t* weightValue = nullptr;
        lv_obj_t* weightUnit = nullptr;
        lv_obj_t* flowValue = nullptr;
        lv_obj_t* flowSub = nullptr;
        lv_obj_t* pumpValue = nullptr;
        lv_obj_t* statusLabel = nullptr;

        lv_obj_t* chart = nullptr;
        lv_chart_series_t* boilerSeries = nullptr;
        lv_chart_series_t* setpointSeries = nullptr;

        uint32_t graphLastMs = 0;

        /**
         * @brief A caption in the style the original uses.
         *
         * Small, dim, upper case, positioned by its top-left corner.
         */
        lv_obj_t* makeCaption(const int x, const int y, const char* text) {
            lv_obj_t* l = lv_label_create(screen);

            lv_label_set_text(l, text);
            lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_color(l, kLabel, 0);
            lv_obj_set_pos(l, x, y);

            return l;
        }

        /**
         * @brief A value label.
         *
         * NOTE the absence of any "clear the old text first" handling, which
         * is most of what drawField() in the hand-drawn version does and the
         * source of its trap about `y` being the top and the font height being
         * cleared downward. An LVGL label owns its box and repaints it.
         */
        lv_obj_t* makeValue(const int x, const int y, const int w,
                            const lv_font_t* font, const lv_color_t colour) {
            lv_obj_t* l = lv_label_create(screen);

            lv_label_set_text(l, "--.-");
            lv_obj_set_style_text_font(l, font, 0);
            lv_obj_set_style_text_color(l, colour, 0);
            lv_obj_set_pos(l, x, y);
            lv_obj_set_width(l, w);

            return l;
        }

        /** @brief A horizontal rule. */
        void makeRule(const int x, const int y, const int w) {
            lv_obj_t* r = lv_obj_create(screen);

            lv_obj_remove_style_all(r);
            lv_obj_set_pos(r, x, y);
            lv_obj_set_size(r, w, 1);
            lv_obj_set_style_bg_color(r, kRule, 0);
            lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        }

        /** @brief A vertical rule. */
        void makeVRule(const int x, const int y, const int h) {
            lv_obj_t* r = lv_obj_create(screen);

            lv_obj_remove_style_all(r);
            lv_obj_set_pos(r, x, y);
            lv_obj_set_size(r, 1, h);
            lv_obj_set_style_bg_color(r, kRule, 0);
            lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        }

        void buildHeader() {
            lv_obj_t* title = lv_label_create(screen);

            lv_label_set_text(title, "ARGENTA SME");
            lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
            lv_obj_set_style_text_color(title, kValue, 0);
            lv_obj_set_pos(title, 20, 6);

            makeRule(0, 29, 480);
        }

        void buildChart() {
            chart = lv_chart_create(screen);

            lv_obj_set_pos(chart, kGraphX, kGraphY);
            lv_obj_set_size(chart, kGraphW, kGraphH);

            lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
            lv_chart_set_point_count(chart, kGraphPoints);

            // SHIFT, so appending a sample scrolls the trace left. This is the
            // behaviour the hand-drawn chart implements with a ring buffer and
            // a full sprite repaint.
            lv_chart_set_update_mode(chart, LV_CHART_UPDATE_MODE_SHIFT);

            // No dots on the trace - at one pixel per sample they would merge
            // into a thick smear, which is why the original draws line
            // segments only.
            lv_obj_set_style_size(chart, 0, 0, LV_PART_INDICATOR);
            lv_obj_set_style_line_width(chart, 1, LV_PART_ITEMS);

            lv_obj_set_style_bg_color(chart, kBg, 0);
            lv_obj_set_style_bg_opa(chart, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(chart, kRule, 0);
            lv_obj_set_style_border_width(chart, 1, 0);
            lv_obj_set_style_radius(chart, 0, 0);
            lv_obj_set_style_pad_all(chart, 0, 0);

            // Faint grid instead of the original's axis labels. Dividers are
            // one line of configuration here; in the hand-drawn version they
            // would be another loop and another set of pixel offsets.
            lv_chart_set_div_line_count(chart, 3, 0);
            lv_obj_set_style_line_color(chart, kPanel, LV_PART_MAIN);
            lv_obj_set_style_line_width(chart, 1, LV_PART_MAIN);

            // Two series: the reading, and a flat line at the setpoint. The
            // original draws the setpoint as a dashed rule so it never reads
            // as data; LVGL has no dash style on a series, so it is drawn dim
            // instead, which serves the same purpose.
            boilerSeries = lv_chart_add_series(chart, kAccent, LV_CHART_AXIS_PRIMARY_Y);
            setpointSeries = lv_chart_add_series(chart, kDim, LV_CHART_AXIS_PRIMARY_Y);

            // Start every point hidden. LV_CHART_POINT_NONE is skipped when
            // drawing, so an empty chart is empty rather than a flat line at
            // zero - and the same value is what a sample taken while the
            // sensor was unhealthy gets written as.
            for (uint16_t i = 0; i < kGraphPoints; i++) {
                lv_chart_set_value_by_id(chart, boilerSeries, i, LV_CHART_POINT_NONE);
                lv_chart_set_value_by_id(chart, setpointSeries, i, LV_CHART_POINT_NONE);
            }
        }

        void buildReadout() {
            // Boiler: the number you actually watch.
            makeCaption(20, 122, "BOILER");
            boilerValue = makeValue(20, 134, 140, &lv_font_montserrat_48, kAccent);

            boilerUnit = lv_label_create(screen);
            lv_label_set_text(boilerUnit, "C");
            lv_obj_set_style_text_font(boilerUnit, &lv_font_montserrat_20, 0);
            lv_obj_set_style_text_color(boilerUnit, kLabel, 0);
            lv_obj_set_pos(boilerUnit, 164, 162);

            // Right-hand column.
            makeCaption(270, 40, "TARGET");
            setpointValue = makeValue(270, 56, 100, &lv_font_montserrat_20, kValue);

            makeCaption(270, 96, "GROUP");
            groupValue = makeValue(270, 112, 100, &lv_font_montserrat_20, kValue);

            // Heater duty bar.
            makeCaption(20, 192, "HEATER");

            heaterPercent = lv_label_create(screen);
            lv_label_set_text(heaterPercent, "0%");
            lv_obj_set_style_text_font(heaterPercent, &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_color(heaterPercent, kDim, 0);
            lv_obj_set_pos(heaterPercent, 160, 192);

            heaterBar = lv_bar_create(screen);
            lv_obj_set_pos(heaterBar, 20, 208);
            lv_obj_set_size(heaterBar, 230, 6);
            lv_bar_set_range(heaterBar, 0, 100);
            lv_bar_set_value(heaterBar, 0, LV_ANIM_OFF);
            lv_obj_set_style_radius(heaterBar, 0, 0);
            lv_obj_set_style_bg_color(heaterBar, kBg, LV_PART_MAIN);
            lv_obj_set_style_border_color(heaterBar, kRule, LV_PART_MAIN);
            lv_obj_set_style_border_width(heaterBar, 1, LV_PART_MAIN);
            lv_obj_set_style_radius(heaterBar, 0, LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(heaterBar, kDim, LV_PART_INDICATOR);

            makeRule(0, 220, 480);

            // Shot row: weight, flow, pump.
            makeCaption(24, 224, "WEIGHT");
            weightValue = makeValue(24, 240, 134, &lv_font_montserrat_20, kDim);
            weightUnit = makeCaption(24, 272, "raw counts");

            makeCaption(184, 224, "FLOW");
            flowValue = makeValue(184, 240, 134, &lv_font_montserrat_20, kDim);
            flowSub = makeCaption(184, 272, "ml/s   0 ml");

            makeCaption(344, 224, "PUMP");
            pumpValue = makeValue(344, 240, 130, &lv_font_montserrat_20, kDim);

            makeVRule(160, 222, 70);
            makeVRule(320, 222, 70);

            // Status strip.
            makeRule(0, 294, 480);

            statusLabel = lv_label_create(screen);
            lv_label_set_text(statusLabel, "NO LINK TO CONTROLLER");
            lv_obj_set_style_text_font(statusLabel, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(statusLabel, kHeat, 0);
            lv_obj_set_pos(statusLabel, 20, 300);
        }

        /**
         * @brief The status line, copied verbatim in logic from display.cpp.
         *
         * Same precedence: no link, then fault, then sensor fault, then heater
         * off, then ready, then heating. Changing the order would change what
         * the machine appears to be doing, so it is not "tidied".
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
         * @brief Append one chart sample if the interval has elapsed.
         *
         * An unhealthy reading is written as LV_CHART_POINT_NONE, so the trace
         * breaks rather than running a straight line across the gap. Both
         * series are shifted together so the setpoint line stays aligned with
         * the data beneath it.
         */
        void sampleChart(const machinelink::State& st, const bool live) {
            const uint32_t now = millis();

            if ((now - graphLastMs) < kGraphIntervalMs) {
                return;
            }

            graphLastMs = now;

            const bool healthy = live && st.boilerOk;

            lv_chart_set_next_value(chart, boilerSeries,
                                    healthy ? static_cast<int32_t>(st.boilerC * 10.0f)
                                            : LV_CHART_POINT_NONE);

            lv_chart_set_next_value(chart, setpointSeries,
                                    (live && st.setpointC > 0.0f)
                                        ? static_cast<int32_t>(st.setpointC * 10.0f)
                                        : LV_CHART_POINT_NONE);

            // Rescale to the data, keeping the setpoint in view, with a floor
            // on the span so a steady trace is a flat line rather than a
            // magnified picture of sensor noise. Same rule as the hand-drawn
            // chart; the difference is that LVGL rescales every earlier point
            // for free, which is the reason the original has to repaint its
            // whole sprite on every sample.
            int32_t* ys = lv_chart_get_y_array(chart, boilerSeries);
            int32_t lo = INT32_MAX;
            int32_t hi = INT32_MIN;

            for (uint16_t i = 0; i < kGraphPoints; i++) {
                if (ys[i] == LV_CHART_POINT_NONE) {
                    continue;
                }

                if (ys[i] < lo) lo = ys[i];
                if (ys[i] > hi) hi = ys[i];
            }

            if (lo == INT32_MAX) {
                return; // nothing plotted yet
            }

            if (live && st.setpointC > 0.0f) {
                const auto sp = static_cast<int32_t>(st.setpointC * 10.0f);

                if (sp < lo) lo = sp;
                if (sp > hi) hi = sp;
            }

            if ((hi - lo) < 100) { // 10.0 C in tenths
                const int32_t mid = (hi + lo) / 2;
                lo = mid - 50;
                hi = mid + 50;
            }

            const int32_t pad = (hi - lo) / 10;

            lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, lo - pad, hi + pad);
        }

    } // namespace

    void build() {
        screen = lv_screen_active();

        lv_obj_set_style_bg_color(screen, kBg, 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(screen, 0, 0);

        // No scrolling. A fixed-size instrument panel that can be dragged
        // around by a wet thumb is worse than one that cannot.
        lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

        buildHeader();
        buildChart();
        buildReadout();
    }

    void update() {
        const auto& st = machinelink::state();
        const bool live = machinelink::isConnected();

        char buf[32];

        // Boiler. "--.-" when there is no believable reading, never the last
        // good value.
        const bool boilerGood = live && st.boilerOk;

        if (boilerGood) {
            snprintf(buf, sizeof(buf), "%.1f", st.boilerC);
        }
        else {
            snprintf(buf, sizeof(buf), "--.-");
        }

        lv_label_set_text(boilerValue, buf);
        lv_obj_set_style_text_color(boilerValue, boilerGood ? kAccent : kAlarm, 0);

        snprintf(buf, sizeof(buf), "%.1f", live ? st.setpointC : 0.0f);
        lv_label_set_text(setpointValue, buf);

        const bool groupGood = live && st.groupOk;

        if (groupGood) {
            snprintf(buf, sizeof(buf), "%.1f", st.groupC);
        }
        else {
            snprintf(buf, sizeof(buf), "--.-");
        }

        lv_label_set_text(groupValue, buf);
        lv_obj_set_style_text_color(groupValue, groupGood ? kValue : kDim, 0);

        // Heater: the bar is the duty, the colour says whether it is allowed
        // to be doing anything at all.
        const int duty = live ? static_cast<int>(st.heaterDuty) : 0;

        lv_bar_set_value(heaterBar, duty, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(heaterBar, st.heaterEnabled ? kHeat : kDim, LV_PART_INDICATOR);

        snprintf(buf, sizeof(buf), "%d%%", duty);
        lv_label_set_text(heaterPercent, buf);
        lv_obj_set_style_text_color(heaterPercent, st.heaterEnabled ? kHeat : kDim, 0);

        // Weight. An uncalibrated scale reports raw counts and must never be
        // labelled grams - a dose read off a wrong unit is the kind of
        // plausible wrong number this project refuses everywhere.
        const bool weightGood = live && st.weightOk;

        if (weightGood) {
            snprintf(buf, sizeof(buf), st.weightCalibrated ? "%.1f" : "%.0f", st.weightG);
        }
        else {
            snprintf(buf, sizeof(buf), "--");
        }

        lv_label_set_text(weightValue, buf);
        lv_obj_set_style_text_color(weightValue,
                                    weightGood ? (st.weightCalibrated ? kValue : kHeat) : kDim, 0);
        lv_label_set_text(weightUnit, st.weightCalibrated ? "grams" : "raw counts");

        snprintf(buf, sizeof(buf), "%.1f", live ? st.flowMlPerSec : 0.0f);
        lv_label_set_text(flowValue, buf);
        lv_obj_set_style_text_color(flowValue, live ? kValue : kDim, 0);

        snprintf(buf, sizeof(buf), "ml/s   %.0f ml", live ? st.volumeMl : 0.0f);
        lv_label_set_text(flowSub, buf);

        snprintf(buf, sizeof(buf), "%.0f%%", live ? st.pumpPercent : 0.0f);
        lv_label_set_text(pumpValue, buf);
        lv_obj_set_style_text_color(pumpValue,
                                    (live && st.pumpPercent > 0.0f) ? kReady : kDim, 0);

        lv_color_t statusColour = kDim;
        const char* text = statusText(st, live, &statusColour);

        lv_label_set_text(statusLabel, text);
        lv_obj_set_style_text_color(statusLabel, statusColour, 0);

        sampleChart(st, live);
    }

    uint32_t benchmarkRedraw(const uint8_t iterations) {
        const uint32_t start = micros();

        for (uint8_t i = 0; i < iterations; i++) {
            // Force the whole screen dirty, so this measures a full repaint
            // rather than LVGL's change-detected path. That is what makes the
            // number comparable to display::benchmarkRedraw() in the
            // hand-drawn build.
            lv_obj_invalidate(screen);

            // Drive the refresh to completion rather than waiting for the
            // timer, so one iteration really is one repaint.
            lv_refr_now(nullptr);
        }

        return (micros() - start) / iterations;
    }

    uint32_t benchmarkChartSample(const uint8_t iterations) {
        const uint32_t start = micros();

        for (uint8_t i = 0; i < iterations; i++) {
            // A plausible walk around 95 C, in tenths, so the Y range moves a
            // little and the measurement includes a rescale rather than only
            // the easy case.
            const auto v = static_cast<int32_t>(930 + (i % 40));

            lv_chart_set_next_value(chart, boilerSeries, v);
            lv_refr_now(nullptr);
        }

        return (micros() - start) / iterations;
    }

} // namespace trialpage
