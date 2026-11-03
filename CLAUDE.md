# CleverCoffee → Teensy 4.1 + ESP32-S3 (Argenta SME build)

Splitting CleverCoffee into two firmwares: a **Teensy 4.1** running all
real-time control, physical I/O and safety, and an **ESP32-S3 touch display**
running WiFi, the web UI and the screen, linked by one UART with the Teensy
authoritative.

**Safety-critical mains work** - a ~1500 W element on a boiler. Accuracy and
testing matter more than speed. Verify against the actual code and datasheets;
do not assert from memory.

## Read these

* `BENCH.md` - build/flash, serial ports, the logic analyser on the `bench` PC,
  git remotes, KiCad rules, console commands, how to test here. **Read it before
  touching hardware.**
* `README.md` - why this architecture exists, and the reset-separation decision.
* `TEENSY-PORT-BRIEF.md` - the phased plan and agreed design decisions.
* `teensy-pinout-review.md` - authoritative for pins, plus the schematic faults
  found so far and why earlier reviews missed them.
* `../CLAUDE.md` - the parent machine project.

## Prime directive: stay close to upstream

Read upstream's implementation **before** writing a replacement, not after.
Keep the same shape: hardware behind small interfaces, features in `xHandler`
modules called from `main.cpp`, the same file and class names where they still
fit. Diverge only with a reason, and write the reason in the file.

This was got wrong once: the brew sequence was invented before reading
`brewHandler.h`, producing gratuitous differences that then had to be undone.

Where a name collides with an ESP32-era file still needed by the frozen
reference build, the new file takes a different name and says why in its header
(`weightHandler`, `shotHandler`). They can take upstream's names in phase 6.

## Who owns what

**Teensy 4.1**: heater PID, brew sequence, pump PWM, valves, both RTDs, flow,
scales, encoder, brew switch, and **all safety**.

**ESP32-S3** (LCDwiki 4.0", ST7796S, capacitive touch): WiFi, web, MQTT, the
screen. **No actuators, no safety.** A touch is a request; the Teensy decides.

**The link**: one UART, framed and checksummed, deliberately non-critical. Pull
it and the machine finishes the shot. Nothing in the control path asks whether
the display is present.

## Safety stance

No external fail-safe relays. The firmware is the only active line of defence.

* Fail safe on sensor fault, implausible reading, watchdog timeout, boot
  self-check failure, over-temperature, low water.
* Outputs default off. Everything off at boot until self-checks pass.
* **Dry-fire interlock blocks heating whenever water is unknown.** The probe
  circuit does not exist yet, so the firmware refuses to heat. That is correct;
  do not "fix" it by defaulting it true.
* Load shed: heater and pump never run together.

Two patterns used throughout, both worth preserving:

**Permits and commands EXPIRE.** The heater ISR refuses a permit older than
500 ms; pump speed commands expire after 1.5 s. A flag set by a loop that has
since hung is a lie, so staying on requires continuous proof of life and the
safe state is what you get by doing nothing.

**Stale readings are treated as no reading**, never as the last good value, and
staleness is measured from the last *successful* read so a dead sensor cannot
refresh its own freshness.

**Brew-by-weight always has a time backstop.** Upstream's time stop is optional;
here it cannot be disabled. It is the only stop condition with no sensor behind
it.

## Status

Verified on hardware: safety layer and watchdog; both PT100 RTDs (**4-wire,
never 3-wire**); DM60 flow; DuPPa encoder at 0x04; display, touch and link;
brew switch; dual HX711; pump PWM; boiler PID and the ULN2003 drive; brew
sequence with encoder profiles.

Blocked on parts: brew pressure sensor, water-level probe circuit, load cell
bracket, silicone tubing for the duty-to-flow sweep.

Not written yet: hot water, steam, backflush, manual flush, the web UI and
config sync on the head, and FlasherX for updates over the link.

**The heater has never been connected to mains.** Every interlock was proven
with no element attached. The only unproven one is the water probe, because its
circuit does not exist.

## How to work here

* Propose a plan and get agreement before large refactors.
* Test safety paths hard, and make them testable: `permittest`, `overtemp`,
  `failsensor`, `ulntest` all exist so a mechanism can be fired on demand
  rather than reasoned about. A safety mechanism tested once is one nobody will
  notice breaking.
* Report failures with the actual output. When unsure, measure or ask.
* Commit and push only when asked.
