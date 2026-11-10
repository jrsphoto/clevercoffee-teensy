/**
 * @file uiScreens.cpp
 *
 * @brief The screens, built in code. THIS IS THE UI.
 *
 * Written here rather than drawn in SquareLine Studio. That was a deliberate
 * reversal on 2026-11-10: the editor was adopted as an OPTION - something
 * available if a layout ever needs nudging - not as a job John is taking on.
 * Designing screens is firmware work and stays firmware work.
 *
 * SquareLine remains a live option and costs nothing to keep open, which is
 * the whole reason `uiContract.h` exists. An export declares exactly the
 * symbols this file defines, so if a screen is ever redrawn in the editor its
 * generated code lands in `ui/`, this file's definitions are dropped, and
 * NOTHING ELSE CHANGES - `uiBind.cpp` keeps binding the same widgets by the
 * same names and every rule it enforces survives untouched.
 *
 * WHAT BELONGS HERE AND WHAT DOES NOT
 *
 * Here: what exists, where it sits, what size and colour it is. Layout.
 *
 * Not here: what anything SAYS. No telemetry, no formatting, no decisions
 * about whether a reading is believable. All of that is `uiBind.cpp`, and the
 * separation is what makes the editor swap possible at all - a layout tool
 * cannot express "show a gap when the probe is dead", so that must not live
 * anywhere a layout tool might one day own.
 *
 * The layout mirrors the hand-drawn `Page::Main` it replaces, so the change of
 * framework could be judged without the design also moving underneath it.
 */

#include <lvgl.h>

#include "uiContract.h"

// Defined here only while no SquareLine export is present. When one is,
// ui/ui.c defines these instead and this whole file drops out.
#if UI_EXPORT_PRESENT == 0

extern "C" {

    lv_obj_t* ui_ScreenReadout = nullptr;
    lv_obj_t* ui_BoilerValue = nullptr;
    lv_obj_t* ui_SetpointValue = nullptr;
    lv_obj_t* ui_GroupValue = nullptr;
    lv_obj_t* ui_HeaterBar = nullptr;
    lv_obj_t* ui_HeaterPercent = nullptr;
    lv_obj_t* ui_WeightValue = nullptr;
    lv_obj_t* ui_WeightUnit = nullptr;
    lv_obj_t* ui_FlowValue = nullptr;
    lv_obj_t* ui_FlowSub = nullptr;
    lv_obj_t* ui_PumpValue = nullptr;
    lv_obj_t* ui_StatusLabel = nullptr;
    lv_obj_t* ui_BoilerChart = nullptr;

    lv_obj_t* ui_ScreenHome = nullptr;
    lv_obj_t* ui_HomeTiles[9] = {};
    lv_obj_t* ui_HomeTileSubs[9] = {};
    lv_obj_t* ui_HomeFooter = nullptr;
    lv_obj_t* ui_StopButton = nullptr;
    lv_obj_t* ui_ScreenSetup = nullptr;
    lv_obj_t* ui_ScreenDisplay = nullptr;
    lv_obj_t* ui_BrightnessValue = nullptr;
    lv_obj_t* ui_FlipValue = nullptr;
    lv_obj_t* ui_SetupBack = nullptr;
    lv_obj_t* ui_SetupDisplayBtn = nullptr;
    lv_obj_t* ui_SetupBrewBtn = nullptr;
    lv_obj_t* ui_SetupWifiBtn = nullptr;
    lv_obj_t* ui_DisplayBack = nullptr;
    lv_obj_t* ui_BrightnessDown = nullptr;
    lv_obj_t* ui_BrightnessUp = nullptr;
    lv_obj_t* ui_FlipButton = nullptr;
    lv_obj_t* ui_ScreenBrew = nullptr;
    lv_obj_t* ui_BrewBack = nullptr;
    lv_obj_t* ui_SetpointDown = nullptr;
    lv_obj_t* ui_SetpointUp = nullptr;
    lv_obj_t* ui_SetpointShown = nullptr;
    lv_obj_t* ui_HeaterToggle = nullptr;
    lv_obj_t* ui_HeaterToggleLbl = nullptr;
    lv_obj_t* ui_TareButton = nullptr;
    lv_obj_t* ui_ZeroVolButton = nullptr;
    lv_obj_t* ui_ClearFaultButton = nullptr;
    lv_obj_t* ui_BrewStatus = nullptr;
    lv_obj_t* ui_ScreenWifi = nullptr;
    lv_obj_t* ui_WifiBack = nullptr;
    lv_obj_t* ui_WifiState = nullptr;
    lv_obj_t* ui_WifiNetwork = nullptr;
    lv_obj_t* ui_WifiAddress = nullptr;
    lv_obj_t* ui_WifiSetupBtn = nullptr;
    lv_obj_t* ui_WifiForgetBtn = nullptr;
    lv_obj_t* ui_WifiForgetLbl = nullptr;

} // extern "C"

