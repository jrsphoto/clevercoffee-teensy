#!/usr/bin/env bash
#
# Build and run the screen preview. Writes one image per screen into out/.
#
#   tools/uipreview/build.sh
#
# Nothing here touches the firmware or the boards. It compiles the head's own
# uiScreens.cpp against the head's own LVGL on this machine.
#
# THE VERSION CHECK BELOW IS THE POINT. A preview drawn by a different LVGL
# than the firmware links, or with a different lv_conf.h, would show a layout
# the panel does not produce - and it would look completely plausible. So both
# are taken from the firmware's own build and the version is asserted, not
# assumed.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"

LVGL="$REPO/.pio/libdeps/esp32s3/lvgl"
HEAD="$REPO/src-esp32s3"
WANT_VERSION="9.6.0"

if [ ! -d "$LVGL" ]; then
    echo "LVGL source not found at $LVGL" >&2
    echo "Build the head once first, which downloads it:" >&2
    echo "  /home/john/.platformio/penv/bin/pio run -e esp32s3" >&2
    exit 1
fi

GOT_VERSION="$(grep -m1 '"version"' "$LVGL/library.json" | sed 's/.*"version": *"\([^"]*\)".*/\1/')"

if [ "$GOT_VERSION" != "$WANT_VERSION" ]; then
    echo "LVGL is $GOT_VERSION but this preview expects $WANT_VERSION." >&2
    echo "The firmware's pin in platformio.ini moved. Update WANT_VERSION here" >&2
    echo "in the same commit, so the preview cannot draw with a different" >&2
    echo "version than the panel runs." >&2
    exit 1
fi

if [ ! -f "$HEAD/lv_conf.h" ]; then
    echo "no lv_conf.h in $HEAD - the preview must use the head's own." >&2
    exit 1
fi

# LVGL's compiled files and ours go in separate folders. They were in one
# folder at first and the linker picked up the UI twice, once by name and once
# through the wildcard.
OBJ="$HERE/build/obj"
LIBOBJ="$OBJ/lvgl"
mkdir -p "$LIBOBJ" "$HERE/out"

# This folder comes BEFORE the head's on purpose. Both hold an lv_conf.h, and
# the one here has to win: it includes the head's and raises only the memory
# pool, which a 64-bit machine needs. See the comment in that file.
FLAGS=(
    -O1 -g0 -w
    -DLV_CONF_INCLUDE_SIMPLE
    -I "$LVGL"
    -I "$HERE"
    -I "$HEAD"
)

echo "building LVGL $GOT_VERSION for this machine (first run takes a minute)"

# Compile each LVGL source once and keep the objects, so a change to the UI
# rebuilds in a second rather than recompiling the whole library.
find "$LVGL/src" -name '*.c' -print0 |
    xargs -0 -P "$(nproc)" -I{} bash -c '
        src="$1"; obj="$2"; shift 2
        out="$obj/$(echo "$src" | md5sum | cut -c1-16).o"
        if [ ! -f "$out" ] || [ "$src" -nt "$out" ]; then
            gcc "$@" -c "$src" -o "$out"
        fi
    ' _ {} "$LIBOBJ" "${FLAGS[@]}"

echo "building the UI and the preview"

g++ "${FLAGS[@]}" -std=gnu++17 -c "$HEAD/uiScreens.cpp" -o "$OBJ/uiScreens.o"
g++ "${FLAGS[@]}" -std=gnu++17 -c "$HERE/preview.cpp" -o "$OBJ/preview.o"

g++ "$OBJ/preview.o" "$OBJ/uiScreens.o" "$LIBOBJ"/*.o -o "$HERE/build/preview" -lm

cd "$HERE"
./build/preview

# PPM is what the renderer writes because it needs no image library at all.
# Convert to PNG if anything here can, since a PNG is what a person can
# actually open.
if python3 "$HERE/topng.py" out/*.ppm 2>/dev/null; then
    rm -f out/*.ppm
    echo "images in $HERE/out"
    ls -1 "$HERE/out"
else
    echo "images in $HERE/out (as .ppm - no PNG converter available)"
fi
