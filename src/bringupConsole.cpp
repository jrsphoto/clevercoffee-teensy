/**
 * @file bringupConsole.cpp
 *
 * @brief Serial bring-up console for testing one output at a time on the bench
 */

#include "bringupConsole.h"

#include <Arduino.h>
#include <stdlib.h>
#include <string.h>

#include "commsHandler.h"
#include "encoderHandler.h"
#include "flowHandler.h"
#include "hardware/outputs.h"
#include "i2cHandler.h"
#include "pumpHandler.h"
#include "safetyHandler.h"
#include "switchHandler.h"
#include "tempHandler.h"
#include "updateMode.h"
#include "weightHandler.h"

namespace {

    /**
     * How long a console-energized channel stays on before switching itself off.
     * Long enough to get a meter on a terminal, short enough that walking away
     * is harmless.
     */
    constexpr uint32_t kAutoOffMs = 10000;

    /**
     * Warn once, this long before the auto-off fires, so the reason a channel
     * dropped is never a mystery.
     */
    constexpr uint32_t kAutoOffWarningMs = 3000;

    constexpr size_t kLineMax = 48;

    char line[kLineMax];
    size_t lineLen = 0;

    /** Which channel the console energized, and when. */
    bool haveActive = false;
    outputs::Channel activeChannel = outputs::Channel::Heater;
    uint32_t activeSinceMs = 0;
    bool warned = false;

    void printHelpImpl() {
        Serial.println();
        Serial.println("Bring-up console - commands:");
        Serial.println("  status            show every channel, fault state, uptime");
        Serial.println("  on <channel>      energize one channel (re-issue to extend)");
        Serial.println("  off <channel>     de-energize one channel");
        Serial.println("  off               de-energize everything");
        Serial.println("  temp              read both RTD temperature sensors");
        Serial.println("  rtd               raw RTD resistance, for diagnosing wiring");
        Serial.println("  failsensor        toggle a simulated boiler sensor failure (safety test)");
        Serial.println("  flow              flow sensor: volume, rate, pulses, calibration");
        Serial.println("  flowreset         zero the flow volume accumulator");
        Serial.println("  flowcal <ml>      calibrate from a measured volume");
        Serial.println("  flowk [n]         show or set calibration directly, pulses/litre");
        Serial.println("  pump <0-100>      pump speed %% (PWM); re-issue to hold, 0 stops");
        Serial.println("  pumpshed <0|1>    heater/pump load-shed interlock");
        Serial.println("  brew              brew switch: raw level, debounced state, sense");
        Serial.println("  brewsense         flip which level means pressed (JP2 orientation)");
        Serial.println("  brewpull <u|d|x>  pull: internal up / internal down / none (PCB has R9)");
        Serial.println("  brewtype          flip between momentary and toggle");
        Serial.println("  brewwatch         watch the brew switch live while you operate it");
        Serial.println("  scale             load cell: weight, calibration, health");
        Serial.println("  scaletare         zero the scale (non-blocking)");
        Serial.println("  scalecal <g>      calibrate from a known mass on the cell");
        Serial.println("  scalesamples <n>  averaging length; fewer = faster, noisier");
        Serial.println("  scalerestart      retry start-up, e.g. after plugging one in");
        Serial.println("  scalediag         probe the HX711 data line directly, bypassing the lib");
        Serial.println("  scalefactor <n>   set the calibration factor directly (1 = raw counts)");
        Serial.println("  scaledual <0|1>   read one load cell or two (two share one plate here)");
        Serial.println("  flowwatch         watch the flow signal line directly (wiring test)");
        Serial.println("  i2c               scan the I2C bus and name what is found");
        Serial.println("  enc               encoder counter, rotation and LED state");
        Serial.println("  link              display-head link state and frame counters");
        Serial.println("  update            reboot into firmware-update mode (machine must be idle)");
        Serial.println("  led <rrggbb>      force the encoder LED to a colour");
        Serial.println("  encint            watch the encoder INT line while you turn the knob");
        Serial.println("  i2cslow           SDA electrical test + scan at 10/2/1 kHz");
        Serial.println("  clear             clear a latched fault (deliberate act)");
        Serial.println("  help              this list");
        Serial.println("Channels: heater, valve, fill   (the pump is PWM - use 'pump <0-100>')");
        Serial.print("One channel at a time. Auto-off after ");
        Serial.print(kAutoOffMs / 1000);
        Serial.println("s. Nothing energizes while faulted.");
        Serial.println();
    }

