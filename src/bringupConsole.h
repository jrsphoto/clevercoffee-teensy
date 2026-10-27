/**
 * @file bringupConsole.h
 *
 * @brief Serial bring-up console for testing one output at a time on the bench
 *
 * This is firmware-plan.md's "diagnostic firmware / test mode": a small command
 * interface over the USB serial port that fires each output on its own, so the
 * board can be validated subsystem by subsystem before any control logic exists.
 *
 * The non-negotiable rule from that plan is preserved here: this commands the
 * CONTROL side only. It can never bypass a physical safety, and on the real
 * machine the mains chain (and the inline TCO) is unchanged by anything typed in.
 *
 * Three interlocks, because this thing switches a ~1500 W element:
 *
 *  1. ONE CHANNEL AT A TIME. Turning on a second channel is refused while
 *     another is live. Matches "fire mains relays deliberately, one at a time",
 *     and means the load-shed rule cannot be violated from the console.
 *  2. AUTO-OFF. Any channel energized from here switches itself off after a few
 *     seconds unless the command is re-issued. You cannot walk away from the
 *     bench leaving the heater on.
 *  3. NO ENERGIZING WHILE FAULTED. A latched fault blocks every "on", and the
 *     only way past it is to deliberately clear the fault.
 *
 * Compiled into the Teensy firmware for now because the whole board is in
 * bring-up. Once control logic lands it should go behind a build flag so a
 * production image cannot be driven from a serial port.
 */

#pragma once

/**
 * @brief Print the banner and command list. Call once at the end of setup().
 */
void bringupConsoleBegin();

/**
 * @brief Service the console. Call every pass of the main loop.
 *
 * Non-blocking: reads whatever characters are waiting, acts only on a completed
 * line, and returns immediately otherwise. Also enforces the auto-off timer, so
 * this must be called even when nobody is typing.
 */
void bringupConsoleLoop();
