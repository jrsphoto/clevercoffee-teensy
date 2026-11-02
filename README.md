# CleverCoffee, ported to Teensy 4.1 + ESP32-S3

A fork of [CleverCoffee](https://github.com/rancilio-pid/clevercoffee) that splits
the firmware across two processors, built for a C.M.A. Laurentis "Astoria Argenta
SME" 1N Junior: a 120 V, single group, heat exchanger machine with a rotary pump.

Upstream CleverCoffee runs on a single ESP32 and is the right answer for most
builds. This fork exists because this particular machine asks for more, and the
reasons are worth stating plainly since "two microcontrollers" looks like
overkill at first glance.

## Why a Teensy, and why two processors

The short version is that this machine is being asked to do more than a standard
CleverCoffee build, and the thing controlling a 1500 W element should not also be
serving web pages.

On a normal single-ESP32 build, the same chip runs WiFi, the web UI, MQTT, the
display, the PID and all the safety logic. That works, and thousands of machines
prove it. But everything shares one loop, so a browser hitting the web interface
or the WiFi stack doing its own housekeeping competes with the control timing.
Here the Teensy 4.1 does nothing but real time control: PID, pump, valves,
sensors, and every interlock. The ESP32-S3 display board does WiFi, the web
interface and the screen, and talks to the Teensy over a single serial link. The
Teensy is in charge; the display is a terminal. The display can be unplugged mid
shot and the machine carries on and finishes the shot normally, which has been
tested rather than assumed.

The Teensy specifically because of how much real time I/O runs at once: two PT100
temperature probes on SPI, an I2C encoder, two load cells for brew by weight, a
flow sensor counting pulses, hardware PWM driving a variable speed gear pump, and
the serial link. It is a 600 MHz chip with hardware floating point and a hardware
watchdog that cannot be switched off once started, which matters because this
build has no external fail safe relay chain. The firmware is the only active line
of defence, so that firmware should be boring, predictable, and not sharing a
processor with a radio.

Is it overkill for a straightforward PID retrofit? Honestly, yes. If you want brew
temperature control and a web UI, a single ESP32 is the right answer. It stops
being overkill once you add variable speed pump control closing the loop on
pressure and flow, brew by weight, and boiler autofill, and once you decide the
microcontroller is the only safety there is. The cost is real: CleverCoffee is
ESP32 native, so it does not drop in. It has to be split into two firmwares with
a protocol between them, which is a lot more work than flashing an ESP32.

The precedent is [OnStep](https://github.com/hjd1964/OnStep), which pairs a Teensy
doing telescope motion control with a separate ESP32 web head over serial.

## How the work is split

**Teensy 4.1** owns all real time control and all physical I/O: heater PID, brew,
autofill, pump PWM, valves, both RTDs, brew pressure, flow, the HX711 scales, the
encoder and buttons, and every safety interlock.

**ESP32-S3 display module** (LCDwiki 4.0 inch, ST7796S with capacitive touch) owns
WiFi, the web server, MQTT and the colour screen. It holds no actuators and no
safety. A touch on its screen is a request sent to the Teensy, which decides.

**The link** is one UART at 115200 with a framed, checksummed ASCII protocol. It
is deliberately non critical: if it drops, the Teensy runs the machine on its own.
Nothing in the control path asks whether the display is present.

## Safety stance

There are no external fail safe relays. The Teensy is the only active line of
defence, so the safety logic is first class rather than an afterthought:

* Fail to a safe state on sensor fault, implausible reading, watchdog timeout,
  boot self check failure, over temperature or loss of the water level signal.
* Hardware watchdog that resets into the safe state.
* Dry fire interlock: no heater power unless the water probe confirms water.
  With no probe fitted the firmware refuses to heat, which is the correct
  behaviour and not a bug.
* Load shed so the heater and pump never run together on a 15 A circuit.
* Outputs default off; everything stays off at boot until self checks pass.

Two mechanisms are worth calling out because they are not obvious:

**Permits expire.** The heater ISR cannot evaluate the safety gates, but it does
not blindly trust a flag either, because a flag set by a loop that has since hung
is a lie. The main loop stamps the permit and the ISR refuses to energise on a
stamp older than 500 ms. Verified by deliberately hanging the loop: the heater
dropped at exactly 500 ms. The pump uses the same idea, with speed commands that
expire if not re-asserted.

**The output stage is checked every tick.** The ULN2003 output is monitored by a
Teensy pin, and each timer tick verifies the output followed what was commanded.
A dead driver channel shows up as a mismatch count rather than as a boiler that
quietly stops heating.

A passive one shot thermal cutoff stays inline in the heater line as the only non
firmware backstop. The firmware neither knows nor relies on it.

## Status

Working and verified on real hardware:

| Area | State |
|------|-------|
| Safety layer, watchdog, fault latching, bench console | verified |
| Two PT100 RTDs (MAX31865, 4 wire) | verified |
| Digmesa DM60 flow sensor | verified |
| DuPPa I2C encoder | verified |
| ESP32-S3 display, touch, and the serial link | verified |
| Brew switch, debounced, with long press | verified |
| Dual HX711 load cells | verified, calibration pending a better bracket |
| Pump PWM into a DRV8871 | verified on the wire to 0.4% |
| Boiler PID and the ULN2003 heater drive | verified, output still gated |

Not done yet: brew state machine, the web UI and config sync on the display head,
and firmware update over the link. Waiting on parts: brew pressure sensor, the
water level probe circuit, and a thermal cutoff.

The heater has never been connected to mains. Every interlock above was proven
with no element attached, which is the only sensible order.

## Building

PlatformIO, three environments:

```
pio run -e teensy41     # the control firmware, src/
pio run -e esp32s3      # the display head, src-esp32s3/
pio run -e esp32_usb    # upstream's ESP32 firmware, kept as a porting reference
```

`build_src_filter` in `platformio.ini` is the running ledger of the port. Each
phase adds the files it brings over, so the Teensy build cannot silently pick up
ESP32 era code that happens to still be sitting in `src/`.

The Teensy firmware has an extensive serial bring up console. `help` lists it.

## Relationship to upstream

This fork tracks [rancilio-pid/clevercoffee](https://github.com/rancilio-pid/clevercoffee)
and tries to stay readable against it: same modular shape, same handler names,
same config system, so upstream changes remain cross referenceable. Where this
fork diverges it is for a stated reason, usually recorded in the file header.

Please direct support questions about CleverCoffee itself to the upstream project
rather than here. Their Discord supports their PCBs, and this is neither.

---

# About upstream CleverCoffee

The text below describes the upstream project this fork is based on.

CleverCoffee (formerly Rancilio PID) implements a PID controller for stable and
accurate temperature control, originally for the Rancilio Silvia but also
supporting Gaggia and Quickmill machines. Other machines can easily be added or
may already be compatible.

Additional features include:

* Shot timer
* Configurable pre-infusion
* Brew by weight
* Brew by time
* Pressure monitoring / profiling (still a work in progress)

The hardware has a small footprint and can easily fit into most compact espresso
machines. The original wiring of the machine (mostly) remains intact and is only
extended. The machine can be easily reverted back to its original state.

The project has been in active development and supported for over 5 years with
continuous improvements. Hundreds of machines have been successfully modded
already.

Project website: [Clever Coffee Website](https://clevercoffee.de).
PCB and parts: [Clever Coffee Shop](https://shop.clevercoffee.de/).
Manual (German): [handbook](https://rancilio-pid.github.io/ranciliopid-handbook/).
Chat and support: [Discord](https://discord.gg/Kq5RFznuU4).

Upstream requires no development environment: from version 4.0.0 you can flash
directly from any Chromium based browser using their
[Web Flasher](https://rancilio-pid.github.io/clevercoffee-flasher/). That applies
to the ESP32 firmware only, not to the Teensy build in this fork.

## Upstream feature overview

* Control of the brew temperature with an accuracy of up to +/- 0.1 degrees
* Reaches the target temperature within 5 to 10 minutes after switching on
* Set PID parameters and monitor current temperature and heater output on a web
  page hosted on the ESP controller
* Separate PID for steam mode with its own parameters and target temperature
* Automatically brew by set time including pre-infusion timing
* Automatically brew by weight using integrated weight cells or Bluetooth Low
  Energy scales
* Automatic backflush program
* Programmable standby timer
* Supports toggle or momentary switches for brew, steam, hot water delivery and
  power/standby
* Brew switch detection via an optocoupler module for a minimally invasive
  installation
* MQTT support to monitor and manipulate all important parameters
* Multiple display templates including vertical, with support for custom designs
* Over the air updates of the firmware via WiFi

Version 4.0.0 also brought a JSON based configuration system replacing EEPROM,
full Bluetooth Low Energy scale support including Acaia models, feature complete
support for the official CleverCoffee PCB, and improved Home Assistant MQTT
discovery.

Thanks to every single supporter of the upstream project. This fork would not
exist without five years of their work.