namespace {

    const lv_color_t kBg = lv_color_hex(0x000000);
    const lv_color_t kRule = lv_color_hex(0x3C3C3C);
    const lv_color_t kLabel = lv_color_hex(0x808080);
    const lv_color_t kValue = lv_color_hex(0xE0E0E0);

    lv_obj_t* caption(lv_obj_t* parent, const int x, const int y, const char* text) {
        lv_obj_t* l = lv_label_create(parent);

        lv_label_set_text(l, text);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(l, kLabel, 0);
        lv_obj_set_pos(l, x, y);

        return l;
    }

    lv_obj_t* value(lv_obj_t* parent, const int x, const int y, const int w,
                    const lv_font_t* font) {
        lv_obj_t* l = lv_label_create(parent);

        lv_label_set_text(l, "--.-");
        lv_obj_set_style_text_font(l, font, 0);
        lv_obj_set_style_text_color(l, kValue, 0);
        lv_obj_set_pos(l, x, y);
        lv_obj_set_width(l, w);

        return l;
    }

    void rule(lv_obj_t* parent, const int x, const int y, const int w, const int h) {
        lv_obj_t* r = lv_obj_create(parent);

        lv_obj_remove_style_all(r);
        lv_obj_set_pos(r, x, y);
        lv_obj_set_size(r, w, h);
        lv_obj_set_style_bg_color(r, kRule, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    }

} // namespace

namespace {

    // --- Home grid geometry, copied from the hand-drawn page --------------
    //
    // Same numbers deliberately, so the change of framework can be judged
    // without the layout also moving. 3 columns of 152 across 480, 3 rows of
    // 92 above a 26 px footer strip.
    constexpr int kGridX0 = 8;
    constexpr int kGridY0 = 4;
    constexpr int kTileW = 152;
    constexpr int kTileH = 92;
    constexpr int kTileGapX = 8;
    constexpr int kTileGapY = 6;
    constexpr int kFooterH = 26;
    constexpr int kScreenW = 480;
    constexpr int kScreenH = 320;

    struct HomeTile {
            const char* label;
            const char* sub;
            bool drink;
    };

    /**
     * The nine tiles, reading across. Brief 9.9.
     *
     * Every one is backed by something that works. Hot water and steam are
     * absent because they are manual valves on this machine and the board has
     * no output for either - a tile that cannot do anything is worse than a
     * missing tile.
     *
     * The sub-labels say what a shot STOPS on, never a bar figure: there is
     * no pressure sensor fitted, and labelling a tile "9 bar" with nothing
     * measuring pressure would be the UI inventing a number.
     */
    constexpr HomeTile kHomeTiles[9] = {
        {"RISTRETTO", "by weight", true},
        {"ESPRESSO", "by weight", true},
        {"LUNGO", "by volume", true},
        {"MANUAL", "STOP to end", false},
        {"FLUSH", "purge", false},
        {"BACKFLUSH", "tap to arm", false},
        {"SCALE", "tare", false},
        {"STATUS", "live readout", false},
        {"SETTINGS", "", false},
    };

    const lv_color_t kPanel = lv_color_hex(0x1C1C1C);
    const lv_color_t kAccent = lv_color_hex(0x00FFFF);
    const lv_color_t kAlarm = lv_color_hex(0xFF0000);

