/**
 * @file pumpHandler.h
 *
 * @brief Variable-speed pump: BOTH DRV8871 inputs driven, PWM on pin 6
 *
 * The pump is NOT a relay. It is a brushed DC motor behind John's DRV8871
 * driver board, and the whole point of it is proportional speed: pre-infusion
 * at low flow, a pressure ramp, a declining profile at the end of a shot. An
 * on/off pump cannot do any of that.
 *
 * The board was originally laid out with a "Pmode" jumper so pin 9 could go
 * either to ULN2003 channel 2 (plain on/off pump relay) or straight to the
 * driver. That jumper is being removed: this machine will only ever use a DC
 * pump, and the pump no longer appears in the relay channel list at all.
 * ULN channel 2 is now spare.
 *
 * WIRING
 *   Teensy pin 9 -> DRV8871 IN1       held HIGH while running
 *   Teensy pin 6 -> DRV8871 IN2       carries the 20 kHz PWM
 *   Teensy GND   -> DRV8871 GND       VM  -> motor supply
 *
 * **DO NOT GROUND IN2.** This header used to say to, and that instruction is
 * now actively dangerous: the firmware DRIVES pin 6 as an output and pulses
 * it high, so an external ground on IN2 would short a driven Teensy pin.
 * Corrected 2026-11-17 after an outside review caught it; the code changed on
 * 2026-11-16 and this comment did not.
 *
 * SLOW DECAY, and the old text argued against it on reasoning that the bench
 * disproved. IN1 high with PWM on IN2 alternates DRIVING and BRAKING, which
 * is what the DRV8871 datasheet asks for when the point is speed control.
 * Driving one pin and coasting gave so little low-speed torque that nothing
 * moved below about 56% duty, so pre-infusion at 30% did nothing at all.
 * After the change: a trickle at 10%, turning at 20%, running at 30%.
 *
 * THE DUTY WRITTEN TO IN2 IS INVERTED. It is the BRAKING fraction, so 0 on
 * IN2 is full speed. pumpHandler.cpp does that conversion; callers still ask
 * for a speed in the ordinary direction.
 *
 * THE ORDER THE TWO PINS CHANGE IN IS A SAFETY DETAIL, not tidiness. IN1 low
 * with IN2 high is REVERSE on this chip, so stopping takes IN2 down first and
 * starting raises IN1 first, and the pair never passes through the reverse
 * combination. safetyHardOff() does the same. Both low is coast, which is the
 * safe state and what the board's pulldowns give while the pins float at
 * reset.
 *
 * The DRV8871's minimum motor supply is 6.5 V, which matters for the 6-8 V test
 * pump: run it at 8 V, not 6, or the driver is below its own spec. Logic inputs
 * are valid from 1.5 V and tolerate up to 5.5 V, so the Teensy's 3.3 V drives
 * it directly with no level shifter.
 *
 * SAFETY
 *
 * A pump is the actuator most able to make a mess, and unlike the heater it does
 * damage quietly. Three rules, all enforced here rather than trusted to callers:
 *
 *   1. OFF AT BOOT, and off before the firmware runs at all. The board's
 *      pull-downs on PMP_IN1 and PMP_IN2 hold both inputs low from power-on,
 *      which is coast, and safetyHardOff() takes deliberate control as the
 *      first statement of setup(). (The net was called PMP_PWM when there was
 *      only one of them.)
 *
 *   2. NO SPEED WHILE FAULTED. setSpeed() refuses outright if a fault is
 *      latched, the same way outputs::set() refuses to energise a relay.
 *
 *   3. COMMAND STALENESS. This is the one that is easy to leave out and the one
 *      that matters most. A speed command EXPIRES: if nothing re-commands the
 *      pump within kCommandTimeoutMs, it stops itself. A pump left running
 *      because the control loop died or a state machine wedged is a flood, and
 *      a gear pump dead-heading against a closed valve is also a pump
 *      destroying itself. Requiring the speed to be continuously re-asserted
 *      means a stalled caller stops the pump instead of abandoning it on.
 *
 * THERE IS NO LOAD-SHED INTERLOCK, and that is deliberate. An earlier version
 * refused to run the pump while the heater was on, to keep a 2000 W element and
 * the pump off the same 15 A circuit. The machine is specified with a 1000 W
 * element (8.3 A) and a 24 V DC pump drawing well under an amp, so the two
 * together never approach a 120 V outlet's limit. John's decision, 2026-11-08,
 * restated 2026-11-09: remove it rather than ship a disabled mechanism. Do not
 * re-add it without a measured reason - a gate that cannot trip is a gate
 * nobody will notice breaking.
 *
 * safetyHardOff() stops a PWM-driven pump correctly, which is not obvious and
 * was verified rather than assumed: it calls pinMode on both bridge inputs, and on
 * Teensy 4 pinMode unconditionally re-points the pad mux at GPIO
 * (digital.c: `*(p->mux) = 5 | 0x10`), tearing down the FlexPWM routing. A bare
 * digitalWrite would NOT have been enough - the pad would still be driven by
 * the PWM peripheral and the GPIO register ignored. Do not "simplify" that
 * pinMode call away.
 */

