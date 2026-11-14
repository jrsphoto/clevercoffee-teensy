/**
 * @file uiContract.h
 *
 * @brief The names the SquareLine export must provide. HAND-WRITTEN.
 *
 * THIS FILE IS THE AGREEMENT between the half of the UI that is drawn in
 * SquareLine Studio and the half that is written here. SquareLine regenerates
 * its export wholesale, so nothing hand-written may live among those files;
 * what survives a re-export is this contract and the code that uses it.
 *
 * HOW THE NAMES WORK
 *
 * SquareLine exports every widget as a global named `ui_<Name>`, where <Name>
 * is whatever is typed in the widget's Name box in the editor. So the list
 * below is not a suggestion - it is the set of names that must be used in the
 * editor, spelled exactly, or the firmware will not find the widget.
 *
 * Rename a widget in SquareLine and the build breaks at link time with an
 * undefined reference, which is the failure we want: loud, immediate, and
 * pointing at the name that moved. The alternative - looking widgets up by
 * string at runtime - would fail silently on a live machine.
 *
 * WHAT IS DELIBERATELY NOT HERE
 *
 * No layout, no colours, no fonts. Those belong in the editor and are John's
 * to change without touching firmware. This file knows only that a label
 * exists and what goes in it.
 *
 * TODAY THERE IS NO EXPORT, AND THAT IS THE NORMAL STATE
 *
 * `uiScreens.cpp` defines these symbols and builds the screens in code. That
 * is the UI - not a stand-in waiting to be replaced. The editor was adopted on
 * 2026-11-10 as an OPTION for nudging a layout later, not as how the screens
 * get made.
 *
 * So this file is insurance, and cheap insurance: if a screen is ever redrawn
 * in SquareLine, its export lands in `ui/`, the `#if` below notices, and
 * `uiScreens.cpp` drops out of the build. `uiBind.cpp` goes on binding the
 * same widgets by the same names, and every rule it enforces survives
 * untouched. Keeping that door open costs one header.
 */

#pragma once

#include <lvgl.h>

#if __has_include("ui/ui.h")
// The SquareLine export is present. It declares every `ui_*` symbol itself,
// so the hand-written declarations below must not also be made.
#include "ui/ui.h"
#define UI_EXPORT_PRESENT 1
#else
#define UI_EXPORT_PRESENT 0