    void printStatus() {
        Serial.println();
        Serial.print("uptime ");
        Serial.print(millis() / 1000);
        Serial.println("s");

        if (safety::isFaulted()) {
            Serial.print("FAULT LATCHED: ");
            Serial.print(safetyFaultName(safety::latchedFault));
            Serial.print(" (");
            Serial.print(safety::latchedDetail);
            Serial.println(") - outputs blocked, 'clear' to reset");
        }
        else {
            Serial.println("no fault latched");
        }

        for (uint8_t i = 0; i < outputs::kChannelCount; i++) {
            const auto channel = static_cast<outputs::Channel>(i);

            Serial.print("  ");
            Serial.print(outputs::name(channel));
            Serial.print(" (pin ");
            Serial.print(outputs::pin(channel));
            Serial.print(") : ");
            Serial.println(outputs::isOn(channel) ? "ON" : "off");
        }

        temps::printStatus();
        flow::printStatus();
        encoder::printStatus();
        comms::printStatus();

        Serial.println();
    }

    /**
     * @brief Resolve a channel name. Returns false if it is not one of ours.
     */
    bool parseChannel(const char* token, outputs::Channel& out) {
        for (uint8_t i = 0; i < outputs::kChannelCount; i++) {
            const auto channel = static_cast<outputs::Channel>(i);

            if (strcasecmp(token, outputs::name(channel)) == 0) {
                out = channel;

                return true;
            }
        }

        return false;
    }

    void releaseActive(const char* reason) {
        if (!haveActive) {
            return;
        }

        outputs::set(activeChannel, false);

        Serial.print(reason);
        Serial.print(": ");
        Serial.print(outputs::name(activeChannel));
        Serial.println(" off");

        haveActive = false;
        warned = false;
    }

    void commandOn(const char* token) {
        outputs::Channel channel;

        if (!parseChannel(token, channel)) {
            Serial.print("unknown channel '");
            Serial.print(token);
            Serial.println("' - try: heater, pump, valve, fill");

            return;
        }

        if (safety::isFaulted()) {
            Serial.print("REFUSED: fault latched (");
            Serial.print(safetyFaultName(safety::latchedFault));
            Serial.println(") - nothing energizes until it is cleared");

            return;
        }

        // Re-issuing the same channel just restarts the auto-off timer, so a
        // measurement that needs longer does not require turning it off and on.
        if (haveActive && activeChannel == channel) {
            activeSinceMs = millis();
            warned = false;

            Serial.print("extended: ");
            Serial.print(outputs::name(channel));
            Serial.print(" stays on for another ");
            Serial.print(kAutoOffMs / 1000);
            Serial.println("s");

            return;
        }

        if (haveActive) {
            Serial.print("REFUSED: ");
            Serial.print(outputs::name(activeChannel));
            Serial.println(" is still on - one channel at a time. 'off' first.");

            return;
        }

        if (!outputs::set(channel, true)) {
            if (channel == outputs::Channel::Heater && !safety::boilerSensorHealthy) {
                Serial.println("REFUSED: boiler temperature is not fresh/believable right now.");
                Serial.println("         The heater is never permitted without a live boiler reading.");
            }
            else {
                Serial.println("REFUSED: output layer declined the request");
            }

            return;
        }

        haveActive = true;
        activeChannel = channel;
        activeSinceMs = millis();
        warned = false;

        Serial.print("ON: ");
        Serial.print(outputs::name(channel));
        Serial.print(" (pin ");
        Serial.print(outputs::pin(channel));
        Serial.print(") - auto-off in ");
        Serial.print(kAutoOffMs / 1000);
        Serial.println("s");
    }

    void commandOff(const char* token) {
        if (token == nullptr || *token == '\0') {
            outputs::allOff();
            haveActive = false;
            warned = false;

            Serial.println("all channels off");

            return;
        }

        outputs::Channel channel;

        if (!parseChannel(token, channel)) {
            Serial.print("unknown channel '");
            Serial.print(token);
            Serial.println("'");

            return;
        }

        outputs::set(channel, false);

        if (haveActive && activeChannel == channel) {
            haveActive = false;
            warned = false;
        }

        Serial.print("off: ");
        Serial.println(outputs::name(channel));
    }

