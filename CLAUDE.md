# CleverCoffee → Teensy 4.1 + ESP32-S3 port (Argenta SME build)

This folder is a clone of the working ESP32 CleverCoffee firmware. The job here is to **split it into two firmwares** for the next board of the Argenta SME espresso machine: a **Teensy 4.1** that runs all real-time control, and an **ESP32-S3 touch-display module** that runs WiFi, the web UI, and the on-machine color LCD. See `TEENSY-PORT-BRIEF.md` in this folder for the full technical brief and the phased plan.

This is **safety-critical mains work** (a ~1500 W heater on a boiler). Accuracy and testing matter more than speed. Verify against the actual code and datasheets; do not assert from memory.

## Read these first (parent project context)

The parent folder (`../`) is the whole machine project. Before planning, read:
- `../CLAUDE.md` — the project guide (machine, goals, current status, the Teensy direction block).
- Memory notes (the authoritative design record for this pivot): `teensy-esp32-architecture-pivot` and `mg304-pump-and-brew-sensors` in the project's auto-memory. If they are not auto-loaded in this session, ask the user for them or read the project `MEMORY.md`.
- `../pin-map.md` (current ESP32 board; Teensy pins NOT assigned yet), `../parts-list.md`, `../firmware-plan.md`, `../documents/fluid-o-tech-pumps.md`, `../documents/thermal-management.md`.
- `../documents/github-repos.md` before any git work.

## Prime directive: stay close to upstream CleverCoffee

Keep the code laid out **as much like the current CleverCoffee code as possible**. Same modular shape: hardware devices behind small interfaces (`TempSensor`, `Relay`, `Switch`, `Scale`, `LED`), features in `xHandler` modules called from the `main.cpp` loop, the `Config` / `ParameterRegistry` / `Parameter` config system, the same file and class names wherever they still fit. The Teensy firmware should read like CleverCoffee with the **hardware layer swapped** and a **comms layer added**; the ESP32-S3 firmware keeps the web/config/display parts. Minimize gratuitous divergence so upstream changes stay cross-referenceable (upstream remote: `github.com/rancilio-pid/clevercoffee`).

## The two processors (who owns what)

