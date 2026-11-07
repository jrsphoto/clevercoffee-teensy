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

### 9.2 Hot water and steam are NOT being ported (2026-11-05)

**Decision:** upstream's `hotWaterHandler.h` and `steamHandler.h` are not ported. Hot water and steam on this machine are manual valves and stay that way. John confirmed both valves exist and considered converting them to solenoids, and decided against it: he is building this for espresso, and he is happy to steam milk by hand.

**Why they cannot be ported even if wanted.** This is not a preference, it is what the board is. Read from the schematic (`relays.kicad_sch`, confirmed 2026-11-05):

- Three actuator nets exist: `HEATER`, `GROUP`, `FILL`, plus `PUMP_PWM`. There is no steam valve output and no hot water valve output.
- One switch input exists: `BREW_SW`. There is no steam switch and no hot water switch.

Upstream's `hotWaterHandler` (267 lines) reads a hot water switch and runs the pump to deliver water. Porting it would produce a handler with no input to read and no output to drive. The same decision was already taken on the ESP32 board - see `../firmware-plan.md` item 1, which removed the power, steam and hot water switch code because those switches "don't physically exist on this board either."

**Why steam is doubly inapplicable.** Upstream's `steamHandler` (55 lines) sets `steamON`, which selects a second, higher PID setpoint. That is a single-boiler idea: lift the boiler from brew temperature to steam temperature. This is a heat exchanger machine whose boiler is *already* a steam boiler held near 123 C permanently, and `../documents/thermal-management.md` section 6 states that the boiler setpoint is not free to move because boiler temperature sets the steam pressure. There is no second setpoint to switch to. Porting steam mode would be porting a concept this machine does not have.

**What is genuinely lost, and it is not nothing.** A steam or hot water draw dumps water and heat out of the boiler, so autofill should respond and the PID will go to full output. Recognising that as a *mode* would be useful. Section 6 of the thermal document calls this mode detection and says it needs a boiler-side low-range pressure sensor to do properly. John confirmed 2026-11-05 that he has only the machine's mechanical dual gauge and the one I2C brew-side sensor, so there is no low-range boiler sensor and no way to detect a draw. This is parked, not forgotten, and it is a sensor problem rather than a firmware one.

**What this frees up.** If a solenoid for either is ever wanted, the board can take it without a respin: ULN channels 4-7 are unused and Teensy pins D4, D6, D7 and D8 are spare.

### 9.3 Manual flush and backflush, and the brew switch gesture change (2026-11-05)

**Decision:** port upstream's `manualFlush()` and `backflush()` into `shotHandler`, which required also porting upstream's `BrewSwitchState` gesture machine and changing when a shot starts.

Upstream reads a momentary brew switch as a *gesture*, not a level, because one switch has to mean three things: short press brews, long press flushes, and short press while backflush is armed backflushes. That costs something real - a shot now starts when the switch is **released**, not when it is pressed, because the firmware cannot tell a press from a hold without waiting to find out. A toggle switch keeps level behaviour and, as upstream, has no flush gesture at all (`IOSwitch::longPressDetected()` returns false for toggles).

Two deliberate divergences, both recorded in `shotHandler.h`:

1. **A manual flush is time-capped; upstream's is not.** Upstream's flush runs as long as the switch is held, with the operator's finger as the only limit. That is the right shape, but it assumes the switch is telling the truth, and a stuck switch or a lead chafed onto the rail is indistinguishable from a finger that never lets go. Same reasoning as the shot time limit: it is the only stop condition with no sensor behind it.
2. **Backflush must be explicitly armed and is never armed at boot.** Upstream's `backflushOn` comes from the web UI; here it is a console command with a loud warning, because backflushing drives a gear pump with no internal relief against a blind basket, the mandatory mechanical relief valve is not fitted, and there is no brew pressure sensor to notice over-pressure. The firmware cannot check any of that and does not pretend to - arming is the only interlock, and a fake one would be worse than none.

**Verified on hardware 2026-11-06.** LA2016 on `HEATER`, `FILL`, `GROUP`, `PUMP_PWM` and `BREW_SW` simultaneously, with the firmware's own log as an independent second record. Nothing plumbed; the machine is in pieces, so these are electrical sequences only.

| Test | Result |
|---|---|
| Flush, console-started | `GROUP` high, `PUMP_PWM` **34.98% at 19998 Hz** against 35% commanded. `HEATER` and `FILL` never moved |
| Flush time cap | fired at the cap, logged "MAXIMUM TIME - check the brew switch is not stuck" |
| Backflush, 5 cycles | fill 2.999 s / 2.999 s / 2.999 s against 3000 ms; flush 5.001 s / 5.001 s against 5000 ms; ran all five and returned to idle unaided |
| Brew switch HOLD | 3.035 s hold. Flush started 564 ms after contact (500 ms threshold + 20 ms debounce + loop latency) and stopped **21 ms** after release. Firmware said 2.5 s, analyser said 2.492 s |
| Brew switch TAP | 239 ms tap. Shot's valve opened **21 ms after release**, confirming a shot starts on release as designed, then pre-infusion, soak and full-speed extraction in order |
| Shot time backstop | the tapped shot hit its limit unprompted: "STOPPED after 60.0 s - MAXIMUM TIME - check the scale and flow sensor", with no scale and no flow sensor fitted. The one stop condition with no sensor behind it, proving itself without being asked to |

### 9.4 The bug worth remembering: a level where an edge was needed (2026-11-06)

Three separate features were broken by one mistake, and it took hardware to find it.

The brew-switch abort was written as "**if** the switch is idle, stop whatever is running". The switch is idle almost all the time, so anything not started by the switch died on the very next loop pass. A console-started flush, backflush or shot started and stopped inside the same millisecond.

It needed to be "if the switch **has just returned** to idle" - an edge, not a level.

Three things are worth keeping from this:

- **Upstream was right and still did not transfer.** Upstream cannot hit this because the switch is its only way to start anything, so its gesture state is never Idle while something runs. The level test is safe there and unsafe here. Porting faithfully is not the same as porting correctly: a correct original can become wrong under a new assumption, and the new assumption here was simply "there is now a second way in".
- **The shot path had the same bug before any of this work**, inherited from the original `if (!pressed) { ... stop(); }`. So `shot start` from the console had never worked while switch control was on, in code marked verified on hardware. What had been verified was the switch path; the console path had never been exercised.
- **It compiled, it read correctly, and the status output looked right.** Only running it showed it. Worth remembering next time a sequence seems too simple to need the bench.

The fix also kept the switch's authority: because it triggers on a transition, a console-started shot can still be stopped at the panel, since pressing and releasing produces a real return to Idle.
