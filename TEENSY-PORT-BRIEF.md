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
- [x] ~~Load-shed (heater + pump never together)~~ — **dropped entirely 2026-11-09.** Built, tested, then removed on John's decision: a 1000 W element (8.3 A) and a 24 V DC pump never approach a 120 V outlet's limit, so it had nothing to protect. No code, no config key, no console command. See 9.14.
- [ ] Autofill anti-flood timeout preserved.
- [ ] **Autofill must not run during a shot.** On this machine the boiler fill is fed from the PUMP OUTPUT (manual §10), so a cold-inlet flow meter counts fill water as delivered volume and a shot would stop short. Required for brew-by-volume to be truthful, not just for pressure stability. See 9.17.
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

### 9.5 The automated cooling flush (2026-11-06)

**Decision:** add a cooling flush as two leading stages of the shot sequence, computed **feed-forward** from the group probe, defaulting OFF with its coefficients declared uncalibrated.

**This one is genuinely net-new, and that was checked rather than assumed.** `git grep` across `upstream/master` and `upstream/develop` finds no cooling flush, no pre-brew flush and no idle-time flush anywhere. The only flush concepts upstream has are `manualFlush` and `backflush`, both of which were ported rather than written. `documents/thermal-management.md` section 6 calls this one the prize.

**Why it exists.** On a heat exchanger machine the water standing in the exchanger and the line to the group creeps towards boiler temperature while the machine idles, around 123 C. The first water through the puck is therefore far too hot. On most machines the fix is folklore: flush "a few seconds". With a probe on the exchanger outlet line it can be computed. At home it matters more, not less, because the machine is always coming off hours of idle, so every shot is a first shot.

**Why feed-forward and not closed-loop, which is the whole design.** The obvious implementation is to flush until the probe reads cool. That is wrong. The probe is a clamp-on surface sensor and per the thermal document it "reads close on a stagnant line, lags during flow" - during a flush it is still reading hot pipe metal while cool water is already past it. Waiting on that reading would flush long after the job was done, dumping boiler water and overcooling the group. So the reading is taken while the line is stagnant, which is exactly when it is accurate and exactly the condition before a shot, and used to compute a duration up front. The loop is closed only as an **early stop**, which can shorten a flush but never extend one, so the lag cannot cause harm through it.

**Idle time is a gate, not a coefficient.** The thermal document says to use idle time as well as temperature. Its real job turned out to be deciding whether the temperature can be *believed*: a reading taken shortly after water last moved is still in transit. Below a minimum undisturbed period the flush is skipped rather than computed from a settling reading - which is also physically correct, since back-to-back shots need no cooling flush. Upstream already tracks idle time in `standby.h` (`standbyModeStartTimeMillis`, reset by `resetStandbyTimer()`), so the same reset-on-activity shape is used here. **When standby is ported these must become one clock, not two that disagree.**

**Two numbers are placeholders and the feature is off by default.** Converting "the group is 118 C and I want 95 C" into seconds needs a constant depending on exchanger volume, line length and flow rate. None of that is in a datasheet and there is no water in the machine. It is therefore not invented - the same refusal as the millilitres-to-grams constant in 9.1, for the same reason: a made-up constant presented as a measurement is worse than an obviously missing one, because it looks like it works.

The second placeholder is the idle threshold, and John pointed out it is likely badly wrong: this machine's group head is solid copper at about 8 lb, so the settling time is set by that mass rather than by the small water volume next to it, and the shipped 2 minutes is probably an order of magnitude short. The same copper mass has a second consequence to check at the same time - a group that cools slowly also reheats the water a flush leaves behind, so the recovery pause may need to be longer and a flush may be less effective than a simple model suggests.

Both are called out by `coolflush on` and by `printStatus()` until `coolflush calibrated 1` is set from real measurements.

**Flush water is not espresso.** The shot clock and the weight and volume baselines are taken in `beginExtraction()`, after the flush and recovery, not when the shot was requested. Baselining earlier would count flush water as delivered volume - triggering the volume target immediately - and would spend part of the hard 60 s shot limit on flushing.

**Verified dry on hardware 2026-11-06**, using a new `groupsim <C>` bench override so the path is testable without a hot boiler. Overriding the group reading is safe in a way overriding the boiler would not be: the group sensor never latches a fault and never gates the heater.

| Test | Result |
|---|---|
| group 90 C (below target) | no flush, "group already at or below target" |
| group 118 C | 4.6 s planned, exactly (118-95) x 200 ms |
| group 200 C | clamped to the 15 s cap, reported as "CLAMPED - coefficient may be wrong" |
| used recently | no flush, "used too recently - group reading still settling" |
| override cleared | returns to the real probe, no flush at room temperature |
| full sequence, group 130 C | cooling flush **7.002 s** against 7.0 s planned, then recovery **3.001 s** against 3.0 s with valve shut AND pump stopped, then pre-infusion, soak and extraction in order |
| shot clock | the shot reported 45.0 s having had 10 s of flush and recovery before it, confirming the baselines are taken at extraction |

`HEATER` and `FILL` stayed flat low throughout every test. `BREW_SW` stayed released, and the console-started shot was not killed by it, which is the 9.4 fix holding.

### 9.6 Config authority across the link: the ESP32 holds the master (2026-11-06)

This closes the question left open in section 4. **Agreed with John 2026-11-06.**

**Upstream already contains the seam.** `ParameterRegistry::addNumericConfigParam` and its siblings do two things: write the JSON config store, which is persisted to the filesystem, and *optionally mirror the value into a `globalVar` pointer* that the control code reads. So upstream's config store is already the master and the globals are already a fast local cache. Splitting across two processors replaces that pointer with a wire. It is not a redesign of upstream's config system.

