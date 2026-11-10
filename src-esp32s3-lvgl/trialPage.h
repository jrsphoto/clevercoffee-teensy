/**
 * @file trialPage.h
 *
 * @brief The live readout and boiler chart, rebuilt on LVGL
 *
 * WHY THIS EXISTS
 *
 * The head's UI is ~2000 lines of hand-drawn LovyanGFX. LVGL is the obvious
 * alternative and the question "is it the better choice" was not answerable
 * from opinion, so this rebuilds ONE page that already exists hand-drawn -
 * `Page::Main` in `src-esp32s3/display.cpp` - to make the comparison concrete:
 * same panel, same data, same workload underneath, different drawing layer.
 *
 * It is a measuring instrument, not a feature. It lives in its own build env
 * (`esp32s3_lvgl`) and the shipping head firmware does not contain it.
 *
 * WHAT IT DELIBERATELY COPIES
 *
 * The layout, the palette and the rules about honesty in the original:
 *
 *   - An unhealthy or stale reading shows as "--.-", never as the last good
 *     value. Same rule the control firmware follows.
 *   - A chart sample taken while the sensor was unhealthy is a GAP, not a
 *     line through it. LVGL spells this `LV_CHART_POINT_NONE`, which skips
 *     the point - so the framework supports the rule rather than fighting it.
 *   - An uncalibrated scale is never labelled "grams".
 *   - The chart's Y axis always keeps the setpoint in view, because the
 *     distance to it is the thing being read.
 *
 * WHAT IT DOES NOT DO
 *
 * No touch actions. The original's STOP button and tile navigation are not
 * reproduced: this page answers a drawing question, and wiring commands back
 * to the Teensy from a trial build would give a throwaway UI the ability to
 * ask for a shot. Not worth it for a measurement.
 */

#pragma once

#include <stdint.h>

namespace trialpage {

    /**
     * @brief Build the page. Call once, after LVGL and the panel are up.
     */
    void build();

    /**
     * @brief Refresh the values from link telemetry.
     *
     * Cheap to call often: LVGL repaints only what changed, which is the
     * whole point of the comparison. Call it on every telemetry frame.
     */
    void update();

    /**
     * @brief Average microseconds for one full-page invalidate and redraw.
     *
     * The counterpart to `display::benchmarkRedraw()` in the hand-drawn build,
     * so the two numbers mean the same thing and can be put side by side.
     * Forces the whole screen dirty each pass rather than measuring the
     * change-detected path.
     */
    uint32_t benchmarkRedraw(uint8_t iterations);

    /**
     * @brief Average microseconds for one chart sample appended and drawn.
     *
     * The number that actually matters for a strip chart: what it costs to add
     * a point, not what a full repaint costs.
     */
    uint32_t benchmarkChartSample(uint8_t iterations);

} // namespace trialpage
