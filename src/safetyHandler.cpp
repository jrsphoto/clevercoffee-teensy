/**
 * @file safetyHandler.cpp
 *
 * @brief Hardware watchdog and reset-cause detection (Teensy 4.1 / i.MX RT1062)
 *
 * Split out of safetyHandler.h because this is the one genuinely chip-specific
 * part of the safety layer; everything else is plain pin writes.
 *
 * Driven straight at the chip's WDOG1 registers rather than through a watchdog
 * library. That is deliberate: no third-party dependency sits in the one path
 * that has to work when everything else has stopped working, every register bit
 * is visible here, and the register names come from the Teensy core's own
 * imxrt.h so they are not hand-copied magic numbers.
 *
 * Two-stage by design:
 *   1. ~3 s of silence  -> interrupt fires, actuators are forced off immediately.
 *   2. ~4 s of silence  -> the chip resets, landing in the safe state because the
 *                          board's bias resistors hold the outputs off while the
 *                          Teensy pins are floating inputs during reset.
 *
 * Stage 1 matters: it kills the heater about a second earlier than the reset
 * would, and it works even if the reset path itself is somehow delayed.
 *
 * WDOG1 cannot be disabled once enabled, except by a reset. That is the correct
 * behaviour here - there is no legitimate reason for this firmware to ever turn
 * its own watchdog off.
 */

#include "safetyHandler.h"

#if defined(ARDUINO_TEENSY41) || defined(__IMXRT1062__)

namespace {

    /**
     * WDOG1 timeout field. The timeout is (WT + 1) x 0.5 seconds, so WT=7 gives
     * 4.0 s. Long enough that a slow-but-legitimate loop pass (an SPI retry, a
     * config write) never trips it, short enough that a genuinely hung machine
     * does not sit with a live heater for long.
     */
    constexpr uint8_t kWatchdogTimeoutWt = 7; // (7 + 1) * 0.5 s = 4.0 s

    /**
     * Pre-reset interrupt, in half-seconds BEFORE the timeout above. 2 = fires
     * one second early, at roughly 3 s of silence.
     */
    constexpr uint8_t kWatchdogWarningHalfSeconds = 2;

    // WICR bits. The Teensy core header defines the WCR and WRSR bits but not
    // these, so they are spelled out here against the reference manual.
    constexpr uint16_t kWicrInterruptEnable = (uint16_t)(1u << 15); // WIE
    constexpr uint16_t kWicrInterruptStatus = (uint16_t)(1u << 14); // WTIS, write 1 to clear

    bool resetCauseRead = false;
    bool resetWasWatchdog = false;

    /**
     * The chip latches why it last reset in two places: SRC_SRSR (system-wide)
     * and WDOG1_WRSR (the watchdog's own view). Read both once, as early as
     * possible, then clear SRC_SRSR so the next boot reports its own cause
     * instead of inheriting ours.
     *
     * WRSR is read-only and is reloaded by hardware on each reset, so there is
     * nothing to clear there.
     */
    void readResetCauseOnce() {
        if (resetCauseRead) {
            return;
        }

        const uint32_t srsr = SRC_SRSR;
        const uint16_t wrsr = WDOG1_WRSR;

        resetWasWatchdog = ((srsr & (SRC_SRSR_WDOG_RST_B | SRC_SRSR_WDOG3_RST_B)) != 0) || ((wrsr & WDOG_WRSR_TOUT) != 0);

        SRC_SRSR = srsr; // write-1-to-clear
        resetCauseRead = true;
    }

    /**
     * Fires roughly one second before the watchdog resets the chip.
     *
     * The only job is to drop the actuators earlier than the reset would and
     * leave a breadcrumb. Deliberately does almost nothing else: by definition we
     * are already in a state that cannot be trusted, so this must not depend on
     * any other part of the firmware still working.
     */
    void onWatchdogWarning() {
        safetyHardOff();

        WDOG1_WICR |= kWicrInterruptStatus; // acknowledge

        // Serial here is best-effort. If it blocks or is not connected, the reset
        // arrives a second later regardless, which is the actual protection.
        Serial.println("WATCHDOG: control loop stalled - actuators forced off, reset imminent");
    }

} // namespace

void safetyWatchdogInit() {
    readResetCauseOnce();

    // WICR must be configured before the watchdog is enabled: its interrupt
    // fields are write-once after reset.
    WDOG1_WICR = kWicrInterruptEnable | kWicrInterruptStatus | (uint16_t)(kWatchdogWarningHalfSeconds & 0xFF);

    attachInterruptVector(IRQ_WDOG1, &onWatchdogWarning);
    NVIC_ENABLE_IRQ(IRQ_WDOG1);

    // WDE enables the watchdog and is sticky until reset - intended.
    // WDZST  : hold the counter in low-power modes, so sleeping is not a timeout.
    // WDBG   : hold the counter while halted in a debugger, so debugging does not
    //          look like a hang.
    // SRS/WDA: both must be written as 1. They are "assert this reset signal"
    //          controls that are active-low, so writing 0 would fire them.
    WDOG1_WCR = WDOG_WCR_WT(kWatchdogTimeoutWt) | WDOG_WCR_WDE | WDOG_WCR_WDZST | WDOG_WCR_WDBG | WDOG_WCR_SRS | WDOG_WCR_WDA;

    safetyWatchdogFeed();
}

void safetyWatchdogFeed() {
    // The service sequence is a fixed two-word magic write. Both halves must
    // land, in order, with no other write to WSR in between - so an interrupt
    // must not be able to land between them.
    //
    // Saves and restores the interrupt-enable state rather than unconditionally
    // re-enabling, so this stays correct if it is ever called from a context
    // that already had interrupts masked. This is the same idiom the Teensy core
    // uses itself (see EventResponder.h) - the CMSIS __get_PRIMASK helpers are
    // not reliably in scope from a plain Arduino.h include.
    uint32_t savedPrimask;
    __asm__ volatile("mrs %0, primask\n" : "=r"(savedPrimask)::);
    __disable_irq();

    WDOG1_WSR = 0x5555;
    WDOG1_WSR = 0xAAAA;

    if (savedPrimask == 0) {
        __enable_irq();
    }
}

bool safetyWasWatchdogReset() {
    readResetCauseOnce();

    return resetWasWatchdog;
}

#else

// Non-Teensy builds (the frozen ESP32 reference firmware) do not use this safety
// layer. These stubs exist only so the file is harmless if it is ever compiled
// outside the Teensy env.
void safetyWatchdogInit() {}

void safetyWatchdogFeed() {}

bool safetyWasWatchdogReset() {
    return false;
}

#endif