    /** @brief One tile: a button with a label and a sub-label. */
    void buildTile(lv_obj_t* parent, const int i) {
        const int col = i % 3;
        const int row = i / 3;
        const HomeTile& t = kHomeTiles[i];

        lv_obj_t* btn = lv_button_create(parent);

        lv_obj_set_pos(btn, kGridX0 + col * (kTileW + kTileGapX),
                       kGridY0 + row * (kTileH + kTileGapY));
        lv_obj_set_size(btn, kTileW, kTileH);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_set_style_bg_color(btn, kPanel, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(btn, t.drink ? kAccent : kValue, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);

        // A pressed tile dims rather than animating. The panel is a machine
        // control, and the feedback wanted is "it registered", not a flourish.
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x303030), LV_STATE_PRESSED);

        lv_obj_t* label = lv_label_create(btn);
        lv_label_set_text(label, t.label);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(label, t.drink ? kAccent : kValue, 0);
        lv_obj_align(label, t.sub[0] != '\0' ? LV_ALIGN_CENTER : LV_ALIGN_CENTER,
                     0, t.sub[0] != '\0' ? -10 : 0);

        lv_obj_t* sub = lv_label_create(btn);
        lv_label_set_text(sub, t.sub);
        lv_obj_set_style_text_font(sub, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(sub, kLabel, 0);
        lv_obj_align(sub, LV_ALIGN_CENTER, 0, 12);

        ui_HomeTiles[i] = btn;
        ui_HomeTileSubs[i] = sub;
    }

