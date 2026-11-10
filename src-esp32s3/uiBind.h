/**
 * @file uiBind.h
 *
 * @brief Telemetry into the exported widgets, widget events back out
 *
 * The hand-written half of the UI. SquareLine owns where things are and what
 * colour they are; this owns what they SAY and what happens when they are
 * pressed.
 *
 * The split is not tidiness. SquareLine regenerates its export wholesale, so
 * anything living among those files is destroyed on the next export. Every
 * rule below is therefore here rather than in the editor, and a re-export
 * cannot lose any of it:
 *
 *   - A stale or faulty reading shows "--.-", never the last good value.
 *     Same rule the control firmware follows: a stale reading is NO reading.
 *   - A chart sample taken while the sensor was unhealthy is a GAP.
 *     LV_CHART_POINT_NONE is skipped when drawing, so a dead probe reads as
 *     a dead probe rather than as a flat plausible line.
 *   - An uncalibrated scale is never labelled "grams". It reports raw counts
 *     and says so, because a dose read off the wrong unit is exactly the kind
 *     of plausible wrong number this project refuses everywhere.
 *   - Anything the operator can press is a REQUEST. The head cannot drive an
 *     output; it asks and the Teensy decides. See brief 9.9.
 *
 * None of that is expressible in a layout editor, which is the real reason
 * the split falls where it does.
 */

#pragma once

#include <stdint.h>

namespace uibind {

    /**
     * @brief Attach to the exported widgets. Call once, after ui_init().
     *
     * Adds the chart's series and sets anything the editor cannot know, then
     * leaves the widgets alone until update().
     */
    void begin();

    /**
     * @brief Push the latest telemetry into the widgets.
     *
     * Cheap to call on every frame: LVGL repaints only what actually changed,
     * so setting a label to the text it already holds costs almost nothing.
     * That is the property the hand-drawn code had to implement by hand with
     * a remembered string per field.
     */
    void update();

    /**
     * @brief Force everything to repaint.
     *
     * For a controller restart: every value on screen came from the previous
     * instance of it.
     */
    void invalidate();

    /**
     * @brief Average microseconds for one full-screen repaint.
     *
     * Same meaning as the hand-drawn build's `display::benchmarkRedraw()`, so
     * the two can be compared directly. Measured rather than claimed.
     */
    uint32_t benchmarkRedraw(uint8_t iterations);

} // namespace uibind