extern "C" {

    /**
     * @brief Build every screen. SquareLine's own entry point.
     *
     * Called once, after LVGL is up. The placeholder provides the same
     * function so the two are interchangeable.
     */
    void ui_init();

    // --- The readout screen -------------------------------------------
    //
    // In SquareLine: one screen named `ScreenReadout`, 480x320, and the
    // widgets below on it. The name in the editor's Name box becomes the
    // `ui_` symbol here.

    extern lv_obj_t* ui_ScreenReadout;

    /** Label. The big boiler temperature. "--.-" when there is no reading. */
    extern lv_obj_t* ui_BoilerValue;

    /** Label. The boiler setpoint. */
    extern lv_obj_t* ui_SetpointValue;

    /** Label. Group head temperature, or "--.-". */
    extern lv_obj_t* ui_GroupValue;

    /** Bar, range 0-100. Heater duty. */
    extern lv_obj_t* ui_HeaterBar;

    /** Label. Heater duty as a percentage, beside the bar. */
    extern lv_obj_t* ui_HeaterPercent;

    /** Label. Brew weight, or "--". */
    extern lv_obj_t* ui_WeightValue;

    /**
     * Label. The weight's UNIT, and it is a real safety-ish detail rather
     * than decoration: it reads "grams" only when the scale is calibrated,
     * and "raw counts" when it is not. Never hard-code "g" in the editor.
     */
    extern lv_obj_t* ui_WeightUnit;

    /** Label. Flow rate in ml/s. */
    extern lv_obj_t* ui_FlowValue;

    /** Label. The sub-line under flow: units and delivered volume. */
    extern lv_obj_t* ui_FlowSub;

    /** Label. Pump speed as a percentage. */
    extern lv_obj_t* ui_PumpValue;

    /** Label. The status strip: READY, HEATING, a fault, or no link. */
    extern lv_obj_t* ui_StatusLabel;

    // --- The home screen -----------------------------------------------
    //
    // The 3x3 tile grid (brief 9.9). Nine buttons and a footer strip, and
    // the tiles are plain LVGL Buttons - which is most of why this page is
    // worth moving: the hand-drawn version kept a table of touch rectangles
    // that had to be held in step with the drawing by hand, two sources of
    // truth for one button. An LVGL button knows its own area.

    extern lv_obj_t* ui_ScreenHome;

    /**
     * The nine tiles, in grid order reading across:
     *   RISTRETTO  ESPRESSO  LUNGO
     *   MANUAL     FLUSH     BACKFLUSH
     *   SCALE      STATUS    SETTINGS
     *
     * An array rather than nine names because the firmware iterates them and
     * attaches one handler with the index as its argument.
     */
    extern lv_obj_t* ui_HomeTiles[9];

    /** The sub-label inside each tile. BACKFLUSH's changes to say ARMED. */
    extern lv_obj_t* ui_HomeTileSubs[9];

    /** Label. The footer strip: boiler temperature and machine state. */
    extern lv_obj_t* ui_HomeFooter;

    /**
     * Button on the readout, shown only while the machine is doing something.
     *
     * The way out of MANUAL and of a running shot. Hidden when idle, because
     * a STOP that does nothing teaches people the button does nothing.
     */
    extern lv_obj_t* ui_StopButton;

    // --- Setup, a MENU of sub-pages ---------------------------------------
    //
    // A MENU, not one page carrying every control. That structure is the fix
    // that worked after four failed attempts at cramming controls onto a
    // single page (see the layout notes in CLAUDE.md). If something new needs
    // a control, ADD A SUB-PAGE - do not add a row here.

    extern lv_obj_t* ui_ScreenSetup;
    extern lv_obj_t* ui_ScreenDisplay;

    /** Label on the Display page showing the current brightness. */
    extern lv_obj_t* ui_BrightnessValue;

    /** Label on the Display page saying which way up the panel is. */
    extern lv_obj_t* ui_FlipValue;

    /** Label on the Display page showing the sound volume, 0-100%. */
    extern lv_obj_t* ui_VolumeValue;

    /** Navigation and action buttons on the setup pages. */
    extern lv_obj_t* ui_SetupBack;
    extern lv_obj_t* ui_SetupDisplayBtn;
    extern lv_obj_t* ui_SetupBrewBtn;
    extern lv_obj_t* ui_SetupWifiBtn;
    extern lv_obj_t* ui_SetupAboutBtn;
    extern lv_obj_t* ui_DisplayBack;
    extern lv_obj_t* ui_BrightnessDown;
    extern lv_obj_t* ui_BrightnessUp;
    extern lv_obj_t* ui_VolumeDown;
    extern lv_obj_t* ui_VolumeUp;

    /** FLIP 180. Rotates the panel and REMEMBERS it across a power cycle. */
    extern lv_obj_t* ui_FlipButton;

    // --- Setup > BREW ----------------------------------------------------
    extern lv_obj_t* ui_ScreenBrew;
    extern lv_obj_t* ui_BrewBack;
    extern lv_obj_t* ui_SetpointDown;
    extern lv_obj_t* ui_SetpointUp;
    extern lv_obj_t* ui_SetpointShown;   ///< Label, the target in C
    extern lv_obj_t* ui_HeaterToggle;    ///< Button, HEAT ON / HEAT OFF
    extern lv_obj_t* ui_HeaterToggleLbl; ///< its label, so the text can change
    extern lv_obj_t* ui_TareButton;
    extern lv_obj_t* ui_ZeroVolButton;
    extern lv_obj_t* ui_ClearFaultButton;
    extern lv_obj_t* ui_BrewStatus;      ///< Label, what the boiler is doing

    // --- Setup > WIFI ----------------------------------------------------
    extern lv_obj_t* ui_ScreenWifi;
    extern lv_obj_t* ui_WifiBack;
    extern lv_obj_t* ui_WifiState;    ///< Label, online / portal / offline
    extern lv_obj_t* ui_WifiNetwork;  ///< Label, the network name
    extern lv_obj_t* ui_WifiAddress;  ///< Label, the IP address
    extern lv_obj_t* ui_WifiSetupBtn;
    extern lv_obj_t* ui_WifiForgetBtn;
    extern lv_obj_t* ui_WifiForgetLbl; ///< its label: FORGET / TAP AGAIN

    // --- Setup > ABOUT ---------------------------------------------------
    //
    // What this machine is running, on the PANEL. The web UI has the same
    // card, but the panel is the only place to read it when the head is not
    // on a network - which is exactly when somebody is standing at the
    // machine wondering why.
    //
    // Product name, both boards and the repository are static text and so
    // belong to layout, not here. Only the two version labels are bound,
    // because only they can change while the machine is running.

    extern lv_obj_t* ui_ScreenAbout;
    extern lv_obj_t* ui_AboutBack;

    /** Label. This board's firmware version, compiled in from AUTO_VERSION. */
    extern lv_obj_t* ui_AboutHeadVersion;

    /**
     * Label. The CONTROLLER's firmware version.
     *
     * This board cannot read the Teensy's flash, so this is whatever the
     * controller announced over the link. When it has not announced, the
     * label says WHY rather than printing a dash - "no link" and "has not
     * announced since this board started" are different situations and only
     * one of them is a fault.
     */
    extern lv_obj_t* ui_AboutControllerVersion;

    /**
     * Chart, LINE type, one or two series.
     *
     * In SquareLine: set the point count to 230 and leave the data alone -
     * the firmware fills it. Series are added by the firmware too, because
     * their colours and the gap handling are tied to what the data means.
     */
    extern lv_obj_t* ui_BoilerChart;

} // extern "C"
#endif
