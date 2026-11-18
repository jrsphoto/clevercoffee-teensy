# CleverCoffee, ported to Teensy 4.1 + ESP32-S3

Derived from [CleverCoffee](https://github.com/rancilio-pid/clevercoffee) and splitting the firmware across two processors to drive a C.M.A. Laurentis "Astoria Argenta SME" 1N Junior: a 120 V, single-group heat exchanger machine.

This is a standalone repository rather than a GitHub fork, because the architecture diverges far enough that nothing here is a candidate for merging upstream. It remains a derivative work of CleverCoffee and stays under the same GPL-2.0 licence, with upstream's file and class names preserved wherever they still fit.

Upstream CleverCoffee runs on a single ESP32, which is the correct architecture for standard retrofits. This project moves to a dual-MCU setup to isolate real-time control loops and safety interlocks from the high-overhead Wi-Fi stack and web server.

## Architecture: Why a Teensy + ESP32 Split

Running a 1000 W heating element, variable-speed pump, and safety watchdog on the same core servicing HTTP requests introduces unnecessary scheduling risk.

In this implementation, responsibilities are strictly divided:

* **Teensy 4.1**: Operates as the deterministic core handling the heater PID, pump control, solenoid valves, sensor acquisition, and hardware watchdog.
* **ESP32-S3 Display Module**: Manages Wi-Fi, the web UI, and the local GUI. It communicates with the Teensy over a framed serial link.

The control boundary is one-directional regarding safety. The Teensy functions entirely autonomously; if the display module is disconnected or crashes mid-shot, the machine completes the extraction without interruption.

### Hardware I/O Load on the Teensy

The Teensy 4.1 handles the dense real-time polling requirements:

* Dual PT100 RTDs (MAX31865, 4-wire) sharing one hardware SPI bus with separate chip selects
* I2C rotary encoder input
* Dual HX711 load cell channels for gravimetric dosing (brew-by-weight)
* Two independent flow inputs, counted on separate interrupts
* PWM driving a DRV8871 H-bridge for pump speed, with both bridge inputs driven so the bridge brakes rather than coasts between pulses
* UART telemetry link to the display head

The OEM rotary vane pump failed and is replaced by a gear pump on a brushed 24 V motor, which is why pump control is PWM into an H-bridge rather than a triac on a vibratory pump.

Two flow connectors are fitted on purpose, one per sensor, each with its own pull-up, filter and Teensy pin. They are 25x apart in resolution, so each channel carries its own calibration both compiled in and in EEPROM. Reading one sensor through the other's constant is a failure that looks entirely plausible in the logs, and keeping the constants apart is what prevents it.

## Firmware Division of Labor

* **`src-controller/` (Teensy 4.1)**: Contains the deterministic control loop, safety state machine, sensor filtering, and actuator drivers.
* **`src-display/` (ESP32-S3)**: Houses the LCDwiki 4.0-inch (ST7796S capacitive touch) interface, the web server and network services.
* **Serial Protocol**: A lightweight, checksum-verified ASCII protocol running over UART at 115200 baud. The link is non-blocking; missing heartbeat frames trigger UI stale-state warnings but never impact PID timing.

The panel is built in [SquareLine Studio](https://squareline.io/) and runs its LVGL export, with LovyanGFX underneath as the panel driver only. Generated code lives in `src-display/ui/` and is regenerated wholesale on every export, so all display logic that has to stay correct sits in the hand-written `src-display/uiBind.cpp` instead: placeholders rather than stale values, chart gaps where a sensor was unhealthy, and never the word "grams" on an uncalibrated scale. The earlier hand-drawn LovyanGFX UI is frozen, uncompiled, in `reference/display-lovyangfx/`.

## Reset Behavior and Power Isolation

The two microcontrollers maintain completely independent power-on reset circuits.

The display module cannot reset the Teensy. Because the ESP32 handles network tasks prone to occasional memory fragmentation or watchdog reboots during heavy OTA operations, a hard reset line tied to the controller would risk cutting power to the brew valve or heater mid-shot.

Boot synchronization is advisory in both directions, and neither message has any authority. When the Teensy completes its self-test it announces itself, and the head uses that to sync variables and clear stale telemetry. The head also sends one message of its own at boot, which prompts the controller to re-announce: without it, reflashing the head left it unable to say what the controller was running, because the Teensy only ever announced at its own boot. The controller ignores the routine heartbeats that follow.

## Configuration Across Two Processors

The display head holds the persistent master configuration, stored as JSON on its filesystem, and serves upstream's `/parameters` API to the browser. The Teensy keeps a validated EEPROM cache of the control values it needs, so it boots and runs correctly with the display unplugged.

Every value crossing the link is applied through the controller's own validating setters. A setting the Teensy considers out of range is refused regardless of what the web UI asked for, so a wrong or malfunctioning front end cannot do anything an operator at the panel could not.

## Safety Implementation

Because this build lacks an external secondary relay chain, the firmware layer enforces strict functional safety constraints:

* **Watchdog Enforcement**: The Teensy's hardware watchdog runs continuously; a software hang forces an immediate fail-safe state (relays open, pump disabled).
* **Permit Expiration**: Control loops cannot rely on stale static boolean flags. The heater ISR refuses to energize on an execution stamp older than 500 ms, so a stalled main loop drops the heater automatically. Pump speed commands expire the same way.
* **Stale Readings Are Not Readings**: A sensor value that has stopped updating is treated as absent rather than as the last good value, and staleness is measured from the last successful read so a dead sensor cannot refresh its own freshness.
* **Dry-Fire Interlock**: Heater power is inhibited unless the boiler water level probe confirms water. That probe circuit is not built yet, so the firmware currently refuses to heat at all. This is correct behavior, not a defect.
* **Brew-by-Weight Always Has a Time Backstop**: Upstream's time stop is optional. Here it cannot be disabled, because a weight target is the only stop condition with a sensor behind it, and a scale that stops reporting mid-shot would otherwise leave nothing to end the extraction.
* **One Sequence at a Time**: The shot, the manual flush and the backflush drive the same valve and pump, and one loop services all three. They are mutually exclusive against a single shared list of what is running, rather than each start function keeping its own idea, which is how a case came to be missed once already.
* **Output Stage Verification**: The heater's 10 ms ISR can sample the ULN2003 output one tick at a time, immediately after driving the pin, and count mismatches. That measures the whole path from PID through the ISR and the Teensy pin to the ULN output, and a channel that dies mid-shot shows up as a mismatch count rather than as a boiler that quietly stopped heating. It is a bench instrument rather than a standing feature: the monitor line is not a trace on the board, it is a wire fitted by hand, and the firmware leaves the check disarmed until a test arms it. Never fit that wire once the SSR side is live, since the pin is not 5 V tolerant and the SSR rail is 24 V.

A non-resettable inline thermal cutoff is planned as the final physical backstop inside the chassis, independent of the firmware. **It is not fitted yet**, and its trip temperature cannot be chosen until the boiler is reassembled and can be measured under pressure. Nothing in the firmware knows about it or relies on it either way.

Note also what is deliberately absent: there is no heater control anywhere in the web UI or on the panel. The output module's permit check does not consult the dry-fire interlock, so a heater hold reachable from a browser would energize the element with no water check. Adding one means routing it through the full gate chain first.

### Deliberately Not Ported

**Hot water and steam.** Upstream supports switched hot water and a second steam PID setpoint. Neither is ported. Both are manual valves on this machine and the controller board has no output or switch for either. Upstream's steam mode raises a single boiler from brew temperature to steam temperature, which a heat exchanger already held at steam temperature does not have.

**MQTT.** Upstream has it and it works; there is no use for it here. The web UI and the panel cover everything this machine needs, so a broker would be another always-on dependency earning nothing.

**Load shedding.** An earlier version refused to run the heater and pump together, to keep both off one 15 A circuit. That made sense against the OEM 2000 W element. A 1000 W element at 8.3 A plus a 24 V DC pump drawing well under an amp never approaches the limit, so the mechanism was removed rather than disabled. Two gates that can never fire, sitting in the heater permit chain and the pump's refusal path, are worse than none.

## Current Validation Status

| Subsystem | Status |
| :--- | :--- |
| Core safety state machine & watchdog | Verified |
| Dual MAX31865 4-wire PT100 RTD interfaces | Verified |
| Flow pulse counting, both channels | Verified with water on the fitted meter; the second connector is wired and counts nothing yet |
| DuPPa I2C encoder polling | Verified |
| ESP32-S3 display link & touch input | Verified |
| Debounced group head switch w/ long-press | Verified |
| Dual HX711 load cell amplifier chain | Verified, but **uncalibrated**, so brew-by-weight cannot stop a shot. Bracket redesign also pending |
| DRV8871 PWM pump speed control | Verified on logic analyzer, within 0.4% of commanded |
| Boiler PID & ULN2003 driver logic | Verified (bench tested, unheated) |
| Brew sequence with encoder-selected profiles | Verified; the three profiles live in controller EEPROM |
| Manual flush and automatic backflush | Verified, including two-tap arming |
| Pre-shot cooling flush | Logic verified; coefficients uncalibrated, so shipped disabled |
| Wi-Fi, web server, and settings over the link | Verified |
| Live diagnostics page and screen sleep | Verified |
| On-panel boiler temperature chart | Verified |
| Boiler autofill | Not written |
| Firmware update over the link | Gating written and verified; the flashing half is not written |

*Note: The heating element has never been energized on mains. Every interlock was proven with no element attached, and the one that remains unproven is the dry-fire interlock, because the water probe circuit does not exist yet. The brew pressure sensor and the water level probe are both outstanding hardware.*

*The machine itself is not plumbed, so its hydraulic behaviour is unverified. Flow and pump work is done on a bench rig where the meter discharges into the same reservoir the pump draws from.*

The cooling flush computes its duration from the group probe, but the two coefficients that conversion needs depend on exchanger volume and the group's thermal mass. Neither can be derived from a datasheet and both require a machine with water in it. The feature therefore ships disabled, with its placeholder values flagged as uncalibrated in both the serial console and the web UI.

## Build Configuration

Managed via PlatformIO with three defined environments:

```bash
pio run -e teensy41      # Control firmware (Teensy 4.1)
pio run -e teensy41_sim  # Control firmware plus a boiler model, for desk work
pio run -e esp32s3       # Display head firmware (ESP32-S3)
```

The `build_src_filter` rules in `platformio.ini` separate target source paths to prevent cross-contamination between the Teensy real-time codebase and the display head. `teensy41_sim` is `teensy41` plus a thermal model of the boiler and nothing else, so the firmware that goes into the machine does not contain the simulation at all. It is a separate build rather than a runtime switch on purpose: there is no mode to leave enabled by accident.

The upstream all-ESP32 reference target was removed once the port no longer needed it. Upstream CleverCoffee remains the reference and is tracked by the `upstream` remote.

### Repository Layout

```
src-controller/   Teensy 4.1 control firmware
src-display/      ESP32-S3 head: panel, web server, network
  ui/             SquareLine's generated LVGL export, regenerated wholesale
  uiBind.cpp      hand-written display logic, safe across a re-export
lib/LinkProtocol/ frame format shared by both boards
frontend/         web UI, uploaded as a separate filesystem image
ui-design/        the SquareLine project the panel is exported from
docs/             port brief, pin review, SquareLine notes, config reference
reference/        frozen, uncompiled code kept for reference only
scripts/          build helpers
```

The display head's web interface is a separate filesystem image and therefore a second upload:

```bash
pio run -e esp32s3 -t uploadfs
```

## Relationship to Upstream

This repository is structured to maintain file-level readability alongside [rancilio-pid/clevercoffee](https://github.com/rancilio-pid/clevercoffee), preserving modular naming conventions where applicable to simplify tracking upstream patches.

Please direct support questions about CleverCoffee itself to the upstream project rather than here. Their Discord supports their PCBs, and this is neither.

---

# About Upstream CleverCoffee

*The following section details the base upstream project.*

CleverCoffee implements an open-source PID controller designed for thermal stability on single-boiler espresso machines, originally built for the Rancilio Silvia and expanded to Gaggia and Quickmill platforms.

Core features provided upstream include:

* Accurate PID temperature regulation
* Configurable pre-infusion profiles
* Gravimetric brew-by-weight and time-based dosing
* Automated backflush cycles
* MQTT integration and web-based configuration dashboards

Project Resources:

* **Website:** [Clever Coffee](https://clevercoffee.de)
* **Hardware:** [Clever Coffee Shop](https://shop.clevercoffee.de/)
* **Documentation:** [Handbook (German)](https://rancilio-pid.github.io/ranciliopid-handbook/)
* **Community:** [Discord Support](https://discord.gg/Kq5RFznuU4)

*Standard upstream builds support browser-based flashing via the [Web Flasher](https://rancilio-pid.github.io/clevercoffee-flasher/) for single-chip ESP32 deployments. That workflow does not apply to the dual-MCU Teensy target described here.*