    void buildHomeScreen() {
        ui_ScreenHome = lv_obj_create(nullptr);

        lv_obj_set_style_bg_color(ui_ScreenHome, kBg, 0);
        lv_obj_set_style_bg_opa(ui_ScreenHome, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(ui_ScreenHome, 0, 0);
        lv_obj_set_style_border_width(ui_ScreenHome, 0, 0);
        lv_obj_remove_flag(ui_ScreenHome, LV_OBJ_FLAG_SCROLLABLE);

        for (int i = 0; i < 9; i++) {
            buildTile(ui_ScreenHome, i);
        }

        rule(ui_ScreenHome, 0, kScreenH - kFooterH, kScreenW, 1);

        // The footer strip. Boiler temperature and machine state on the one
        // page where none of the big readouts are visible - whatever page you
        // are on, the numbers that matter are still there.
        ui_HomeFooter = lv_label_create(ui_ScreenHome);
        lv_label_set_text(ui_HomeFooter, "NO LINK TO CONTROLLER");
        lv_obj_set_style_text_font(ui_HomeFooter, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(ui_HomeFooter, kLabel, 0);
        lv_obj_set_pos(ui_HomeFooter, 12, kScreenH - kFooterH + 5);
    }


    /** @brief A plain action button with a centred label. */
    lv_obj_t* actionButton(lv_obj_t* parent, const int x, const int y, const int w,
                           const int h, const char* text, const lv_color_t edge) {
        lv_obj_t* b = lv_button_create(parent);

        lv_obj_set_pos(b, x, y);
        lv_obj_set_size(b, w, h);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_set_style_bg_color(b, kPanel, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(b, edge, 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x303030), LV_STATE_PRESSED);

        lv_obj_t* l = lv_label_create(b);
        lv_label_set_text(l, text);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(l, kValue, 0);
        lv_obj_center(l);

        return b;
    }

    /** @brief A page header with its own BACK button. */
    lv_obj_t* pageHeader(lv_obj_t* parent, const char* title) {
        lv_obj_t* t = lv_label_create(parent);

        lv_label_set_text(t, title);
        lv_obj_set_style_text_font(t, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(t, kValue, 0);
        lv_obj_set_pos(t, 20, 8);

        rule(parent, 0, 32, kScreenW, 1);

        // BACK top right. These pages have room for one, unlike the readout,
        // where every pixel is spoken for and a tap anywhere goes back.
        return actionButton(parent, 370, 4, 100, 26, "BACK", kRule);
    }

    lv_obj_t* makeScreen() {
        lv_obj_t* scr = lv_obj_create(nullptr);

        lv_obj_set_style_bg_color(scr, kBg, 0);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(scr, 0, 0);
        lv_obj_set_style_border_width(scr, 0, 0);
        lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

        return scr;
    }

    /**
     * The setup MENU.
     *
     * Deliberately almost empty. Everything used to be crammed onto one page
     * and it looked it. Adding a group means adding a tile and a page, not
     * squeezing another row into space that does not exist.
     */
    void buildSetupScreen() {
        ui_ScreenSetup = makeScreen();

        ui_SetupBack = pageHeader(ui_ScreenSetup, "SETUP");

        ui_SetupDisplayBtn = actionButton(ui_ScreenSetup, 20, 60, 210, 84, "DISPLAY", kValue);
        ui_SetupBrewBtn = actionButton(ui_ScreenSetup, 250, 60, 210, 84, "BREW", kValue);
        ui_SetupWifiBtn = actionButton(ui_ScreenSetup, 20, 160, 210, 84, "WIFI", kValue);
    }

    /** The DISPLAY sub-page: brightness, and which way up the panel is. */
    void buildDisplayScreen() {
        ui_ScreenDisplay = makeScreen();

        ui_DisplayBack = pageHeader(ui_ScreenDisplay, "DISPLAY");

        caption(ui_ScreenDisplay, 20, 56, "BRIGHTNESS");

        ui_BrightnessValue = lv_label_create(ui_ScreenDisplay);
        lv_label_set_text(ui_BrightnessValue, "200");
        lv_obj_set_style_text_font(ui_BrightnessValue, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(ui_BrightnessValue, kValue, 0);
        lv_obj_set_pos(ui_BrightnessValue, 200, 76);

        ui_BrightnessDown = actionButton(ui_ScreenDisplay, 20, 72, 70, 44, "-", kValue);
        ui_BrightnessUp = actionButton(ui_ScreenDisplay, 100, 72, 70, 44, "+", kValue);

        rule(ui_ScreenDisplay, 0, 130, kScreenW, 1);

        // SCREEN ORIENTATION. John asked for this 2026-11-10: the panel can
        // end up mounted either way round in the machine, and a screen that
        // reads upside down is useless whatever else it does.
        //
        // It PERSISTS - see uiBind's flip handler. A flip that forgets itself
        // on every power cycle would have to be redone every time the machine
        // is switched on, which is worse than not having the button.
        caption(ui_ScreenDisplay, 20, 146, "ORIENTATION");

        ui_FlipValue = lv_label_create(ui_ScreenDisplay);
        lv_label_set_text(ui_FlipValue, "normal");
        lv_obj_set_style_text_font(ui_FlipValue, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(ui_FlipValue, kValue, 0);
        lv_obj_set_pos(ui_FlipValue, 200, 166);

        ui_FlipButton = actionButton(ui_ScreenDisplay, 20, 162, 160, 44, "FLIP 180", kAccent);
    }


    /** @brief The label inside a button made by actionButton(). */
    lv_obj_t* buttonLabel(lv_obj_t* button) {
        return lv_obj_get_child(button, 0);
    }

    /** A big value label, for a setting being adjusted. */
    lv_obj_t* settingValue(lv_obj_t* parent, const int x, const int y,
                           const char* text, const lv_color_t colour) {
        lv_obj_t* l = lv_label_create(parent);

        lv_label_set_text(l, text);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(l, colour, 0);
        lv_obj_set_pos(l, x, y);

        return l;
    }

    /**
     * Setup > BREW.
     *
     * CLEAR FAULT lives here, and it matters more than it looks: a latched
     * fault blocks the temperature loop entirely, and the Teensy latches one
     * on every power interruption. Without this button the only way out is a
     * serial cable.
     */
    void buildBrewScreen() {
        ui_ScreenBrew = makeScreen();

        ui_BrewBack = pageHeader(ui_ScreenBrew, "BREW");

        caption(ui_ScreenBrew, 20, 46, "BREW TEMPERATURE");

        ui_SetpointDown = actionButton(ui_ScreenBrew, 20, 64, 70, 44, "-", kAccent);
        ui_SetpointUp = actionButton(ui_ScreenBrew, 100, 64, 70, 44, "+", kAccent);
        ui_SetpointShown = settingValue(ui_ScreenBrew, 190, 76, "--.- C", kAccent);

        ui_HeaterToggle = actionButton(ui_ScreenBrew, 320, 64, 140, 44, "HEAT OFF", kRule);
        ui_HeaterToggleLbl = buttonLabel(ui_HeaterToggle);

        rule(ui_ScreenBrew, 0, 122, kScreenW, 1);

        ui_TareButton = actionButton(ui_ScreenBrew, 20, 138, 140, 44, "TARE", kRule);
        ui_ZeroVolButton = actionButton(ui_ScreenBrew, 170, 138, 140, 44, "ZERO VOL", kRule);
        ui_ClearFaultButton = actionButton(ui_ScreenBrew, 320, 138, 140, 44, "CLEAR", kAlarm);

        // What the boiler is actually doing, so the effect of a setpoint
        // change is visible without going back to the readout.
        ui_BrewStatus = lv_label_create(ui_ScreenBrew);
        lv_label_set_text(ui_BrewStatus, "--");
        lv_obj_set_style_text_font(ui_BrewStatus, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(ui_BrewStatus, kLabel, 0);
        lv_obj_set_pos(ui_BrewStatus, 20, 200);
    }

    /**
     * Setup > WIFI.
     *
     * The only setup page that works with the controller link down, because
     * everything on it is this board's own business.
     */
    void buildWifiScreen() {
        ui_ScreenWifi = makeScreen();

        ui_WifiBack = pageHeader(ui_ScreenWifi, "WIFI");

        caption(ui_ScreenWifi, 20, 44, "STATUS");
        ui_WifiState = settingValue(ui_ScreenWifi, 20, 60, "offline", kLabel);

        caption(ui_ScreenWifi, 20, 96, "NETWORK");
        ui_WifiNetwork = settingValue(ui_ScreenWifi, 20, 112, "-", kValue);

        caption(ui_ScreenWifi, 20, 148, "ADDRESS");
        ui_WifiAddress = settingValue(ui_ScreenWifi, 20, 164, "-", kAccent);

        ui_WifiSetupBtn = actionButton(ui_ScreenWifi, 20, 210, 200, 44, "SET UP WIFI", kAccent);

        // FORGET is two-tap, like BACKFLUSH and for the same reason: it drops
        // the stored network and reboots into the setup portal, which is a
        // long way back from a mis-tap on a machine across the room.
        ui_WifiForgetBtn = actionButton(ui_ScreenWifi, 240, 210, 200, 44, "FORGET", kAlarm);
        ui_WifiForgetLbl = buttonLabel(ui_WifiForgetBtn);
    }

} // namespace

void ui_init() {
    ui_ScreenReadout = lv_screen_active();

    lv_obj_set_style_bg_color(ui_ScreenReadout, kBg, 0);
    lv_obj_set_style_bg_opa(ui_ScreenReadout, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(ui_ScreenReadout, 0, 0);

    // A fixed instrument panel that can be dragged about by a wet thumb is
    // worse than one that cannot.
    lv_obj_remove_flag(ui_ScreenReadout, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* const scr = ui_ScreenReadout;

    // Header.
    lv_obj_t* title = lv_label_create(scr);
    lv_label_set_text(title, "ARGENTA SME");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, kValue, 0);
    lv_obj_set_pos(title, 20, 6);
    rule(scr, 0, 29, 480, 1);

    // Chart. Styling only - the series, the point count and the gap handling
    // belong to uiBind, because they are about what the data MEANS.
    ui_BoilerChart = lv_chart_create(scr);
    lv_obj_set_pos(ui_BoilerChart, 20, 34);
    lv_obj_set_size(ui_BoilerChart, 230, 84);
    lv_obj_set_style_bg_color(ui_BoilerChart, kBg, 0);
    lv_obj_set_style_bg_opa(ui_BoilerChart, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(ui_BoilerChart, kRule, 0);
    lv_obj_set_style_border_width(ui_BoilerChart, 1, 0);
    lv_obj_set_style_radius(ui_BoilerChart, 0, 0);
    lv_obj_set_style_pad_all(ui_BoilerChart, 0, 0);
    lv_obj_set_style_size(ui_BoilerChart, 0, 0, LV_PART_INDICATOR); // no dots
    lv_obj_set_style_line_width(ui_BoilerChart, 1, LV_PART_ITEMS);
    lv_chart_set_div_line_count(ui_BoilerChart, 3, 0);
    lv_obj_set_style_line_color(ui_BoilerChart, lv_color_hex(0x1C1C1C), LV_PART_MAIN);

    // Boiler, the number you actually watch.
    caption(scr, 20, 122, "BOILER");
    ui_BoilerValue = value(scr, 20, 134, 140, &lv_font_montserrat_48);

    lv_obj_t* unit = lv_label_create(scr);
    lv_label_set_text(unit, "C");
    lv_obj_set_style_text_font(unit, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(unit, kLabel, 0);
    lv_obj_set_pos(unit, 164, 162);

    caption(scr, 270, 40, "TARGET");
    ui_SetpointValue = value(scr, 270, 56, 100, &lv_font_montserrat_20);

    caption(scr, 270, 96, "GROUP");
    ui_GroupValue = value(scr, 270, 112, 100, &lv_font_montserrat_20);

    // Heater duty.
    caption(scr, 20, 192, "HEATER");

    ui_HeaterPercent = lv_label_create(scr);
    lv_label_set_text(ui_HeaterPercent, "0%");
    lv_obj_set_style_text_font(ui_HeaterPercent, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ui_HeaterPercent, kLabel, 0);
    lv_obj_set_pos(ui_HeaterPercent, 160, 192);

    ui_HeaterBar = lv_bar_create(scr);
    lv_obj_set_pos(ui_HeaterBar, 20, 208);
    lv_obj_set_size(ui_HeaterBar, 230, 6);
    lv_bar_set_range(ui_HeaterBar, 0, 100);
    lv_bar_set_value(ui_HeaterBar, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(ui_HeaterBar, 0, 0);
    lv_obj_set_style_bg_color(ui_HeaterBar, kBg, LV_PART_MAIN);
    lv_obj_set_style_border_color(ui_HeaterBar, kRule, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui_HeaterBar, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(ui_HeaterBar, 0, LV_PART_INDICATOR);

    rule(scr, 0, 220, 480, 1);

    // Shot row.
    caption(scr, 24, 224, "WEIGHT");
    ui_WeightValue = value(scr, 24, 240, 134, &lv_font_montserrat_20);
    ui_WeightUnit = caption(scr, 24, 272, "raw counts");

    caption(scr, 184, 224, "FLOW");
    ui_FlowValue = value(scr, 184, 240, 134, &lv_font_montserrat_20);
    ui_FlowSub = caption(scr, 184, 272, "ml/s   0 ml");

    caption(scr, 344, 224, "PUMP");
    ui_PumpValue = value(scr, 344, 240, 130, &lv_font_montserrat_20);

    rule(scr, 160, 222, 1, 70);
    rule(scr, 320, 222, 1, 70);

    // Status strip.
    rule(scr, 0, 294, 480, 1);

    ui_StatusLabel = lv_label_create(scr);
    lv_label_set_text(ui_StatusLabel, "NO LINK TO CONTROLLER");
    lv_obj_set_style_text_font(ui_StatusLabel, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ui_StatusLabel, kLabel, 0);
    lv_obj_set_pos(ui_StatusLabel, 20, 300);

    // STOP, top right, hidden until the machine is actually doing something.
    // It sits where the old page cleared all the way to x=470 and so looked
    // empty but was not - see the note in the hand-drawn drawMainValues.
    ui_StopButton = lv_button_create(scr);
    lv_obj_set_pos(ui_StopButton, 380, 40);
    lv_obj_set_size(ui_StopButton, 90, 56);
    lv_obj_set_style_radius(ui_StopButton, 8, 0);
    lv_obj_set_style_bg_color(ui_StopButton, lv_color_hex(0x6E0000), 0);
    lv_obj_set_style_border_color(ui_StopButton, lv_color_hex(0xFF0000), 0);
    lv_obj_set_style_border_width(ui_StopButton, 1, 0);
    lv_obj_set_style_shadow_width(ui_StopButton, 0, 0);
    lv_obj_add_flag(ui_StopButton, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* stopLabel = lv_label_create(ui_StopButton);
    lv_label_set_text(stopLabel, "STOP");
    lv_obj_set_style_text_font(stopLabel, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(stopLabel, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(stopLabel);

    buildHomeScreen();
    buildSetupScreen();
    buildDisplayScreen();
    buildBrewScreen();
    buildWifiScreen();

    // THE GRID IS THE ROOT, as it is in the hand-drawn build. The readout is
    // reached from the STATUS tile, and a shot started from a drink tile
    // switches to it automatically.
    lv_screen_load(ui_ScreenHome);
}

#endif // UI_EXPORT_PRESENT == 0
