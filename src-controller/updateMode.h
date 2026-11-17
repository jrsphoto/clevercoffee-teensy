/**
 * @file updateMode.h
 *
 * @brief Groundwork for flashing the Teensy over the link from the ESP32-S3
 *
 * NOT A FLASHER YET. This is deliberately the safety half of the feature, built
 * first and on its own: the gating, the mode entry, and the guarantee that a
 * machine being updated is provably not controlling anything. The actual
 * receive-and-write step (FlasherX) comes later and drops into runLoop().
 *
 * Doing it in this order is the point. Retrofitting safety onto a working
 * flasher means testing it by nearly flashing a live machine; building the gate
 * first means the dangerous path can be exercised now, while nothing downstream
 * can actually write flash.
 *
 * THE PROBLEM THIS SOLVES
 *
 * The runtime watchdog cannot be switched off - WDOG1's enable bit is sticky
 * until reset, deliberately, so firmware can never disable its own safety net.
 * But flash erase/write blocks for long stretches with interrupts disabled and
 * would trip a 4 second watchdog mid-write.
 *
 * So rather than weaken the watchdog, the Teensy REBOOTS into a dedicated update
 * mode:
 *
 *   1. A request arrives (console or link) and is checked against isSafe().
 *   2. The flag is stored and the chip resets.
 *   3. That boot sees the flag and takes a different path: outputs forced safe,
 *      NO watchdog armed, no sensors, no control loop - only the flash receiver.
 *   4. When it finishes (or is power-cycled) it boots normally, watchdog armed.
 *
 * The runtime watchdog stays absolute, and a machine in update mode is provably
 * not controlling anything, because the code that controls things was never
 * started.
 *
 * WHERE THE FLAG LIVES
 *
 * A DMAMEM variable, in RAM2. Teensy 4's startup deliberately does not zero that
 * region (the linker marks .bss.dma NOLOAD), so it survives a software reset but
 * NOT a power cycle - exactly right: the reboot carries the request across, and
 * pulling the plug always returns the machine to normal firmware. A flag in
 * EEPROM could strand it in update mode permanently.
 *
 * RAM2 IS CACHED, which matters more than it looks. The write must be flushed
 * with arm_dcache_flush_delete() before the reset or it never reaches real RAM,
 * and the read at boot must invalidate the line first. Without the flush the
 * flag verified correctly before the reset - because the read came out of the
 * same cache - and was gone afterwards. The variable is 32-byte aligned and a
 * whole cache line is flushed, so nothing shares a line with it.
 *
 * SNVS_LPGPR0 was tried first and is the textbook answer, but it is locked on
 * this Teensy: writing it reads back zero. The write is verified rather than
 * trusted, which is the only reason that was caught instead of silently
 * rebooting back into normal firmware.
 *
 * It is also read-and-cleared in one go, so a crash during update mode cannot
 * produce a boot loop that stays in update mode.
 */

#pragma once

#include <stdint.h>

namespace updateMode {

    /**
     * @brief Is the machine in a state where a firmware update is allowed?
     *
     * The bar is simply that nothing is energised. Every actuator must be
     * commanded off, because the control loop stops during an update and
     * anything left on would stay on, unsupervised, for the duration.
     *
     * Note what is deliberately NOT a blocker: a latched fault. You may well be
     * updating precisely to fix the fault, and refusing on that basis would be
     * both annoying and circular. A fault already forces every output off, which
     * is the condition that actually matters.
     *
     * @param reason set to a short static explanation when the answer is false
     * @return true if an update may proceed
     */
    bool isSafe(const char** reason);

    /**
     * @brief Request an update and reboot into update mode.
     *
     * Checks isSafe() first and refuses otherwise. Does not return on success.
     *
     * @param reason set to a short static explanation when refused
     * @return false if refused; never returns if accepted
     */
    bool requestAndReboot(const char** reason);

    /**
     * @brief Did we boot because an update was requested? READS AND CLEARS.
     *
     * Call once, early in setup(), before anything is initialised. The
     * read-and-clear is what prevents a crash in update mode from becoming a
     * boot loop that never leaves it.
     */
    bool consumeRequest();

    /**
     * @brief The update-mode main loop. Does not return.
     *
     * Holds every actuator off and waits. The flash receiver lands here.
     */
    [[noreturn]] void runLoop();

} // namespace updateMode
