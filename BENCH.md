# Bench and development notes

The practical details. Small things that waste an hour if you do not know them.

## Building and flashing

`pio` is NOT on PATH. Use the full path:

```
/home/john/.platformio/penv/bin/pio run -e teensy41
```

Three environments:

| env | what | sources |
|-----|------|---------|
| `teensy41` | control firmware, the active work | `src/` |
| `esp32s3` | display head | `src-esp32s3/` |
| `esp32_usb` | upstream ESP32 firmware, frozen porting reference | `src/` + `src-esp32-legacy/` |

`build_src_filter` in `platformio.ini` is the **port ledger**. Each phase adds the
files it brings over, so the Teensy build cannot silently pick up ESP32-era code
sitting in `src/`. Keep the `esp32_usb` exclusion list in step with the
`teensy41` inclusion list, or a Teensy-only file drifts into that build.

**Teensy uploads fail transiently** maybe one time in five, with no error text.
Just run it again. A real failure shows a compiler or linker error.

## Serial ports

Auto-detect picks the **wrong** device on this bench - there is a Prolific
PL2303 on `/dev/ttyUSB0` that wins. Both envs pin the port by USB VID:PID:

* Teensy `hwgrep://16C0:0483`, usually `/dev/serial/by-id/*Teensy*`
* ESP32-S3 `hwgrep://303A:1001`, usually `/dev/ttyACM1`

`cat` does not assert DTR, so a Teensy will not transmit to it. Use a real
terminal, or Python with `dtr=True`.

Scripts that drive the console should set `dsrdtr=False` then `s.dtr=True`.

## Recovering a bricked Teensy

Holding the program button for ~15 s does a **full erase**. The board then
enumerates as `1fc9:0135 SE Blank RT Family` (NXP ROM recovery) and will not
run. To recover: **press the program button briefly**, confirm it comes back as
`16c0:0478` (Teensy bootloader), then upload normally.

A full erase also wipes EEPROM, so the stored scale calibration is lost.

## The logic analyser (separate machine)

A Kingst LA2016 lives on a **different PC**, reachable over SSH as `bench`
(key-based, no password).

```
ssh bench 'sigrok-cli --scan'
```

**SCAN for the connection string, do not trust a stored one.** It has already
changed once: notes said `conn=1.9`, reality was `conn=1.16`.

Capture and analyse on the bench machine, bring back only the summary - a CSV of
a few hundred thousand samples is large:

```
ssh bench 'sigrok-cli -d kingst-la2016:conn=1.16 --config samplerate=10m \
  --samples 20000 -O csv > /tmp/c.csv; python3 - <<PY
... parse and report ...
PY'
```

CSV format: channel names are in a `; Channels (16/18): CH0, CH1, ...` comment,
then a `logic,logic,...` header row, then `0,1,0,...` data rows.

**The LA2016 also has two PWM OUTPUTS** (`PWM1`, `PWM2`), settable with
`-g PWM1 --config enabled=on:output_frequency=100:output_duty_cycle=50 --set`.
They are 3.3 V, 50 ohm, 0.1 Hz to 20 MHz, so they can inject synthetic signals
safely.

**They can also break things.** A PWM output left wired to a Teensy pin idles
LOW and shorts that pin to ground, which cost most of a session chasing a flow
sensor that was fine. If a pin reads stuck low, check nothing else is on it.

## Git

| remote | url | push |
|--------|-----|------|
| `origin` | `git@github.com:jrsphoto/clevercoffee-teensy.git` | yes |
| `esp32-fork` | the ESP32 fork | **no_push** |
| `upstream` | rancilio-pid/clevercoffee | **no_push** |

The no_push guards are deliberate: this used to be a second clone of the ESP32
fork, so Teensy work would have landed in the repo holding the firmware that
runs the machine.

Working branch: `claude/teensy-pinout-review-dec021`.

GitHub Actions inherited from upstream are disabled in `.github/workflows-disabled/`
because they failed on every push and sent John an email each time.

**Commit and push only when asked.**

## KiCad

**This board:**

```
/home/john/projects/Astoria SME 1N JUN Espresso Machine/clevercoffee-hardware/CleverCoffee_Teensy41_Dev_SMD/
```

**Check the folder name every time.** `clevercoffee-hardware/` holds SIX KiCad
projects and five of them are the old ESP32 boards:

```
CleverCoffee_Teensy41_Dev_SMD    <-- THIS ONE
CleverCoffee_ESP32_Dev           CleverCoffee_ESP32_Dev_SMD
CleverCoffee_ESP32_minimal       CleverCoffee_ESP32_minimal_SMD
ABP2_Breakoutboard               (the brew pressure sensor breakout)
```

`CleverCoffee_ESP32_Dev_SMD` is the board currently running the machine. It is
NOT the one being worked on, and its pin numbers are different - see the
`#else` branch in `src/hardware/pinmapping.h`.

The Teensy project has **two sheets**:

* `CleverCoffee_Teensy41_Dev_SMD.kicad_sch` - the Teensy itself, SPI, I2C,
  flow, the link header, the pump PWM header
* `relays.kicad_sch` - the ULN2003, the brew switch, the water probe, and the
  sensor connectors

A global label appears in only one sheet, so grep both before concluding a net
is dangling.

* **Read** `.kicad_sch` to answer pin questions. Never eyeball the PNG render.
* **Never edit** `.kicad_sch` / `.kicad_pcb`. Give exact click-by-click steps.
* KiCad is often open; check for `~*.lck`. Reading is always safe.
* **John's edits are not on disk until he saves.** If a change seems missing,
  ask him to save before concluding anything.

Three faults were found by reading the files that a visual review missed: a net
split in two by a label name mismatch, a resistor of the wrong value, and a 5 V
pin beside a 3.3 V-only input. Check **pin assignment**, **net connectivity**,
and **component values against the connected part's datasheet** - all three.

## Bring-up console

The Teensy firmware has an extensive serial console. `help` lists everything.
The ones worth knowing:

```
status temp rtd failsensor          sensors and fault injection
flow flowwatch flowfast flowcal     flow sensor, including a MHz pin sampler
scale scaletare scalecal scalediag  load cells
brew brewwatch brewpull brewsense   brew switch
pump <0-100> pumpshed               pump PWM and load shed
heater [on|off] setpoint pid        boiler PID
drybench <0|1>                      BYPASS dry-fire interlock, no element fitted
permittest ulntest ulnduty          safety and output-stage tests
overtemp <C> overtempreset          lower the over-temp cutoff to test it
shot [start|stop] shotset profile   brew sequence and profiles
i2c enc link update clear           bus scan, encoder, link, update mode
```

The display head has its own console: `rot inv bl redraw play vol tone amp regs
dpin audio cmd`.

## Testing style that works here

**One physical action per exchange.** Ask for one change with nothing running,
wait for confirmation, then do all the measuring. Never ask John to hold a meter
while a timed script runs - he has two hands.

**Prefer a firmware diagnostic over asking for a meter reading.** The megahertz
pin sampler settled in one command what several meter requests had not.

**A diagnostic cannot validate itself.** A duty measurement that sampled at
main-loop rate read 13% high and was only caught because the logic analyser was
already clipped to that pin.

**Always assert in scripted edits.** A `str.replace` with no assertion silently
matched nothing and a fix was reported that had not happened.