    void handleLine(char* text) {
        // Split into a command and at most one argument.
        char* command = strtok(text, " \t");

        if (command == nullptr) {
            return;
        }

        char* argument = strtok(nullptr, " \t");

        if (strcasecmp(command, "help") == 0 || strcasecmp(command, "?") == 0) {
            printHelpImpl();
        }
        else if (strcasecmp(command, "status") == 0) {
            printStatus();
        }
        else if (strcasecmp(command, "temp") == 0) {
            temps::printStatus();
        }
        else if (strcasecmp(command, "failsensor") == 0) {
            const bool on = !temps::boilerFaultInjected();
            temps::injectBoilerFault(on);

            Serial.print("boiler sensor fault injection ");
            Serial.println(on ? "ON - the machine should latch safe within ~2s" : "OFF");
        }
        else if (strcasecmp(command, "pump") == 0) {
            if (argument == nullptr) {
                pump::printStatus();
                Serial.println("usage: pump <0-100>    0 stops; a command expires after 1.5 s");
            }
            else {
                const float pct = static_cast<float>(atof(argument));

                if (pump::setSpeed(pct)) {
                    Serial.print("pump: requested ");
                    Serial.print(pct, 1);
                    Serial.println("%");

                    if (pct > 0.0f) {
                        Serial.println("      NOTE: the console does not re-assert this, so it will");
                        Serial.println("      stop itself after 1.5 s. Re-issue to keep it running.");
                    }
                }
                else {
                    Serial.print("pump: REFUSED - ");
                    Serial.println(pump::lastRefusal());
                }
            }
        }
        else if (strcasecmp(command, "pumpshed") == 0) {
            if (argument == nullptr) {
                Serial.print("load shed is ");
                Serial.println(pump::loadShed() ? "ON" : "off");
                Serial.println("usage: pumpshed 0 | 1");
            }
            else {
                pump::setLoadShed(atoi(argument) != 0);

                Serial.print("load shed is now ");
                Serial.println(pump::loadShed() ? "ON - pump will not run with the heater on" : "off");
            }
        }
        else if (strcasecmp(command, "scale") == 0) {
            weight::printStatus();
        }
        else if (strcasecmp(command, "scaletare") == 0) {
            weight::startTare();
        }
        else if (strcasecmp(command, "scalefactor") == 0) {
            if (argument == nullptr) {
                Serial.print("calibration factor is ");
                Serial.println(weight::calFactor(), 4);
                Serial.println("usage: scalefactor <n>    1 = raw counts, negative = inverted cell");
            }
            else if (const float f = static_cast<float>(atof(argument)); f != 0.0f) {
                weight::setCalFactor(f);

                Serial.print("calibration factor set to ");
                Serial.println(weight::calFactor(), 4);
            }
            else {
                Serial.println("REFUSED: zero would make every weight infinite");
            }
        }
        else if (strcasecmp(command, "scaledual") == 0) {
            if (argument == nullptr) {
                Serial.print("reading ");
                Serial.println(weight::dualCell() ? "TWO load cells" : "ONE load cell");
                Serial.println("usage: scaledual 0 | 1");
            }
            else {
                weight::setDualCell(atoi(argument) != 0);
            }
        }
        else if (strcasecmp(command, "scalediag") == 0) {
            weight::probeDataLine();
        }
        else if (strcasecmp(command, "scalerestart") == 0) {
            weight::restart();
        }
        else if (strcasecmp(command, "scalecal") == 0) {
            if (argument == nullptr) {
                Serial.print("calibration factor is ");
                Serial.println(weight::calFactor(), 4);
                Serial.println("usage: scalecal <grams of the mass now on the cell>");
                Serial.println("       tare with nothing on it FIRST, then put the mass on.");
            }
            else {
                weight::calibrateTo(static_cast<float>(atof(argument)));
            }
        }
        else if (strcasecmp(command, "scalesamples") == 0) {
            if (argument == nullptr) {
                Serial.print("averaging ");
                Serial.print(weight::samples());
                Serial.println(" samples");
                Serial.println("usage: scalesamples <n>   (1-32; at 10 SPS, n samples is n*100ms of lag)");
            }
            else if (const int n = atoi(argument); n >= 1 && n <= 32) {
                weight::setSamples(n);

                Serial.print("averaging ");
                Serial.print(weight::samples());
                Serial.print(" samples, about ");
                Serial.print(n * 100);
                Serial.println(" ms of lag at 10 SPS");
            }
            else {
                Serial.println("REFUSED: samples must be 1-32");
            }
        }
        else if (strcasecmp(command, "brew") == 0) {
            switches::printStatus();
        }
        else if (strcasecmp(command, "brewsense") == 0) {
            switches::setActiveHigh(!switches::activeHigh());

            Serial.print("brew switch now active ");
            Serial.println(switches::activeHigh() ? "HIGH (JP2 pulling DOWN, switch to +3.3V)" : "LOW (JP2 pulling UP, switch to GND)");
            Serial.println("Release latch reset - operate the switch once to re-arm it.");
        }
        else if (strcasecmp(command, "brewpull") == 0) {
            if (argument == nullptr) {
                Serial.print("pull is ");
                Serial.println(switches::pullName());
                Serial.println("usage: brewpull u | d | x");
                Serial.println("  u  internal pull-up, switch shorts pin 24 to GND   (breadboard)");
                Serial.println("  d  internal pull-down, switch shorts pin 24 to 3V3 (breadboard)");
                Serial.println("  x  no internal pull - for the finished PCB, where R9 pulls");
            }
            else {
                switch (argument[0]) {
                    case 'u':
                    case 'U':
                        switches::setPull(switches::Pull::InternalUp);
                        switches::setActiveHigh(false);
                        break;
                    case 'd':
                    case 'D':
                        switches::setPull(switches::Pull::InternalDown);
                        switches::setActiveHigh(true);
                        break;
                    case 'x':
                    case 'X':
                        switches::setPull(switches::Pull::External);
                        break;
                    default:
                        Serial.println("usage: brewpull u | d | x");

                        return;
                }

                // The sense is set alongside the pull for the two internal
                // options, because for those the wiring is implied: with a
                // pull-up the switch must go to GND, so it is active low.
                // Pull::External leaves the sense alone - the board's JP2
                // decides, and only 'brewsense' can say which way.
                Serial.print("pull is now ");
                Serial.println(switches::pullName());
                Serial.print("sense is active ");
                Serial.println(switches::activeHigh() ? "HIGH" : "LOW");
            }
        }
        else if (strcasecmp(command, "brewwatch") == 0) {
            switches::startWatch(20000);
        }
        else if (strcasecmp(command, "brewtype") == 0) {
            switches::setMomentary(!switches::momentary());

            Serial.print("brew switch now ");
            Serial.println(switches::momentary() ? "MOMENTARY (spring return)" : "TOGGLE (latching)");
        }
        else if (strcasecmp(command, "flow") == 0) {
            flow::printStatus();
        }
        else if (strcasecmp(command, "flowreset") == 0) {
            flow::reset();
            Serial.println("flow volume zeroed");
        }
        else if (strcasecmp(command, "flowcal") == 0) {
            if (argument == nullptr) {
                Serial.println("usage: flowcal <millilitres actually dispensed>");
            }
            else if (const double ml = atof(argument); flow::calibrateTo(ml)) {
                Serial.print("calibrated: ");
                Serial.print(flow::pulsesPerLitre());
                Serial.println(" pulses/litre");
            }
            else {
                Serial.println("REFUSED: need at least 100 pulses and a positive volume.");
                Serial.println("         Run more water through before calibrating.");
            }
        }
        else if (strcasecmp(command, "flowk") == 0) {
            if (argument == nullptr) {
                Serial.print("calibration is ");
                Serial.print(flow::pulsesPerLitre());
                Serial.println(" pulses/litre");
                Serial.println("usage: flowk <pulses per litre>   (DM60 Nano is ~36000-49000)");
            }
            else {
                flow::setPulsesPerLitre(static_cast<uint32_t>(atol(argument)));
                Serial.print("calibration set to ");
                Serial.print(flow::pulsesPerLitre());
                Serial.println(" pulses/litre");
            }
        }
        else if (strcasecmp(command, "i2cslow") == 0) {
            i2c::probeSdaDrive();
            i2c::scanSlow();
        }
        else if (strcasecmp(command, "encint") == 0) {
            i2c::watchEncoderInt(12);
        }
        else if (strcasecmp(command, "enctest") == 0) {
            i2c::compareWireVsBitBang(0x04);
        }
        else if (strcasecmp(command, "update") == 0) {
            const char* reason = "";

            if (!updateMode::isSafe(&reason)) {
                Serial.print("REFUSED: not safe to update - ");
                Serial.println(reason);
                Serial.println("         Everything must be off: the control loop stops during");
                Serial.println("         an update, so anything left on stays on unsupervised.");
            }
            else {
                updateMode::requestAndReboot(&reason); // does not return
            }
        }
        else if (strcasecmp(command, "link") == 0) {
            comms::printStatus();
        }
        else if (strcasecmp(command, "enc") == 0) {
            encoder::printStatus();
        }
        else if (strcasecmp(command, "led") == 0) {
            if (argument == nullptr) {
                Serial.println("usage: led <rrggbb hex>   e.g. led ff0000");
            }
            else {
                const uint32_t rgb = static_cast<uint32_t>(strtoul(argument, nullptr, 16));
                encoder::setLedColor(rgb);
                Serial.print("LED set to 0x");
                Serial.println(rgb, HEX);
            }
        }
        else if (strcasecmp(command, "flowwatch") == 0) {
            flow::startWatch(20000);
        }
        else if (strcasecmp(command, "i2c") == 0) {
            i2c::checkLines();
            i2c::scan();
            i2c::probeExpected();
            i2c::scanReserved();
            i2c::scanBothOrientations();
            i2c::scanOtherPinPairs();
            i2c::reportPullupPins();
        }
        else if (strcasecmp(command, "rtd") == 0) {
            temps::printDiagnostics();
        }
        else if (strcasecmp(command, "on") == 0) {
            if (argument == nullptr) {
                Serial.println("usage: on <heater|pump|valve|fill>");
            }
            else {
                commandOn(argument);
            }
        }
        else if (strcasecmp(command, "off") == 0) {
            commandOff(argument);
        }
        else if (strcasecmp(command, "clear") == 0) {
            if (!safety::isFaulted()) {
                Serial.println("no fault latched");
            }
            else {
                Serial.print("clearing latched fault: ");
                Serial.println(safetyFaultName(safety::latchedFault));

                // Everything stays off. Clearing a fault permits future commands;
                // it does not re-energize whatever was on when the fault hit.
                outputs::allOff();
                haveActive = false;
                safety::clearFault();

                Serial.println("fault cleared - outputs remain off");
            }
        }
        else {
            Serial.print("unknown command '");
            Serial.print(command);
            Serial.println("' - type 'help'");
        }
    }

