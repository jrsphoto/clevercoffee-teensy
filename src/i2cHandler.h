/**
 * @file i2cHandler.h
 *
 * @brief The shared I2C bus: recovery, startup, and an address scan
 *
 * One bus (Teensy Wire, SDA 18 / SCL 19) carries the Duppa encoder and the brew
 * pressure sensor. The PCF8574 button expander that the ESP32 board used has
 * been dropped - the Teensy has pins to spare, so the dose buttons go straight
 * onto GPIO instead of through an expander.
 *
 * The scan exists because on the previous board the encoder's address was NOT
 * what the datasheet's jumper arithmetic predicted: 0x66 against a predicted
 * 0x61. Scanning first and believing the result is faster than trusting a
 * calculation, and it immediately separates "the device is wired wrong" from
 * "the device is at a different address than expected" - two faults that look
 * identical from inside a driver.
 */

#pragma once

#include <stdint.h>

namespace i2c {

    /**
     * Bus speed. 100 kHz standard mode, deliberately not 400 kHz: this bus runs
     * to connectors and flying leads inside a machine full of pump and solenoid
     * noise, and nothing on it is fast enough to care.
     */
    constexpr uint32_t kBusHz = 100000;

    /** Expected addresses, used to name devices in the scan output. */
    constexpr uint8_t kAddrDuppaEncoder = 0x66; // confirmed by live scan on the ESP32 board
    constexpr uint8_t kAddrPressure = 0x28;     // Honeywell ABP2

    /**
     * @brief Recover a wedged bus, then start it.
     *
     * Bus recovery must happen BEFORE Wire.begin() claims the pins: if a device
     * was mid-transaction when the Teensy last reset, it can still be holding
     * SDA low, and no amount of starting the peripheral will shift it. Manually
     * clocking SCL with SDA released walks it out of that state. Cheap, harmless
     * when not needed, and saves a genuinely confusing debugging session.
     */
    void init();

    /**
     * @brief Scan the bus and print every device found, named where known.
     */
    void scan();

    /**
     * @brief Report the electrical state of SDA and SCL before talking to anyone.
     *
     * A scan that finds nothing cannot tell you why. Reading the two lines with
     * and without the Teensy's internal pull-ups separates the three common
     * causes: no pull-ups on the bus at all, a line being held low by a wedged
     * or miswired device, and a bus that is electrically fine (so the fault is
     * addressing or wiring order instead).
     */
    void checkLines();

    /**
     * @brief Probe the addresses we expect, reporting WHY each one fails.
     *
     * A sweep only says present or absent. The error code distinguishes "nothing
     * answered" from "the bus itself is broken", which point at very different
     * faults.
     */
    void probeExpected();

    /**
     * @brief Bit-banged scan with the SDA/SCL roles given explicitly.
     *
     * The hardware peripheral can only drive SDA on 18 and SCL on 19. Doing it
     * in software means the roles can be reversed, which tests for crossed wires
     * from this end instead of asking someone to re-inspect a loom they have
     * already checked. If devices answer with the roles swapped, the wires are
     * swapped - no interpretation required.
     */
    void scanBitBang(uint8_t sdaPin, uint8_t sclPin, const char* label);

    /** @brief Run the bit-banged scan both ways round. */
    void scanBothOrientations();

    /** @brief Bit-bang scan the other hardware I2C pin pairs, both ways round. */
    void scanOtherPinPairs();

    /** @brief Bit-bang scan at progressively slower clocks (10/2/1 kHz). */
    void scanSlow();

    /** @brief Probe the reserved 0x00-0x07 range, where an unjumpered DuPPa sits. */
    void scanReserved();

    /** @brief Does the Wire library reach this address, or only bit-bang? */
    void compareWireVsBitBang(uint8_t addr);

    /** @brief Electrical test of SDA: can it be pulled low, and how fast does it rise? */
    void probeSdaDrive();

    /** @brief List pins that something external is pulling high. */
    void reportPullupPins();

    /**
     * @brief Watch the encoder's INT line for activity while the knob is moved.
     *
     * The Duppa pulls INT low on every rotation and press, and that path does
     * not involve I2C at all. So this answers "is the encoder alive and powered"
     * independently of whether the bus works - which separates a dead/unpowered
     * encoder from a purely I2C problem. Two faults that otherwise look the same
     * from the bus side.
     */
    void watchEncoderInt(uint32_t seconds);

    /**
     * @brief Service the INT watch. Call every main-loop pass.
     *
     * The watch is non-blocking on purpose. A console command that sits in a
     * loop for twelve seconds starves the watchdog and resets the board - which
     * is precisely what happened, and precisely what the watchdog is for.
     * Feeding the watchdog from inside a wait loop would have defeated it, so
     * the watch is spread across main-loop passes instead.
     */
    void serviceEncoderIntWatch();

} // namespace i2c
