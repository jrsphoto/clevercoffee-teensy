# CleverCoffee, ported to Teensy 4.1 + ESP32-S3

A fork of [CleverCoffee](https://github.com/rancilio-pid/clevercoffee) that splits the firmware across two processors to drive a C.M.A. Laurentis "Astoria Argenta SME" 1N Junior: a 120 V, single-group heat exchanger machine with a rotary pump.

Upstream CleverCoffee runs on a single ESP32, which is the correct architecture for standard retrofits. This fork moves to a dual-MCU setup to isolate real-time control loops and safety interlocks from the high-overhead Wi-Fi stack and web server.

## Architecture: Why a Teensy + ESP32 Split

Running a 1500 W heating element, variable-speed pump, and safety watchdog on the same core servicing HTTP requests and MQTT introduces unnecessary scheduling risk.

In this implementation, responsibilities are strictly divided:

* **Teensy 4.1**: Operates as the deterministic core handling the heater PID, pump control, solenoid valves, sensor acquisition, and hardware watchdog.
* **ESP32-S3 Display Module**: Manages Wi-Fi, the web UI, MQTT telemetry, and the local GUI. It communicates with the Teensy over a framed serial link.

The control boundary is one-directional regarding safety. The Teensy functions entirely autonomously; if the display module is disconnected or crashes mid-shot, the machine completes the extraction without interruption.

### Hardware I/O Load on the Teensy

The Teensy 4.1 handles the dense real-time polling requirements:

* Dual PT100 RTDs (MAX31865, 4-wire) sharing one hardware SPI bus with separate chip selects
* I2C rotary encoder input
* Dual HX711 load cell channels for gravimetric dosing (brew-by-weight)
* Flow meter pulse counting
* Hardware PWM output driving a DRV8871 motor driver for pump control
* UART telemetry link to the display head

## Firmware Division of Labor

* **`src/` (Teensy 4.1)**: Contains the deterministic control loop, safety state machine, sensor filtering, and actuator drivers.
* **`src-esp32s3/` (ESP32-S3)**: Houses the LCDwiki 4.0-inch (ST7796S capacitive touch) interface code, drawn directly with LovyanGFX, plus the web server and network services.
* **Serial Protocol**: A lightweight, checksum-verified ASCII protocol running over UART at 115200 baud. The link is non-blocking; missing heartbeat frames trigger UI stale-state warnings but never impact PID timing.

## Reset Behavior and Power Isolation

The two microcontrollers maintain completely independent power-on reset circuits.

The display module cannot reset the Teensy. Because the ESP32 handles network tasks prone to occasional memory fragmentation or watchdog reboots during heavy OTA operations, a hard reset line tied to the controller would risk cutting power to the brew valve or heater mid-shot.

Boot synchronization is handled via a unidirectional advisory message: when the Teensy completes its self-test and boots, it transmits an initialization frame. The display head listens for this frame to sync variables and clear stale telemetry, keeping the authority model strictly centered on the controller.

## Configuration Across Two Processors

The display head holds the persistent master configuration, stored as JSON on its filesystem, and serves upstream's `/parameters` API to the browser. The Teensy keeps a validated EEPROM cache of the control values it needs, so it boots and runs correctly with the display unplugged.

Every value crossing the link is applied through the controller's own validating setters. A setting the Teensy considers out of range is refused regardless of what the web UI asked for, so a wrong or malfunctioning front end cannot do anything an operator at the panel could not.

## Safety Implementation

Because this build lacks an external secondary relay chain, the firmware layer enforces strict functional safety constraints:

* **Watchdog Enforcement**: The Teensy's hardware watchdog runs continuously; a software hang forces an immediate fail-safe state (relays open, pump disabled).
* **Permit Expiration**: Control loops cannot rely on stale static boolean flags. The heater ISR refuses to energize on an execution stamp older than 500 ms, so a stalled main loop drops the heater automatically. Pump speed commands expire the same way.
* **Stale Readings Are Not Readings**: A sensor value that has stopped updating is treated as absent rather than as the last good value, and staleness is measured from the last successful read so a dead sensor cannot refresh its own freshness.
* **Dry-Fire Interlock**: Heater power is inhibited unless the boiler water level probe confirms water. That probe circuit is not built yet, so the firmware currently refuses to heat at all. This is correct behavior, not a defect.
* **Output Stage Verification**: A monitor pin on the heater channel samples the ULN2003 output, so the duty actually reaching the driver can be measured against what the PID requested. This checks the whole path from PID through the ISR and the Teensy pin to the ULN output.

An inline thermal fuse serves as the final non-resettable physical backstop inside the chassis. The firmware neither knows about it nor relies on it.

### Not Implemented: Hot Water and Steam

Upstream supports switched hot water and a second steam PID setpoint. Neither is ported here. Both are manual valves on this machine and the controller board has no output or switch for either. Upstream's steam mode raises a single boiler from brew temperature to steam temperature, which a heat exchanger already held at steam temperature does not have.

## Current Validation Status

| Subsystem | Status |
| :--- | :--- |
| Core safety state machine & watchdog | Verified |
| Dual MAX31865 4-wire PT100 RTD interfaces | Verified |
| Digmesa DM60 flow sensor pulse counting | Verified |
| DuPPa I2C encoder polling | Verified |
| ESP32-S3 display link & touch input | Verified |
| Debounced group head switch w/ long-press | Verified |
| Dual HX711 load cell amplifier chain | Verified (bracket machining pending) |
| DRV8871 PWM pump speed control | Verified on logic analyzer, 0.4% of commanded |
| Boiler PID & ULN2003 driver logic | Verified (bench tested, unheated) |
| Brew sequence with encoder-selected profiles | Verified |
| Manual flush and automatic backflush | Verified |
| Pre-shot cooling flush | Logic verified; coefficients uncalibrated |
| Wi-Fi, web server, and settings over the link | Verified |

*Note: The heating element has not been energized on mains voltage during initial bench verification phases; all interlocks and PID ramp responses were validated via isolated logic testing. Nothing is plumbed, so hydraulic behavior is unverified.*

The cooling flush computes its duration from the group probe, but the two coefficients that conversion needs depend on exchanger volume and the group's thermal mass. Neither can be derived from a datasheet and both require a machine with water in it. The feature therefore ships disabled, with its placeholder values flagged as uncalibrated in both the serial console and the web UI.

## Build Configuration

Managed via PlatformIO with three defined environments:

```ini
pio run -e teensy41     # Control firmware (Teensy 4.1)
pio run -e esp32s3      # Display head firmware (ESP32-S3)
pio run -e esp32_usb    # Upstream reference target
```

The `build_src_filter` rules in `platformio.ini` separate target source paths to prevent cross-contamination between the Teensy real-time codebase and upstream ESP32 components.

The display head's web interface is a separate filesystem image and therefore a second upload:

```ini
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

*Standard upstream builds support browser-based flashing via the [Web Flasher](https://rancilio-pid.github.io/clevercoffee-flasher/) for single-chip ESP32 deployments. This flashing workflow does not apply to the custom dual-MCU Teensy target described in this fork.*
