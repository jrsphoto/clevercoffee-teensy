# Teensy 4.1 + ESP32-S3 port — technical brief

Companion to `CLAUDE.md` in this folder. This is the detailed "what we're building and in what order." Treat the phased plan as a proposal to refine with the user, not a fixed script.

## 1. Target architecture

Two firmwares, one repo (this folder). A positive-displacement gear pump and a mains heater make this real-time and safety-critical, which is why control lives on the Teensy.

```
        [ Duppa encoder ]  [ PCF8574 buttons ]  [ brew switch ]
                 \               |                 /
   RTDs(SPI) ----  \             |               /  ---- pressure(I2C)
   flow(pulse) ---- [   TEENSY 4.1  (control + safety)   ] ---- scale(HX711)
   pump PWM ------ /   runs the CleverCoffee control core  \ ---- SSRs/valves via ULN2003 (24V out)
                  /                    |                     \
                                   UART (3.3V)
                                       |
                        [ ESP32-S3 touch-display module ]
                         WiFi · web server · config front-end · color LCD
                                       |
                                   WiFi / browser
```

- **Teensy = authoritative.** Owns the loop, the state, the actuators, the physical UI, all safety.
- **ESP32-S3 = head.** WiFi + web + LCD. Renders from telemetry; sends commands. No actuators, no safety.
- **Link = non-critical.** Drop it and the machine still runs and finishes shots safely. Model it on OnStep's main-board ↔ SmartWebServer serial design (`github.com/hjd1964/OnStep`): simple ASCII command set, controller authoritative, polled telemetry, WiFi side disposable.

## 2. Where the existing code goes

The current tree divides cleanly. Rough first-pass mapping (refine during the inventory):

| Current file/area | Destination | Notes |
|---|---|---|
| `main.cpp` loop, `brewHandler`, `steamHandler`, `hotWaterHandler`, `powerHandler`, `standby`, `brewStates` | **Teensy** | The control core. Keep structure; swap only the hardware calls underneath. |
| `hardware/tempsensors/TempSensorMAX31865.*`, `TempSensor.h` | **Teensy** | Reconcile against the current `TempSensor` interface (the carried-over MAX31865 file predates the Timer-based refactor — see `../documents/github-repos.md`). |
| `hardware/Relay.*`, `IOSwitch.*`, `GPIOPin.*`, `Switch.h`, `LED`/`StandardLED` | **Teensy** | Re-implement the pin layer on Teensy GPIO/FlexPWM behind the same interfaces. |
| `hardware/pressureSensor.h` | **Teensy** | I2C brew pressure; feeds the pump loop. |
| `scaleHandler.h`, `hardware/scales/HX711Scale.*`, `Scale.h` | **Teensy** | Brew-by-weight. (`BluetoothScale` is ESP32-only; decide whether to keep it on the ESP32 side or drop it.) |
| `encoderHandler.h` + Duppa driver | **Teensy** | Encoder is Teensy-owned (gestures + RGB status). Port the DuPPa `i2cEncoderLibV2` to Teensy `Wire`. RGB-status logic belongs here now (Teensy owns the state). |
| **NEW flow handler** (DM60) | **Teensy** | Does not exist yet. Pulse counter → K-factor → integrate to volume → add as a `brewHandler` stop condition parallel to by-weight; also live flow for pump profiling. |
| **NEW pump handler** (MG304) | **Teensy** | PWM → John's brushed-motor driver. Manual mode (encoder sets flow) + auto mode (stored pressure/flow profile), closed-loop on the pressure + flow sensors. Enforce a pressure/flow ceiling in both modes. |
| `embeddedWebserver.h`, `frontend/`, `mqtt.h`, WiFi, `data/`, `partitions_4M.csv` | **ESP32-S3** | The web/config/comms head. |
| `display/` | **ESP32-S3** | Re-target from the mono SSD1309 (U8g2) to the ST77922 QSPI color panel + touch. This is a rewrite, not a port — confirm the graphics lib first. |
| `Config.*`, `Parameter.*`, `ParameterRegistry.*`, `ConfigDef.h`, `defaults.h` | **Both, split by the link** | Config lives on both sides. The ESP32 edits it via the web UI; the Teensy holds the values it needs for control. Decide the authoritative store and how changes propagate over the UART (see §4). Keep keys/defaults identical. |

Suggested repo layout to keep it CleverCoffee-shaped: keep `src/` as the **Teensy** firmware; add a sibling (e.g. `src-esp32s3/` or a second PlatformIO env/`lib` split) for the head; put the serial protocol message definitions in a **shared header** both build envs include, so there is one source of truth for the wire format.

## 3. Build

PlatformIO, two envs:
- `teensy41`: `platform = teensy`, `board = teensy41`, `framework = arduino`.
- `esp32s3`: `platform = espressif32`, the 8 MB PSRAM / 16 MB flash S3 board variant, `framework = arduino`, USB-CDC.
- `pio` binary: `/home/john/.platformio/penv/bin/pio`.
- Keep the existing single-app partition approach for the ESP32 (no OTA; USB on the back of the machine).

