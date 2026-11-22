# Using the DuPPa i2cEncoder V2 instead of a plain rotary encoder

I've been running one of these as the control input on my build, a Teensy 4.1
plus an ESP32-S3 display driving an Astoria Argenta SME. It does profile
selection, brew start and stop, flush and backflush. It's been on the bench for
weeks now and I think it's a better fit for CleverCoffee than a bare quadrature
encoder, so here's the code and what I ran into.

Library is [Fattoresaimon/ArduinoDuPPaLib](https://github.com/Fattoresaimon/ArduinoDuPPaLib).

## Why I went this way

A plain encoder needs three GPIO pins, two interrupt handlers, and a debounce
scheme you write yourself and then tune. If you also want a status light that's
another part and more pins.

The DuPPa is one I2C device that already does all of it in hardware. Debounce
is a register. Double-press detection is a register and then a flag you read.
The counter lives in the device with its own min, max and step, so you can't
lose counts. And the version I'm using has an RGB LED in the same part, which
turns out to be the thing I'd least want to give up now.

It also has an interrupt line, which I'll come back to because it made a
measurable difference.

## Setting it up

This is all of it, from `encoderHandler.cpp`:

```cpp
uint16_t flags = i2cEncoderLibV2::INT_DATA
               | i2cEncoderLibV2::WRAP_DISABLE
               | i2cEncoderLibV2::DIRE_RIGHT
               | i2cEncoderLibV2::IPUP_ENABLE
               | i2cEncoderLibV2::RMOD_X1
               | i2cEncoderLibV2::RGB_ENCODER;   // leave out on the non-RGB part

dev->begin(flags);

// Which events pull the interrupt line down
dev->writeInterruptConfig(i2cEncoderLibV2::RINC | i2cEncoderLibV2::RDEC
                        | i2cEncoderLibV2::PUSHP | i2cEncoderLibV2::PUSHR
                        | i2cEncoderLibV2::PUSHD);

// Wide limits on purpose. My handlers use deltas, not absolute position.
dev->writeCounter(static_cast<int32_t>(0));
dev->writeMax(static_cast<int32_t>(32767));
dev->writeMin(static_cast<int32_t>(-32768));
dev->writeStep(static_cast<int32_t>(1));

// Button debounce, in the encoder itself. Units of 10 ms.
dev->writeDoublePushPeriod(50);    // 500 ms double-press window
dev->writeAntibouncingPeriod(20);  // 200 ms
```

Those last two lines are the entire button handling. There's no debounce code
anywhere else in my firmware.

I also check something actually answers before building the driver around it.
The library is happy to talk to nothing, and then a missing encoder looks like
a working one that never moves:

```cpp
Wire.beginTransmission(cfgAddress);

if (cfgAddress > 0x07 && Wire.endTransmission() != 0) {
    present = false;
    return false;
}
```

## The interrupt line

I had this polling the status register every pass of the main loop at first.
That's an I2C transaction on a 100 kHz bus, about half a millisecond, and it
turned out to be the biggest single thing holding my loop rate down. I was
getting about 1.3 kHz with nobody touching the knob.

With `INT_DATA` set the interrupt line is open drain and a pull-up holds it high
when there's nothing to report. So now I only talk to the encoder when it says
there's something to talk about:

```cpp
void loop() {
    if (dev == nullptr) {
        return;
    }

    if (digitalReadFast(PIN_ENC_INT) == LOW) {
        dev->updateStatus();

        if (dev->readStatus(i2cEncoderLibV2::RINC)
         || dev->readStatus(i2cEncoderLibV2::RDEC)) {
            const int32_t now = dev->readCounterLong();
            pendingRotation += now - lastCounter;
            lastCounter = now;
        }

        if (dev->readStatus(i2cEncoderLibV2::PUSHP)) {
            pressedAtMs = millis();
        }
        // PUSHR and PUSHD below
    }

    // This stays outside the gate. It's a timer, not knob traffic.
    if (singlePending && static_cast<int32_t>(millis() - singleDeadlineMs) >= 0) {
        singlePending = false;
        pendingGesture = Gesture::Short;
    }

    updateStatusColor();
}
```

Watch what's outside the gate there. Anything that's a clock rather than a knob
event has to keep running every pass. I gated too much at first and the gesture
logic stalled.

## Short, long and double press

The device gives you press, release and double. I work out long press and the
single/double split myself. The only fiddly bit is that you can't act on a
single press straight away:

```cpp
if (dev->readStatus(i2cEncoderLibV2::PUSHR)) {
    const uint32_t heldMs = millis() - pressedAtMs;

    if (heldMs >= kLongPressMs) {            // 600 ms
        pendingGesture = Gesture::Long;
        singlePending = false;
    }
    else {
        // The first press of a double looks exactly like a single at this
        // point, so firing now would select profile 1 every time somebody
        // asked for profile 2.
        singlePending = true;
        singleDeadlineMs = millis() + kDoublePressWindowMs;
    }
}

if (dev->readStatus(i2cEncoderLibV2::PUSHD)) {
    singlePending = false;    // that pending single was this double's first half
    pendingGesture = Gesture::Double;
}
```

Gestures get consumed rather than polled, so nothing can read the same press
twice:

```cpp
Gesture takeGesture() {
    const Gesture g = pendingGesture;
    pendingGesture = Gesture::None;
    return g;
}
```

## The LED, which is the bit I'd keep

On a coffee machine this is worth more than it sounds. The knob is already where
the user's hand is, so it's the obvious place to say what the machine is doing.

The order of the tests matters a lot more than the colours do. Safety goes
first so nothing cosmetic can hide a fault:

```cpp
uint32_t statusColor() {
    // Safety first. A profile colour must never be able to mask a fault.
    if (safety::isFaulted())      return cfgColors[kSlotError];
    if (!temps::boilerHealthy())  return cfgColors[kSlotError];
    if (shot::isRunning())        return cfgColors[kSlotActive];
    if (heater::isHeating())      return cfgColors[kSlotHeating];

    // Nothing worth saying, so show the selected profile instead. More use
    // than a generic idle colour, because it tells you what the knob will pour.
    return profileColor;
}

void updateStatusColor() {
    if (!cfgHasRgbLed || !cfgStatusColors || dev == nullptr) {
        return;
    }

    const uint32_t color = statusColor();

    // Only write when it changes, or you're putting a colour on the bus every
    // pass for no reason.
    if (color != lastColor) {
        dev->writeRGBCode(color);
        lastColor = color;
    }
}
```

One other thing I'd suggest if you do this: have one definition of "heating"
and use it in both places. Mine is `heater::isHeating()` and it feeds the LED
and the status text on the display, so the knob and the screen can't end up
disagreeing about what the machine is doing.

Colours are stored as plain 0x000000 to 0xFFFFFF ints and I expose them as web
settings with a colour picker, so changing them isn't a recompile.

## Stuff that caught me out

All three of these look like a dead encoder, which is why they cost me time.

**A brand new one answers in the I2C reserved range.** The address comes from
solder jumpers A0 to A6 and a new unit arrives with all of them open, which
puts it down at a reserved low address. DuPPa's own docs tell you not to leave
it like that, but that's how it ships. The catch is that a *correct* I2C
scanner skips the reserved range, so it'll never find the thing. Mine answers
at 0x04. My other board has jumpers fitted and sits at 0x66.

If you add a scanner, probe 0x00 to 0x07 separately. The hardware peripheral
won't send those addresses so you have to bit-bang them. I spent a while
convinced I had a dead encoder or a bad cable before I worked this out.

**`writeInterruptConfig()` isn't optional once you gate on the line.** If you
poll regardless you'll never know the register exists. The moment you start
gating, an INTCONF register sitting at whatever it powers up as means the line
never asserts and the knob is just dead. Keep the list in that call and the
list of events you read back the same.

**Without `writeStep()` the counter can't move at all.** Step defaults to zero.
Rotating does nothing, but the button still works fine, so it looks like a
wiring problem on A and B rather than a missing register write.

## Summary of what changed

| | plain encoder | DuPPa |
|---|---|---|
| pins | 3 GPIO | SDA, SCL, plus one interrupt if you want it |
| debounce | software, tuned by hand | one register |
| double press | software state machine | one flag |
| counter | software, loses missed edges | in the device, with limits |
| status light | another part, more pins | built in |

The interrupt line is optional. It works fine polled, you just pay for it in
loop rate.

Happy to answer questions or pull any of this out into a PR if it's useful.