**Decision:**

- The **ESP32 holds the persistent master**, exactly as upstream does, because it is the board with the filesystem and the web UI.
- The **Teensy keeps its own EEPROM cache** of the control values it needs, so it boots and runs correctly with no head attached. This is a cache, not a second master.
- The **head pushes config on each link handshake.** The Teensy applies each value through the setters that already exist, so every range check and refusal is enforced on the receiving side.
- The **Teensy publishes no schema.** Display names, min/max, help text and sections live in the head's registry, compiled in, because that is what the web UI renders. The Teensy only receives, validates, persists and acknowledges.

**The cost, named rather than discovered:** a setting changed on the Teensy's serial console is overwritten the next time the head connects. That is accepted. It keeps exactly one master and avoids a merge problem, and bench console changes are already temporary - nothing on the Teensy persists them today.

**Rejected alternative:** Teensy as config master with the head as a pure view. A cleaner authority story, and it was considered seriously because "the Teensy is authoritative" is the project's whole stance. But the Teensy has no filesystem for the JSON, every settings page load would become a link round-trip, and upstream's config system assumes a filesystem throughout - so it would mean diverging from upstream on the single largest piece of their code.

**The invariant that must survive this.** No config value may weaken a safety limit. `setMaxBrewMs`, `setMaxFlushMs` and `setMaxCoolingFlushMs` already refuse out-of-range values and cannot be disabled, and `heater::setSetpointC` already refuses anything near the over-temperature cutoff. The link path therefore goes through those setters and must never write the underlying variables directly. A wrong or malicious UI must be unable to do anything a person at the panel could not.

**Verified over the real link 2026-11-06**, with the head's USB attached so its console could push frames:

| sent from the head | ack returned |
|---|---|
| `brew.pre_infusion.time 3.7` | `ok=1,why=ok` - became 3700 ms on the Teensy, so the seconds-to-ms scale factor is right across the wire |
| `brew.by_time.target_time 900` | `ok=0,why=refused` - the 180 s clamp held through the link path |
| `cooling_flush.target_temperature 88` | `ok=1,why=ok` |
| `mqtt.topic coffee/machine` | `ok=0,why=notmine` - correctly ignored, not an error |
| `hardware.sensors.temperature.rtd_reference 430` | `ok=0,why=notmine` - see the gap noted below |

**A bug this found, in the head rather than the config path.** The head's serial console silently discarded characters past 32, so `cfg cooling_flush.target_temperature 88` was truncated mid-key and lost its separating space. The limit was ample for `rot 1` and became wrong the moment config keys arrived - upstream has keys like `hardware.sensors.temperature.rtd_reference` that are 42 characters on their own. Raised to 160, sized against the link frame rather than guessed, and an over-long line now says so instead of being acted on in mangled form. Silent truncation was the actual fault: the command looked like it had been rejected.

**Known gap, not a bug.** `hardware.sensors.temperature.rtd_reference` and the other `hardware.sensors.temperature.*` keys come back as `notmine` because the MAX31865's reference resistor and wire count are compiled in rather than settable. They are reboot-required settings, so adding them means deciding how a reboot-required change travels over the link. Left for when the web UI needs them.

**Front end delivery: a LittleFS image, upstream's way.** Keeps upstream's `frontend/` directory portable unchanged and lets HTML be iterated without a firmware rebuild. The cost is a second upload command, which is precisely the trap the startup-sound comment in `platformio.ini` warns about, so the command belongs in `BENCH.md` rather than in anybody's memory.


### 9.7 On-panel graphs, and what the measurements say about how (2026-11-07)

**Wanted, not yet built.** John wants pressure and temperature plotted on the
machine's own screen, not only in the browser. Recording the groundwork now
because a measurement taken today decides the approach.