    /**
     * @brief Enforce the auto-off timer.
     */
    void serviceAutoOff() {
        if (!haveActive) {
            return;
        }

        const uint32_t elapsed = millis() - activeSinceMs;

        if (!warned && elapsed >= (kAutoOffMs - kAutoOffWarningMs)) {
            warned = true;

            Serial.print("warning: ");
            Serial.print(outputs::name(activeChannel));
            Serial.print(" auto-off in ");
            // Round to nearest second rather than truncating, so the countdown
            // reads 3 at exactly 3000 ms left instead of 2.
            Serial.print((kAutoOffMs - elapsed + 500) / 1000);
            Serial.println("s - re-issue 'on' to extend");
        }

        if (elapsed >= kAutoOffMs) {
            releaseActive("auto-off");
        }
    }

} // namespace

void bringupConsoleBegin() {
    lineLen = 0;
    haveActive = false;
    warned = false;

    printHelpImpl();
}

void bringupConsolePrintHelp() {
    printHelpImpl();
}

void bringupConsoleLoop() {
    // If a fault lands while something is on, the safety layer has already
    // dropped the pins. Catch up our own bookkeeping and say why.
    if (haveActive && safety::isFaulted()) {
        releaseActive("fault latched");
    }

    serviceAutoOff();

    while (Serial.available() > 0) {
        const char c = static_cast<char>(Serial.read());

        if (c == '\r') {
            continue;
        }

        if (c == '\n') {
            line[lineLen] = '\0';

            if (lineLen > 0) {
                handleLine(line);
            }

            lineLen = 0;

            continue;
        }

        if (lineLen < (kLineMax - 1)) {
            line[lineLen++] = c;
        }
        else {
            // Overlong line: drop it rather than acting on a truncated command.
            lineLen = 0;

            Serial.println("line too long, ignored");
        }
    }
}