## 4. The serial link (design this early)

- Physical: one UART, 3.3 V both ends. Pick a baud (230400 or 1 Mbaud are both easy).
- Shape (OnStep-style): short ASCII/line-framed messages. Teensy authoritative.
  - **ESP32 → Teensy:** setpoints (target temp, profile selection, target weight/volume, brew time), mode changes, start/stop, config writes.
  - **Teensy → ESP32:** telemetry at ~10–20 Hz (boiler/group temp, brew pressure, flow, delivered volume/weight, pump duty, shot timer, machine state) and acks.
- Robustness: framing + a checksum, a heartbeat, and defined behavior on a dropped/garbled frame. **Dropped link is cosmetic, never a safety event** — the Teensy keeps running the shot to its normal endpoint.
- Config across the link: decide whether the Teensy or the ESP32 holds the master copy, and how a web edit reaches the Teensy and persists. (The `kColor` encoder settings are a concrete case: edited on the ESP32 web UI, consumed by the Teensy that drives the LED.)

## 5. Safety checklist (Teensy is the only line of defense)

Build and test these as first-class features, not add-ons:
- [ ] All outputs default OFF; nothing energizes until boot self-checks pass.
- [ ] Hardware watchdog → resets to the safe state.
- [ ] RTD fault/open/short (MAX31865 flags) and implausible temps → heater OFF, fault state, surfaced on the LED + display.
- [ ] Low-water / dry-fire interlock: no heat unless the probe confirms water.
- [ ] Firmware over-temp cutoff on the boiler RTD (hard limit).
- [ ] Over-pressure handling on the brew sensor; pump pressure/flow ceiling enforced in manual AND auto.
- [ ] Load-shed (heater + pump never together) — configurable, default on.
- [ ] Autofill anti-flood timeout preserved.
- [ ] A hardwired brew switch path that works even if the ESP32/UART is dead.
- [ ] Mechanical over-pressure relief is assumed present (the MG304 dead-heads) but is plumbing, not firmware — the firmware still enforces its own ceiling.

## 6. Suggested phases (propose, then get sign-off before editing)

0. **Pinout review FIRST (before any code).** Read the Teensy schematic (§8), extract John's current Teensy pin assignments, and verify each against (a) what the firmware needs and (b) the Teensy 4.1's real peripheral map. Flag conflicts and suboptimal choices; propose a corrected pin table. This table becomes `pinmapping.h`. Deliver changes as click-by-click steps for John — do not edit the schematic.
1. **Inventory** — read the code; classify every file Teensy / ESP32 / shared / rewrite; list ESP32-specific APIs (WiFi, LittleFS, ESPAsyncWebServer, Preferences, ESP timers) that need a Teensy equivalent or re-homing. Produce the port plan. No edits yet.
2. **Serial protocol** — define the shared message set and the config-sync approach.
3. **Teensy skeleton** — build env, `pinmapping.h` from the reviewed pin table (phase 0), main loop, the hardware interfaces compiling against Teensy stubs; fail-safe defaults and watchdog in from the start.
4. **Hardware layer on Teensy** — RTDs, relays/valves, pump PWM, flow pulse counter, pressure, scale, encoder + buttons. Bring up against the interfaces one at a time.
5. **Control core** — port brew/steam/hot-water/power/PID/standby, keeping structure; wire in the safety checklist.
6. **ESP32-S3 head** — web/config/MQTT + the UART client + the ST77922 color/touch UI.
7. **Integration + hard safety testing.**

## 7. Current-state facts the plan must respect