**The constraint.** A full repaint of the 480x320 panel measures **139 ms**
(`drawbench` on the head's console, 30 passes). That is dominated by pixel
throughput, not by software: a full-screen clear alone is 480 x 320 x 2 bytes
= 307 KB, which at 40 MHz SPI is ~61 ms before a single glyph is drawn.

So a strip chart **must not full-redraw**. At 139 ms a chart would run at
7 fps and visibly crawl.

**The approach that follows:** an `LGFX_Sprite` covering only the chart area,
scrolled and pushed once per sample. Updating a 440x180 region is roughly an
eighth of the pixels of a full screen, and `pushSprite` is one bus transaction
rather than hundreds of primitives.

**Memory, with real numbers rather than worry.** This panel is RGB565, so:

| buffer | bytes |
|---|---|
| full screen 480x320 | 307 KB |
| chart region 440x180 | 158 KB |

Both fit easily in the board's 8 MB PSRAM. Advice to drop sprites to 4-bit or
8-bit palettes to save memory is sized for boards without PSRAM and does not
apply here. **The real tradeoff is speed, not space:** PSRAM blits more slowly
than internal RAM, and the S3 has only ~320 KB of internal DRAM, so a chart
sprite of this size realistically lives in PSRAM. Measure a `pushSprite` from
PSRAM before committing to a refresh rate.

**Already done and worth keeping:** `lcd.startWrite()` / `endWrite()` now wrap
the whole redraw. Measured 133.6 ms against 139.6 ms without - about 4%. Real
and free, but far from the large win it is sometimes claimed to be, for the
reason above: it removes per-call bus locking, which was never the bottleneck.
`drawbench <n>` on the head's console exists so this stays measurable.

**Where the data comes from.** Telemetry is 5 Hz, which is a sample every
200 ms - ample for a temperature trace and marginal for a pressure profile
during a shot. If shot pressure graphing needs finer resolution, the rate is a
link parameter (`kTelemetryIntervalMs`) and the wire is only ~24% utilised at
the current expedited floor, so there is headroom. Raising it is a protocol
change, not a redesign.

**Not to be adopted:** the suggestion that touch commands become a binary
packet. The protocol is deliberately ASCII so it can be read in a terminal and
typed by hand (`linkProtocol.h`, design rule 2). At these rates the bytes are
free and debuggability is not.

### 9.8 Pressure, flow and pump profiling: WANTED, NOT WRITTEN (2026-11-08)

Recorded here at John's request so it does not get lost. None of this exists
yet, and the panel UI must not imply that it does - tiles are labelled by what
a shot STOPS on, never by a bar figure there is no sensor behind.

**What exists today.** The pump runs at a fixed speed per stage: one percentage
for pre-infusion, another for extraction, set per profile. Flow is measured
(DM60) and volume is a stop condition. Weight is measured (dual HX711) and is a
stop condition. Time is a stop condition and is the backstop that cannot be
disabled. All of that is open loop on the pump: nothing adjusts speed during
the shot in response to a measurement.

**1. Brew pressure sensing.** The sensor is in hand, not installed. Needs an
analogue input, a scaling and plausibility check in the same shape as the RTDs
(stale reading = no reading), a field in telemetry, a readout on the panel's
bottom bar and in `/api/status`, and an over-pressure fault - `FaultCode::
OverPressure` is already declared in `safetyHandler.h` with nothing raising it.
This one is a prerequisite for the two below.

**2. Pressure profiling.** Holding a target pressure, and ramp or decline
curves through the shot, by modulating pump speed. Needs 1 first. The classic
"9 bar then decline" belongs here.

**3. Flow profiling.** Holding a target flow rate rather than a fixed pump
percentage. The DM60 already measures it, so this is the control loop, not the
sensing. Note the pump has NO speed encoder and does not need one: the loop
closes on measured flow and pressure, not on RPM.

**4. Variable-speed pump control generally.** John wants this. The MG304 on its
brushed KAG motor is driven by PWM into his own driver board, so the hardware
supports it; what is missing is any profile structure richer than "one
percentage per stage". A profile would need a sequence of stages with a target
(pressure, flow, or plain percentage) and a duration or transition condition.

**Sequencing.** 1 before 2. 3 can be done before 1 since flow is already
measured, but doing 4 first - the staged-profile structure - would mean 2 and 3
become targets a stage can ask for rather than two separate special cases.
Worth designing 4's data structure before writing either loop.

### 9.9 The panel home screen is a 3x3 tile grid (2026-11-08)

Adapted from the DK Espresso machine shown at MICE 2026, which John brought as
a reference. That is a three-group machine and most of its grid repeats the
same volumetric triggers per group; on a single-group machine those tiles are
free for distinct recipes and utilities instead.

**Two of the reference's nine tiles cannot exist here.** Hot water and steam
are manual valves on this machine - the board has three outputs in total
(heater, group valve, boiler fill) and no switch input for either. Upstream's
steam mode is also a second PID setpoint, which a heat-exchanger boiler held at
one temperature does not have. See 9.2. A tile that cannot do anything is worse
than a missing tile, so they are not on the grid.

The grid, every tile backed by something that works:

| | | |
|---|---|---|
| RISTRETTO (stop by weight) | DOUBLE (stop by weight) | LUNGO (stop by volume) |
| MANUAL (pump until tapped, capped) | FLUSH (purge) | BACKFLUSH (arm, then run) |
| SCALE (tare) | STATUS (temps, link, faults, clear) | SETTINGS (the existing menu) |

**BACKFLUSH** gets a tile because it is a real routine that wants a deliberate
trigger rather than a console command.

**STATUS OPENS THE EXISTING READOUT**, John's suggestion and a better idea than
what was being built. The live page - boiler, target, group, heat bar, weight,
flow, pump, status strip - was the top page before the grid existed, and it
already IS the status panel. A separate Status page would have shown the same
numbers twice. So the readout is kept exactly as it was and simply moved down
the tree: `Page::Home` is now the root, the readout is reached from the tile,
and a shot started from a drink tile switches to it automatically because that
is what you want to be looking at for the next thirty seconds.

(An earlier note here claimed a latched fault needed a serial cable to clear.
That was wrong - CLEAR FAULT has been on Settings > Brew all along.)

**Navigation.** The grid is the root. The readout returns to it on a tap
anywhere except the gear; it has no BACK button because every pixel of that
page is already spoken for - see `drawMainChrome()`. Navigating away does not
affect a running shot: the Teensy owns the shot, and the grid's footer strip
keeps showing boiler temperature and machine state.

**Every tile is a REQUEST.** The head cannot drive an output. A tap sends a
command the Teensy runs through the same gates as the brew switch and the
console - `shot=<n>`, `shotstop`, `flush=0|1`, `backflush=0|1`, `tare=1` - and
it is free to refuse. This is "a touch is a request; the Teensy decides"
working as written, not a new privilege for the display.

**Icons.** The three drink tiles use `assets/coffee-drink.png` reduced to
38x38 and compiled in as `src-esp32s3/iconCup.h` (2888 bytes). RGB565 has no
alpha, so transparency is a magenta colour key the artwork contains none of.
The other six are drawn with primitives - simple shapes, no flash cost, and
they stay crisp if the tile size changes. The reference machine mixes a
pictorial cup with line symbols the same way. Regenerate the cup with:

```bash
python3 -c "
from PIL import Image
im = Image.open('assets/coffee-drink.png').convert('RGBA')
im = im.crop(im.split()[3].getbbox())
print(im.size)"
```

and the full script in this session's history; it trims the transparent
margin, squares the image, resizes to 38x38 and emits the RGB565 array.

**Three profiles now**, one per drink tile, and the encoder selects the same
list by press count - two ways in, one set of recipes. Adding a fourth means
adding a tile too, or the knob can reach a profile the screen cannot show.

### 9.10 The home grid's actions, verified on hardware (2026-11-08)

LA2016 at `conn=1.18` (scan it, the string has now changed four times) on
`HEATER` CH0, `FILL` CH1, `GROUP` CH2, `PUMP_PWM` CH3, `BREW_SW` CH4, each
capture triggered on `CH2=r` so the timing does not depend on catching a tap.
Nothing plumbed; electrical sequences only.

| Tile / control | Measured |
|---|---|
| FLUSH | `GROUP` high and stays high; `PUMP_PWM` 20.000 kHz ramping 20% at 0-100 ms, 47% at 100-250 ms, 98% in the last 100 ms |
| STOP (readout page) | stopped a running flush; controller logged `last stop: panel stop` |
| flush time cap | left to run, ended itself: `last stop: MAXIMUM TIME` |
| BACKFLUSH | ran all five cycles, `last stop: completed`. Captured fills measured 5.0 s on, 10.0 s off, 5.0 s on against a configured 5 x (5 s @ 100% + 10 s) |
| HEATER, FILL | flat low, zero edges, in every capture |

So `shot=`, `flush=`, `backflush=` and `stop=` are all proven over the link,
and the panel's STOP works.

**The pump's 400%/s slew is visible and is not a fault.** A short flush
averages well under its commanded percentage because a quarter of a second of
it is the ramp - the first capture read 59.5% against a commanded 100% purely
because the run was stopped mid-ramp. Measure duty at the END of a window, not
across it.

**sigrok-cli is unstable at these sample counts on this bench.** It segfaulted
on one run and aborted on another, both times AFTER writing usable data, and a
third warned "expecting 556331 samples, got 499885". Check the row count and
analyse what landed rather than trusting the exit status.

**BACKFLUSH arming, fixed and on the tile (2026-11-08).** `startBackflush()`
refuses unless armed, which is correct - it drives the pump against a blind
basket with no relief valve fitted - but the tile used to send `backflush=1`
cold, get refused, and say nothing. Measured: a tap did nothing at all.

The tile now arms on the first tap (`backflusharm=1`) and runs on the second,
and its sub-label says which state it is in. **The armed flag comes from
telemetry (`bfa`), not from what the head last asked for**, because the
controller disarms itself after a run and on reboot - a tile claiming ARMED
when the machine is not would be lying about exactly the wrong control.

The frame was near `kMaxFrame`, so `c1`/`c2` dropped from `%.1f` to `%.0f` to
pay for `bfa`. Calibration factors are counts per gram in the high hundreds or
thousands; the fraction was noise.

**MANUAL verified (2026-11-08).** `GROUP` high from the trigger, pump up at
0.27 s, both off at 9.87 s, ended by the panel's STOP (`last stop: panel
stop`). `HEATER` and `FILL` flat low. The pump appearing a quarter second
after the valve is the 400%/s slew seen from the other direction - at 5 kHz
sampling it does not read solidly high until the duty is near full, which the
ramp reaches at 0.25 s. Second independent sighting of that slew.

**MANUAL is not a toggle, and the label said it was.** It was written to send
`flush=0` on a second tap, but tapping it switches to the readout, so the tile
is not on screen to tap again - "tap to stop" pointed at nothing. It now says
"STOP to end" and sends only `flush=1`; STOP appears on the readout by itself
because the machine is running.

**Brew switch, settled.** `pin 24 raw=HIGH, internal pull-up (switch to GND),
active LOW -> released`. The analyser reading HIGH and the firmware saying
"released" were never in conflict. That is the BREADBOARD configuration; the
schematic has R9 pulling BREW_SW down with the switch going to +3.3 V, so the
fabricated board wants `brewpull x` and active HIGH. No PCB exists yet, so the
current setting is right for today.

### 9.11 A thermal model of the boiler, so the PID can be tuned at a desk (2026-11-08)

**Why.** Development happens at John's desk, with the Teensy, the display and
two PT100 probes reading room temperature. The boiler is in the garage with no
seals and no plumbing. That made the PID the largest untested thing in this
firmware with no route to testing it: a control loop cannot be exercised when
its input never answers its output.

An earlier suggestion of a fixed temperature OFFSET was the wrong idea for
exactly that reason - a number that does not respond to the heater gives you a
plausible screen and no loop. The model responds.

**Shape.** `src/boilerSim.cpp`. 1000 W element into 7400 J/K (about 1.5 kg of
water plus a brass shell), losing 2 W/K to the room - nine minutes from cold
to 95 C, which is what the real machine takes. Deliberately NOT accelerated:
scaling time changes what Tn and Tv mean, so gains tuned against a sped-up
plant would not transfer. `boilersim <C>` jumps the model instead, to skip the
boring part without distorting the dynamics.

**The rise is ADDED to the real probe reading, not substituted**, and that is
the part worth keeping. A synthetic value would throw away the MAX31865's
fault detection, the staleness check and the plausibility window - the paths
that most need to keep working. Adding a delta keeps all of them: idle, the
firmware reads the truth and pulling a probe lead still faults.

It is added BEFORE those checks, so a simulated runaway is indistinguishable
from a real one. **Verified:** `boilersim 145` latched a real `over
temperature` fault and dropped the heater.

**Driven by `heater::onTickCount()`**, the time-proportioning ISR's own record
of how many 10 ms ticks it actually energised for. Differencing it each pass
gives the delivered energy exactly, with no aliasing against the loop rate,
and it measures what the ISR DID rather than what the PID asked for - so a
gate that silently blocks heating shows up as a boiler that will not warm.

The first attempt read `outputs::isOn(Channel::Heater)` and always saw "off":
**the heater does not go through the outputs module**, it is driven straight
at the pin by the ISR. The gates said PERMITTED with 9321 of 10181 ticks
energised while the model sat at ambient.

**Separate build, not a runtime switch.** `[env:teensy41_sim]`, `-D DESK_SIM`.
The machine firmware does not contain this code at all - there is no mode to
leave on by accident, which is the only serious objection to simulating an
input the safety layer depends on. **Verified:** `strings` on
`.pio/build/teensy41/firmware.elf` finds none of it; the sim build has it.

It reuses the existing `heater::setBenchOverride(true)` for the dry-fire
interlock rather than adding a second path through that gate - one way through
is enough, and that one already announces itself.

```bash
pio run -e teensy41_sim -t upload    # desk simulation
pio run -e teensy41 -t upload        # the real firmware
```

**Measured on first run:** 65.6 s of heating gave +8.79 C against 8.78 C
predicted. PID held 100% while far from setpoint.

**Not simulated, and why.** Weight - the load cells are real and a printed
platform was being made the same afternoon, so simulating it would be wasted.
Flow and pressure are the two that cannot be connected at the desk (the flow
sensor is blocked on hose fittings, the I2C pressure sensor is not wired) and
are the obvious next additions: both derive from pump duty and valve state,
and together they would make the whole shot sequence exercisable end to end.

**Wanted, not built: a boiler temperature GRAPH above the readout on the
status page.** John asked for it 2026-11-08, explicitly not urgent. See 9.7
for the measured redraw cost and why a chart needs a sprite. The model makes
this worth having - a graph of a room-temperature probe shows nothing, a graph
of a PID settling shows everything.

### 9.12 The integrator clamp could not hold setpoint (2026-11-08)

Found by the first logged heat-up against the thermal model, which is what it
was built for.

**Symptom.** From cold, the boiler climbed smoothly, never overshot, and
stopped at 93.4 C against a 95.0 setpoint. Duty settled at 14% and the machine
sat there. No fault, no overshoot, nothing in the log - just permanently 1.6 C
low.

**Cause.** `SetIntegratorLimits(0, 55)`, from upstream's `AGGIMAX`. The PID's
output range is 0-1000 (per mille of the 1000 ms window), so the integral term
could contribute at most 5.5% duty.

At setpoint the error is zero, so the proportional term contributes NOTHING
and the integrator alone must supply the entire holding power. This boiler
needs about 144 per mille bare, or 86 insulated, to sit at 95 C in a 20 C
room. Clamped at 55 it cannot, so the loop settles wherever P makes up the
difference.

Predicted from `Kp*err + 55 == 2*(T - ambient)`: **93.61 C**. Measured:
**93.4 to 93.5**. That is the confirmation, not a hunch.

**Fix.** `kDefaultIntegratorMax` and `AGGIMAX` to **200**. Raised rather than
removed - the clamp still stops the integrator saturating during the
eight-minute climb from cold, which would overshoot badly. 200 covers the
holding duty with margin for a cold room.

**Result, same test:** reached 95.0 at 700 s, peaked 95.8 (+0.8), then unwound
through 95.5 to 95.0 with duty bottoming at 4% and rising again. Droop gone,
overshoot small and self-correcting.

**This would have shipped.** On the real machine it is a boiler that sits a
degree and a half low for ever, with nothing to point at: no fault, no
overshoot, no log entry. The probe, the offset, the element and the insulation
would all have been suspected first.

**Boiler insulation folded into the model at the same time.** 10 mm melamine
foam, so the loss coefficient went 2.0 -> 1.2 W/K. Derivation is in
`boilerSim.h`; the short version is that the foam itself only accounts for
about 0.35 W/K and the uninsulated bosses, fittings and the group head carry
the rest. Both model constants are estimates and both are directly measurable
on the finished machine - heat-up rate gives the heat capacity, holding power
over temperature difference gives the loss.

### 9.13 PID gains, measured and banked (2026-11-08)

**Kp 62, Tn 52, Tv 11.5, integrator max 200.** The first three are upstream's;
only the clamp changed, and why is in 9.12.

Measured against the thermal model, insulated (1.2 W/K, 7400 J/K, 1000 W).

**Cold start, 24 C to setpoint:**

| | |
|---|---|
| reached 95.0 | 700 s (11.7 min) |
| peak | 95.8 at 789 s, +0.8 |
| then | 95.5, 95.0, duty bottoming at 4% and rising |

**Settling from 90 C, which is the approach a machine makes after a shot:**

| | |
|---|---|
| peak | 95.80, +0.80 |
| ring | one dip to 94.80, back to 94.90 - damped 0.8 then 0.2 |
| last three samples | 94.90, 94.80, 94.90 - holding within 0.1 C |
| final duty | 8.7-9.2% against 8.6% theoretical holding |

Duty landing on the holding figure is the tell that it has genuinely settled
rather than still drifting, with the integrator slowly closing the last 0.1 C.

**Good enough, and deliberately not refined further.** Holding to a tenth of a
degree is better than this machine needs - the shot is pulled from a settled
boiler, and a tenth is well inside what the water does crossing the group. The
remaining 0.8 C overshoot on approach could be cut with
proportional-on-measurement (`pid.use_ponm`, registered and off) or more Tv,
both of which trade settling time, on a machine that takes twelve minutes to
warm up anyway.

**What the first real heat-up should do.** Not a search - the response shape is
known. Measure the two model constants directly and correct them, then expect
a small adjustment rather than a retune:

* heat capacity, from the heat-up rate at 100% duty: `C = P / (dT/dt)`
* loss, from the holding power at setpoint: `k = P_hold / (T - T_room)`

If those come out near 7400 J/K and 1.2 W/K these gains stand as they are.

### 9.14 Load shed is gone, not disabled (2026-11-09)

**Decision:** remove the heater/pump load-shed interlock from the firmware
entirely. John's instruction, and it supersedes the 2026-11-08 position of
keeping a disabled mechanism.

**Why it existed.** The original element measured 7 ohms, 2000 W, 17 A - over a
15 A branch circuit on its own. With a pump on the same circuit the machine
could not run both, so the firmware refused to.

**Why it does not any more.** The machine is specified to run ONE leg of the
dual element: about 1000 W, 8.3 A. The pump is a 24 V DC motor off its own
supply, drawing well under an amp from the mains side. Together they are
nowhere near a 120 V outlet's limit, so the interlock had nothing to protect.

**Why removing beat keeping it switched off.** The 2026-11-08 argument for
keeping it was that it was already written and tested, so re-enabling it would
be one registration rather than a rewrite. Against that: it was two gates in
the two most safety-critical modules in the firmware, in the `setSpeed()` refusal
path and in the heater's permit chain, that could never fire. A gate that cannot
trip is a gate nobody notices breaking, and both of those chains are read by
anyone trying to work out why the heater or the pump will not run. If the element
specification ever changes back, the interlock is eleven lines and it is in this
repository's history.

**What was removed.** `pump::setLoadShed()` and `pump::loadShed()`; the
`loadShedEnabled` flag and both gates in `pumpHandler.cpp`; the gate in
`heaterHandler.cpp`'s `gatesAllow()`; the `pumpshed` console command; the
`pump.load_shed` config key on both sides, including its entry in the Teensy's
`configStore` table; and the `#include "pumpHandler.h"` in `heaterHandler.cpp`
and `#include "hardware/outputs.h"` in `pumpHandler.cpp`, both of which existed
only for load shed. The heater's gate list is now seven, not eight.

**One side effect worth knowing.** `configStore`'s stored values are keyed by
table index and protected by a hash of every key name, so dropping a key
invalidates the Teensy's EEPROM cache and the next boot logs that it is using
compiled defaults. That is the mechanism working as designed, not a fault, and
the head re-pushes the whole config at the next handshake anyway. The scale
calibration is NOT affected - it lives in its own EEPROM block at address 64.

### 9.17 Flow sensing: which meter, where it can go, and the autofill coupling (2026-11-10)

Raised by John after reading the DM60 data sheet. Nothing here is built yet;
it is recorded because the reasoning is not recoverable from the parts list.

#### The DM60 is the wrong shape for this machine, and still probably the right choice

Its data sheet is explicit: "specially designed for coffee machines that use
**vibratory pumps**. The device is installed between the water tank and the
vibration pump (**on the suction side**)." This machine is direct-plumbed with
a gear pump, so that topology does not exist here. Three limits follow:

| | DM60 Nano | this machine |
|---|---|---|
| pressure | -1 to 5 bar | 8-9 bar after the pump |
| temperature | 0 to 65 C | 90 C+ downstream of the exchanger |
| linear flow | 0.05 to 0.40 l/min | extraction 1-2.5 ml/s (in range); flush and autofill exceed it |

So it can only live on the **cold inlet, downstream of the pressure
regulator**. Hose must be clamped (the sheet warns above 0.5 bar), it must be
kept away from the pump motor's magnetic field, and the sub-10-micron filter
stops being optional because the nozzle is 1.1 mm.

**Why keep it anyway: RESOLUTION, and that is the whole argument.**

* DM60: **49,162 pulses/litre** - about 49 per millilitre. At 2 ml/s that is
  ~98 Hz, ten pulses per tenth of a second. Enough to derive an instantaneous
  flow rate and close a loop on it.
* A typical Gicar volumetric meter: **roughly 2,000 pulses/litre**, two per
  millilitre, ~4 Hz at the same flow. Fine for counting out a dose to within
  half a millilitre. Useless for profiling - the loop would chase
  quantisation noise.

The two are not alternatives. The Gicar doses; the DM60 doses AND profiles.
And the trade is structural rather than bad luck: resolution comes from a tiny
fast turbine behind a 1.1 mm nozzle, which is inherently a small-bore,
low-pressure, plastic design. Brass meters rated for mains pressure have
bigger, slower turbines and far fewer pulses.

**The cost of choosing it:** the inlet pressure regulator becomes REQUIRED
EQUIPMENT, not a nicety. A polypropylene body rated to 5 bar sits on a mains
line that in a US house can reach 5.5 bar. A drifting regulator or a
water-hammer spike puts it over its limit inside the cabinet.

**The machine's original Gicar is in hand** (the one with the pulse LED - the
manual describes exactly that: "the flashing of the LED indicates that the
electrical impulse has been sent"). Its pulse rate is NOT in either manual and
would have to be measured with `flowcal` gravimetrically. Worth testing dry as
a signal-chain check: wire it up and spin the turbine, and the counter, the
pull-up and the interrupt path are all proven before water exists. **Do not
blow air through the DM60** - its sheet says that destroys it. The brass Gicar
does not care.

Firmware is sensor-agnostic: the rate is a config value (49162 today, the
datasheet figure) and `flowcal` recalibrates it. Swapping meters is a number.

#### THE AUTOFILL COUPLING, which is the part that is easy to miss

From the machine's own manual, section 10: the pump raises pressure "for the
delivery of the coffee **and for automatic filling of the boiler**".

**So the boiler fill is fed from the PUMP OUTPUT, not from the inlet.** The
split is after the pump. A meter on the cold inlet therefore measures
everything the pump moves - brew water and boiler fill water together. There
is no tee to get past.

**Consequence, and it is a firmware rule rather than a plumbing one:**

> **AUTOFILL MUST NOT RUN DURING A SHOT.**

Otherwise the boiler can start filling mid-extraction and the meter counts it
as delivered volume, stopping the shot early on a volume target - and the same
error corrupts any flow-derived weight estimate (9.1) and any flow profiling
loop (9.8).

This is good practice anyway: filling the boiler mid-shot robs pressure from
the group. But it moves from hygiene to REQUIRED, because without it
brew-by-volume reports a number that is wrong rather than merely imprecise.

The autofill handler is not written yet, so this is a constraint to design in
rather than a bug to retrofit. Note it interacts with the anti-flood timeout
already on the safety checklist: a fill deferred until the shot ends must not
have its timeout running while it waits.

### 9.16 WANTED, NOT BUILT: show how long a running operation has left

John's, 2026-11-10, after a backflush ran for 75 seconds and he had no way to
know whether it was stuck. Explicitly not urgent.

**The gap.** The panel shows STOP while something runs, and nothing else. No
elapsed time, no remaining time, no progress. That is the same omission three
times over - a shot (60 s backstop), a manual flush (20 s cap) and a backflush
(5 cycles x 15 s = 75 s). A backflush is the worst of them because it visibly
stops and starts between cycles, so a person reasonably wonders whether it has
finished or jammed.

**Shape, if it gets built.** A countdown is the obvious thing, but for
backflush the cycle matters as much as the clock: "cycle 2 of 5, 48 s left"
tells you it is working, where "48" alone does not. A shot wants elapsed
rather than remaining, because the 60 s figure is a SAFETY BACKSTOP and not a
target - showing a shot counting down to it would present a limit as a plan.

**Check before writing it** whether the controller already reports what is
needed. The shot timer exists; cycle number and stage deadlines may not cross
the link yet, and adding them is a telemetry change, not a UI one.

### 9.15 The head's UI moves to LVGL, edited in SquareLine (2026-11-10)

**Decision, John's, 2026-11-10:** the display head's UI is rebuilt on LVGL,
graphs included. The ~2000 lines of hand-drawn LovyanGFX in
`src-esp32s3/display.cpp` are replaced.

**The screens are written in code, by firmware, as before.** SquareLine Studio
is installed and the project is structured so an export can drop in, but it is
an OPTION for nudging a layout later - not how the screens get made. This was
got wrong first: the editor was read as John wanting to take on the design
work, and he corrected it twice. His words: he CAN edit a design if he needs
to, not because he wants to. Designing the panel is firmware's job. Do not
hand him SquareLine instructions as a step in the work.

**So what decided it is the measurements and the structure**, not the editor.
LVGL repaints a page in 97 ms against 139 ms, a widget knows its own touch
area so the hand-maintained table of hit rectangles disappears, and the
layout-arithmetic traps recorded in 9.x stop being possible. The editor is a
door left open, and `uiContract.h` is what keeps it open for the price of one
header.

**A correction worth recording, because it was argued from.** The case for
LVGL was first made on the strength of its chart widget, citing this brief's
own 9.7 and the CLAUDE.md line saying on-panel graphs were unwritten. That
line was stale: the **boiler chart already existed**, hand-built, in a PSRAM
sprite with auto-scaling, a dashed setpoint line and genuine gaps for
unhealthy samples. So the chart was never the argument. Read `display.cpp`
before repeating a claim from a status note.

#### What was measured, not assumed

A single page - `Page::Main`, the live readout plus the boiler chart - was
rebuilt on LVGL as `[env:esp32s3_lvgl]` / `src-esp32s3-lvgl/`, deliberately
the one page that already existed hand-drawn, so the comparison is a side by
side rather than a demo. `link.cpp`, `net.cpp` and `headConfig.cpp` are
compiled in unchanged, so WiFi, the web server, the registry and the Teensy
link are all running underneath the measurement.

| | hand-drawn | LVGL 9.6 |
|---|---|---|
| full page repaint | 139 ms (134 ms wrapped) | **101.5 ms** |
| one chart sample | no equivalent benchmark exists | **11.2 ms** |
| static RAM | 58,012 | 119,576 |
| flash | 1,547,209 | 1,609,145 |
| free internal heap, everything running | not measured | 91,024 (largest block 81,908) |
| PSRAM in use | 78 KB (the chart sprite) | ~0 |

**LVGL is about a quarter FASTER at a full repaint**, which is the opposite of
the usual assumption and was the surprise of the exercise. The mechanism is
mundane: the hand-drawn page issues hundreds of individual primitives, each
setting up its own bus transaction, while LVGL composes a rectangle in RAM and
DMAs it in one go.

**It is not faster at the chart and nothing could be by much.** 230x84 RGB565
is 38,640 bytes, which at 40 MHz is 7.7 ms of wire time before anything is
drawn. LVGL's 11.2 ms is that floor plus 3.4 ms of work. The hand-drawn chart
pays the same floor. The flash figures are NOT like for like either - the
trial omits `display.cpp`, `audio.cpp` and `es8311.cpp` - so treat +62 KB as a
lower bound on LVGL's cost, and note that 4.9 MB of flash is free regardless.

**Memory is the real price, and it is knowable:** a 64 KB LVGL working pool of
which this page uses 15 KB, plus 61 KB of draw buffers deliberately placed in
internal RAM rather than PSRAM (9.7 measured that PSRAM blits more slowly, and
this buffer is touched every repaint). About 125 KB of 320 KB. The 8 MB PSRAM
is an escape hatch if a later page needs more, at a speed cost.

#### Two traps found by running it

**1. Byte order, which does not look like a colour fault.** RGB565 is two
bytes a pixel; the ST7796 wants the high byte first and an ESP32 stores a
uint16_t low byte first. Handing LovyanGFX a raw `uint16_t*` pushes memory
order, so every colour came out wrong - and because anti-aliased text picked
up coloured fringes, the first thing anyone says is "the fonts look blurry",
not "the colours are wrong". Fixed by rendering in the panel's order from the
start, via `lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565_SWAPPED)`.

That is a RUNTIME call on purpose. 9.6 has an `lv_conf.h` key for it
(`LV_COLOR_FORMAT_DEFAULT`) and 9.5 has none; the runtime call exists in both.
The LVGL version moved twice while settling what SquareLine wanted, and this
form survived both moves. **Do not also swap in the flush callback** - that
puts the colours back.

**2. A long repaint starves the link.** The head logged bad checksums during
forced full repaints and none when idle. At 115200 baud the UART holds about
11 ms of data and a full repaint is 100 ms, so frames are lost. Page changes
only, so not a live problem - but **the UI must never block for 100 ms during
a shot**, and the hand-drawn build has the same exposure at 139 ms with nobody
having looked.

#### The version pin, which is a rule rather than a number

**The LVGL version in `platformio.ini` must match the LVGL version selected in
the SquareLine project**, and the two change in the same commit. An export is
generated C calling LVGL's API directly, so the editor's idea of that API and
the library's have to agree. The dependency is pinned exactly, not with a
caret: a floating minor version drifts away from whatever the editor exported
against.

SquareLine Studio **1.6.3** bundles runtimes for 8.3.11, 9.1.0, 9.2.2, 9.3,
9.5 and 9.6 - read from `lvgl/` inside its install folder, which beats its
release notes, and which contradicted both a web search (said 1.6.2 / 9.5) and
a Google AI summary (said LCD Ultra Pro supports 9.5, having merged it with
the unrelated official LVGL Pro). **Both sides are on 9.6.**

Not chosen, with reasons: **LCD Ultra Pro** is LVGL 8.4 by its own docs, and
its value is exporting board driver files for 100+ boards, of which the
LCDwiki/Hosyond ST7796S is not one. **LVGL Pro**, the official editor, is
worth revisiting when its announced v2.0 ships - first-party tooling cannot
drift from the library, and it stores UIs as XML, which would put screen
layouts in git as readable files instead of generated C.

#### The structure, and the one rule that makes it work

**SquareLine regenerates its export wholesale.** Anything hand-written living
among those files is destroyed on the next export. So the split is not a
preference, it is the whole design:

| | |
|---|---|
| `src-esp32s3/uiScreens.cpp` | **THE UI.** Hand-written layout: what exists, where, what size and colour. |
| `src-esp32s3/uiBind.cpp` | **HAND-WRITTEN.** Telemetry into widgets; widget events out as link commands. Every honesty rule lives here. |
| `src-esp32s3/uiLvgl.cpp` | **HAND-WRITTEN.** LVGL bring-up and the panel glue. Implements the existing `display::` interface, which is why `main.cpp` and `net.cpp` needed no changes at all. |
| `src-esp32s3/uiContract.h` | The widget names. The seam that lets a SquareLine export replace `uiScreens.cpp` one day without touching anything else. |
| `src-esp32s3/ui/` | Empty. Where a SquareLine export would land. GENERATED, never hand-edited. |

Export **UI files only**, not a board template: `display.h` already describes
this panel correctly and is measured, and SquareLine does not know this board
anyway.

**Everything that makes this machine's UI honest is logic, not layout, and
lives in the hand-written half** - which is precisely why a re-export cannot
lose it:

- A stale or faulty reading shows `--.-`, never the last good value.
- A chart sample taken while the sensor was unhealthy is a GAP.
  `LV_CHART_POINT_NONE` is skipped when drawing, so the framework supports
  this rule rather than fighting it.
- An uncalibrated scale is never labelled "grams".
- The backflush tile reads its armed state from telemetry (`bfa`), never from
  what the head last asked for. See 9.10.
- Every tile is a REQUEST. The head cannot drive an output; the Teensy decides
  and is free to refuse. See 9.9.

**What becomes John's job rather than firmware's:** the palette, the fonts and
the layout move into the editor.

#### Phasing

1. The readout page plus the chart. Already exists as the trial, so it is a
   port into the real build rather than new work.
2. The home grid (9.9), with its tile actions re-bound.
3. The setup menu and its sub-pages. **Keep the sub-page structure** - 9.x
   records that cramming controls onto one page failed four times and that
   making SETUP a menu is what fixed it.
4. The About page.
5. Delete `display.cpp` only once every page has a replacement, and keep the
   hand-drawn env buildable until then.

The trial build is a measuring instrument and is disposable. It keeps its own
`main.cpp` and `lv_conf.h` and is not the shape the real UI should take; do
not grow it into the product.