#pragma once

#include <stdint.h>

namespace pump {

    /**
     * PWM frequency. Above the audible band, so the pump does not whine, and
     * far inside what the driver accepts (the DRV8871 takes up to 200 kHz).
     *
     * Not higher than it needs to be: switching losses in the driver rise with
     * frequency, and 20 kHz is already inaudible.
     */
    constexpr uint32_t kPwmFrequencyHz = 20000;

    /**
     * PWM resolution in bits. 12 bits gives 4096 steps, which at 20 kHz the
     * Teensy's FlexPWM can still produce exactly - the i.MX RT clocks it fast
     * enough that resolution and frequency do not fight here.
     */
    constexpr uint8_t kPwmResolutionBits = 12;

    /**
     * How long a speed command stays valid. See rule 4 above - this is a
     * deliberate expiry, not a timeout on an error.
     *
     * Long enough that a normal control loop re-asserting at any sane rate
     * never trips it, short enough that a wedged caller stops the pump before
     * it has delivered much.
     */
    constexpr uint32_t kCommandTimeoutMs = 1500;

    /**
     * Fastest permitted speed change, in percent per second.
     *
     * Ramping is not politeness. A gear pump slammed from zero to full against
     * a cold group puts a pressure spike through the puck and the plumbing; a
     * ramp also stops the control loop chasing its own step response. 400%/s
     * reaches full speed in a quarter second, which is quick enough to feel
     * instant and slow enough to be smooth.
     *
     * Stopping is NEVER ramped - see stop().
     */
    constexpr float kMaxRampPercentPerSec = 400.0f;

    /**
     * @brief Configure the PWM pin and leave the pump stopped.
     *
     * Call after safetyInitOutputs(). Leaves duty at zero, so the pin stays
     * low exactly as the hard-off left it.
     */
    void init();

    /**
     * @brief Service the ramp and the command expiry. Call every loop pass.
     *
     * MUST be called even when the pump is idle: this is what enforces the
     * staleness rule, so skipping it while "nothing is happening" would disable
     * the protection precisely when a wedged caller needs catching.
     */
    void loop();

    /**
     * @brief Request a pump speed, 0-100%.
     *
     * Advisory in the same sense as everything else here: asking is not
     * getting. Refused while a fault is latched.
     *
     * The command expires - see kCommandTimeoutMs. To hold the pump running,
     * keep calling this.
     *
     * @return false if refused. Zero is always accepted.
     */
    bool setSpeed(float percent);

    /** @brief Stop immediately, with no ramp. Always succeeds. */
    void stop();

    /** @brief The speed last asked for, 0-100. */
    float requestedSpeed();

    /** @brief The speed actually being applied after ramping, 0-100. */
    float actualSpeed();

    /** @brief Is the pump turning at all? */
    bool isRunning();

    /** @brief Why the last setSpeed() was refused, or "ok". */
    const char* lastRefusal();

    /** @brief Print pump state for the bring-up console. */
    void printStatus();

} // namespace pump