- **No PCB yet, but Teensy pins ARE drafted in the schematic** (§8) — review them in phase 0; `pinmapping.h` then mirrors the agreed table. Don't hardcode invented pins elsewhere.
- Pump = **MG304** (0.3 ml/rev) on a **brushed KAG motor + John's driver board**; control is **PWM**, not on/off (positive-displacement pump — speed *is* the flow control). No motor tacho; close the loop on pressure + flow.
- Flow = **Digmesa DM60 Nano**, open-collector, ~36–49k pulses/L, K-factor calibrated in-system.
- Brew pressure sensor and the DM60 are **in hand**; both MAX31865 RTDs are wired in the current ESP32 firmware (boiler on commit `daa1def`, not HW-verified) — reuse that logic.
- Safety hardware: the independent 61F+G7L relay low-water channel is **dropped** (Teensy does low-water/dry-fire in firmware); a **passive inline TCO thermal fuse is kept** in the heater line as the only non-MCU backstop (firmware doesn't rely on it).
- Keep everything **auditable and testable**; this controls a mains heater whose only backstop behind the firmware is that one passive TCO.

## 8. Teensy pin map — source and review (phase 0)

**Source of truth for pins = the KiCad schematic**, not this repo:
`../clevercoffee-hardware/CleverCoffee_Teensy41_Dev_SMD/` — root sheet `CleverCoffee_Teensy41_Dev_SMD.kicad_sch`, sub-sheet `relays.kicad_sch`. **Read** these `.kicad_sch` files (they're text); do **not** edit them and do not eyeball the `.png` render. KiCad may be open (check `~*.lck`). Deliver pin changes as click-by-click steps + a corrected table.

Cross-check each of John's assignments against the **real Teensy 4.1 pinout** (verify on PJRC's pin card, `https://www.pjrc.com/teensy/pinout.html` and `card11b_rev4_web.pdf` — don't trust memory for which pin does what). Signals the firmware needs a home for, with the peripheral each requires:

| Signal(s) | Needs |
|---|---|
| 2× MAX31865 RTD | one hardware **SPI** bus (SCK/MOSI/MISO) + 2 chip-selects (any GPIO) |
| Duppa encoder, PCF8574 buttons, brew pressure sensor | **I2C** (one bus or split across Wire/Wire1/Wire2); encoder also needs an **INT** input |
| ESP32-S3 link | a hardware **UART** (one of Serial1–8) — TX/RX pair |
| Pump speed → driver board | a **PWM** output; watch FlexPWM/QuadTimer grouping (pins sharing a timer share frequency — give the pump its own) |
| DM60 flow | a pulse/interrupt-capable input (all Teensy digital pins do `attachInterrupt`; for clean high-rate counting prefer an input-capture / `FreqMeasureMulti`-capable pin) |
| HX711 scale | clock + data (dual-HX711 = two data lines sharing one clock, per the Gaggiuino kit) |
| ULN2003 inputs | heater SSR, fill valve, group valve (+ optional pump power relay) — plain GPIO outputs |
| Water-level probe, brew switch | GPIO inputs |

Flag: any signal on a pin whose peripheral is already taken; two PWM loads forced onto one timer; I2C/SPI/UART signals on pins that can't do that function; anything colliding with pins reserved for the SD card or onboard QSPI flash; and the 3.3 V / not-5 V-tolerant rule on every input.

**Phase 0 is DONE** (2026-10-26). Result: `teensy-pinout-review.md` in this folder, now the source of truth for `src/hardware/pinmapping.h`. Three changes were made to the schematic and ERC passes clean: the ESP32-S3 link had no pins at all (added Serial1 on D0/D1), the pump PWM path had no default-off bias (pulldown added on PMP_PWM), and the leftover SPI OLED was removed, which also resolved a FlexPWM submodule clash with the pump.

## 9. Design decisions agreed during the port

Decisions taken as the work proceeds, recorded here so they do not get lost in session history. Newest last.

### 9.1 Scale weight survives a Bluetooth drop-out by learning from flow (2026-10-26)

**Decision:** keep the `BluetoothScale` support, and when a BT scale drops out mid-shot the Teensy continues the shot on a flow-derived weight estimate rather than aborting or falling straight back to time.

**Why this needs a decision at all.** `BluetoothScale` is NimBLE, so it can only live on the ESP32-S3, which means BT weight has to cross the UART link. That makes brew-by-weight-on-a-BT-scale the one feature where a dead link could degrade a shot. The wired HX711 is Teensy-local and completely unaffected.

**How the estimate works.** Do NOT convert millilitres to grams with a fixed coffee-science constant - the real ratio varies with dose, grind, roast and puck, so a constant is a guess presented as a measurement. Instead, while the scale is working the Teensy continuously measures the ratio between grams landing in the cup and millilitres through the DM60. That ratio is specific to this shot, this coffee, this puck. On drop-out it keeps integrating flow and converts using the ratio learned seconds earlier, anchored to the last known-good weight reading - so it only ever extrapolates the short remainder of the shot, never the whole thing.

**Guard rails** (an estimator must not be allowed to run a mains heater and a pump open-endedly):

- Only learn the ratio once the puck is saturated and weight is genuinely climbing. Early in the shot water goes into the puck rather than the cup, and the ratio there is meaningless.
- Cap how far the estimate will extrapolate. Past that, stop on a volume target or the hard time limit instead.
- If the scale drops before any ratio was learned there is nothing to extrapolate from - fall back to volume or time directly.
- Mark the shot as estimated in telemetry, logs and on screen. It must never report an estimated weight as a measured one.

**Free bonus.** When the scale and flow sensor are both healthy, predicted-versus-actual weight is a live sanity check. Divergence means something real is wrong (channeling, a knocked cup, a failing flow sensor), which is a useful diagnostic for nothing extra.

**Consequence for the port:** brew-stop logic must be designed from the start to take "weight, plus whether it was measured or estimated", rather than having a second path bolted on later. Implementation lands in phase 5 with the flow handler.
