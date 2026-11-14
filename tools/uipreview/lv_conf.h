/**
 * @file lv_conf.h
 *
 * @brief The head's LVGL settings, with ONE value changed for this machine.
 *
 * The preview has to draw with the same settings as the panel or it is not
 * showing you the panel. So this does not copy anything: it includes the
 * head's own `lv_conf.h` and changes a single number.
 *
 * THE ONE DIFFERENCE: the memory pool.
 *
 * LVGL allocates widgets out of a fixed pool, 64 KB on the ESP32-S3. A PC is
 * 64-bit, so every pointer inside a widget is 8 bytes where the ESP32's is 4,
 * and the same seven screens do not fit. The first version of this tool
 * crashed for exactly that reason, and LVGL said so once its log was given
 * somewhere to print:
 *
 *   lv_realloc: couldn't reallocate memory
 *   circ_calc_aa4: Failed to allocate memory for circle points (radius 8)
 *
 * 8 MB here, which is nothing on a PC. This changes how much room the widgets
 * have, not how they are drawn, so the images are unaffected.
 *
 * It does mean the preview CANNOT tell you the panel is running out of
 * memory. Check that on the hardware: the head prints its free heap at boot,
 * and `/api/net` reports it live.
 */

#pragma once

#include "../../src-esp32s3/lv_conf.h"

#undef LV_MEM_SIZE
#define LV_MEM_SIZE (8 * 1024 * 1024)