**Teensy 4.1** (NXP i.MX RT1062, 600 MHz Cortex-M7, 3.3 V I/O, hardware FPU, FlexPWM/QuadTimer, hardware pulse/quadrature counters, many hardware SPI/I2C/UART, lots of GPIO). Owns **all real-time control and all the physical I/O**:
- Heater PID; brew/steam/hot-water state machines; autofill; standby/power logic.
- Pump: PWM out to John's **brushed-motor driver board** (drives the Fluid-O-Tech MG304 gear pump via the KAG brushed motor). This is variable-speed control; it is NOT an on/off relay.
- Sensors: two **MAX31865** PT100 RTDs (SPI, boiler + group/HX-outlet); brew **pressure** sensor (I2C, 0-10 bar); **DM60 Nano flow** sensor (open-collector pulse → hardware pulse counter); **HX711** scale (brew-by-weight).
- Outputs: heater SSR, pump, fill valve, group valve — via the **ULN2003** buffer. SSR control drive is now **24 V** (shared with the pump's 24 V rail), but 24 V appears ONLY on the ULN2003 output side.
- Physical UI: the **Duppa i2cEncoder V2** (I2C) — rich button gestures (short/long/double press + combos) AND its **RGB LED as the machine-status indicator** (idle/heating/ready/active/error), driven directly by the Teensy since it owns the state; plus the **PCF8574** dose-button expander (I2C); plus an optional hardwired brew switch.
- **All safety** (see below).

**ESP32-S3 touch-display module** (Hosyond, ST77922 QSPI 3.5" 480×320 capacitive-touch color LCD, WiFi/BT, 8 MB PSRAM / 16 MB flash, 3.3 V I/O, USB-C). A **head/terminal only**:
- WiFi, the CleverCoffee web server, the config front-end, MQTT.
- The on-machine color touch LCD, rendered from telemetry streamed by the Teensy.
- Owns **no actuators and no safety.** A touch on its screen is a *command* sent to the Teensy, which decides.

**Link:** one **UART** between them (both 3.3 V, no level shifter). **Teensy is authoritative.** OnStep-style ASCII/line command + telemetry protocol (ESP32 → commands/config, Teensy → telemetry/state). The link is **non-critical**: if it drops, the Teensy runs autonomously and completes the shot normally; a dead display never stops the machine. Precedent to study: **OnStep** (`github.com/hjd1964/OnStep`) — Teensy motion control + a separate ESP32 web/WiFi board over serial, controller authoritative, display can drop without affecting control.

## Safety stance (important, and a change from the old design)

**Assume there are NO external fail-safe relays. The Teensy controls everything and is the only line of defense.** Therefore safety logic is first-class, not an afterthought, and must be written to be obvious, auditable, and hard-tested:
- **Fail to a safe state** (heater OFF, pump OFF, valves de-energized) on ANY of: RTD fault/open/short (MAX31865 fault flags), out-of-range or implausible readings, watchdog timeout, boot self-check failure, loss of the water-level probe signal, over-temperature, over-pressure.
- **Hardware watchdog** on the Teensy that resets into the safe state.
- **Dry-fire / low-water interlock:** no heater power unless the water-level probe confirms water.
- **Over-temperature cutoff** in firmware on the boiler RTD, hard limit well below danger.
- **Load-shed** interlock configurable (heater + pump never simultaneously). With the MG304 drawing ~0.7 A it may be unnecessary — make it a config option, default on until confirmed.
- **Default-off outputs:** SSRs and valves de-energized = safe; everything off at boot until self-checks pass; the existing ULN input pulldowns keep outputs low at reset.
- Keep the **autofill anti-flood timeout.**

What's dropped vs kept at the hardware level: the independent **relay-based** low-water safety channel (the 61F + G7L) is gone — the Teensy does low-water / dry-fire protection in firmware now. A **passive one-shot thermal cutoff (TCO) fuse stays inline in the heater line** as the sole non-MCU backstop (John confirmed 2026-10-26). The firmware neither knows nor relies on it — it's dumb insurance, and the firmware still assumes nothing catches its mistakes.

**KiCad rules (this session will read the Teensy schematic):** read the `.kicad_sch` files to answer pin/schematic questions — never eyeball the PNG render. **Do NOT edit `.kicad_sch` / `.kicad_pcb`** (John drives KiCad; hand-edits are too risky). KiCad may be open (check for `~*.lck` lock files); reading is always fine. Deliver pin changes as exact, click-by-click steps for John to make, plus the corrected pin table.

## Hard constraints

- **3.3 V discipline.** Neither chip is 5 V tolerant. Anything feeding a Teensy or ESP32 pin must be ≤ 3.3 V (open-collector sensors pulled up to 3.3 V, etc.). 24 V lives only on the ULN2003 output/COM side.
- **Don't fold safety or actuators onto the ESP32.** Don't make the link safety-critical.
- **Config is the single source of truth**, and it now spans two processors: the ESP32 web UI edits settings, values cross the link to the Teensy. Keep keys and defaults in sync; the `kColor` encoder settings John added are an example of config that now rides the link.
- **Teensy pins are drafted in the KiCad schematic, and reviewing them is the FIRST task — before any porting** (see `TEENSY-PORT-BRIEF.md` §6 phase 0 and §8). The schematic is the source of truth for pins; `pinmapping.h` mirrors it once it's settled. **Do NOT invent final pin numbers** and scatter them through the code — take them from the agreed schematic. Until the review is done, keep `pinmapping.h` a clearly isolated placeholder.
- **The DM60 flow handler is net-new.** CleverCoffee has no flow-sensor code anywhere (checked fork + upstream master/develop); its only shot-stop methods are by time and by weight (HX711). Write the flow path from scratch; on the Teensy use its hardware pulse counter.
- Build system is **PlatformIO**: a Teensy 4.1 env (`platform = teensy`, `board = teensy41`) and an ESP32-S3 env (`platform = espressif32`, the 8 MB PSRAM / 16 MB flash S3 board). For the ST77922 QSPI panel, confirm a graphics lib that supports it (LovyanGFX or Arduino_GFX) before committing to the display code.

## How to work here

- This is a large refactor. **Propose a plan and get agreement before mass edits** (use plan mode). Start by reading the code and producing a port inventory, not by editing.
- `pio` is not on PATH; use `/home/john/.platformio/penv/bin/pio`.
- Test the safety paths hard. Report failures with the actual output. When unsure, measure or ask — don't assert.
