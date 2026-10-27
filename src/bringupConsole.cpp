/**
 * @file bringupConsole.cpp
 *
 * @brief Serial bring-up console for testing one output at a time on the bench
 */

#include "bringupConsole.h"

#include <Arduino.h>
#include <string.h>

#include "hardware/outputs.h"
#include "safetyHandler.h"

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
        Serial.println("  clear             clear a latched fault (deliberate act)");
        Serial.println("  help              this list");
        Serial.println("Channels: heater, pump, valve, fill");
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
            Serial.println("REFUSED: output layer declined the request");

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
