# The hand-drawn LovyanGFX UI, kept for reference

This is the display head's original user interface, drawn directly with
LovyanGFX primitives. It ran the panel from the start of the port until
2026-11-13, when the UI moved to LVGL and this was taken out of the build.

**It is NOT compiled.** No environment in `platformio.ini` references this
folder, deliberately: code that is half maintained is worse than code that is
plainly frozen. It is here to be read.

| file | was |
|---|---|
| `display.cpp` | the whole UI: every page, every drawing call, and the touch-rectangle table |
| `display.h` | the interface as it stood then, including the parts only this UI used |

Last built as part of the firmware at commit `c90b82b`, 2026-11-08. The full
history is in git under `src-esp32s3/display.cpp` if a particular version is
ever wanted.

## What replaced it, and why

`src-esp32s3/uiLvgl.cpp` implements the same `display::` interface, so nothing
else in the head changed when it was swapped in. The screens live in
`uiScreens.cpp` and the behaviour in `uiBind.cpp`.

The decision was made on measurements, not taste. A full page repaint took
139 ms here against 101.5 ms on LVGL, because this code issues hundreds of
individual drawing calls, each setting up its own transfer to the panel, while
LVGL composes a rectangle in memory and sends it in one go.

The structural reason mattered more. In this file a button exists twice: once
where it is drawn, and again as a rectangle in a table that the touch handler
compares against. Those two had to be kept in step by hand, and when they
drifted a button either moved or stopped responding. An LVGL button knows its
own area, so that whole class of fault is gone, along with four failed attempts
at layout arithmetic recorded in `TEENSY-PORT-BRIEF.md`.

## Worth reading it for

* **The boiler chart**, built by hand in a PSRAM sprite with auto-scaling, a
  dashed setpoint line and real gaps where the sensor was unhealthy. It existed
  long before LVGL arrived, and a status note claiming otherwise was once
  argued from. The LVGL chart had to match it, not beat it.
* **The pixel-range comments** beside each page's `Hit` constants, which record
  what occupies which rows. Those were hard won.
* **`drawField`**, which clears downward from `y` while a raw `setCursor` with a
  GFX font places text by its baseline and so extends upward. That difference
  cost a session.

## What it never had

No ABOUT page and no screen-flip control; both arrived with the LVGL UI. Its
DISPLAY page did carry a sound volume control that the first LVGL version
dropped by accident, which is how this folder's existence proved useful on the
day it was created: the gap was found by comparing the two.
