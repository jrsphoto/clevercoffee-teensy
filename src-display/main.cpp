/**
 * @file main.cpp
 *
 * @brief ESP32-S3 touch-display HEAD for the Argenta SME espresso machine
 *
 * This board is a terminal, not a controller. It owns WiFi, the web UI and the
 * colour touch LCD. It owns NO actuators and NO safety. A touch on its screen is
 * a request sent to the Teensy, which decides what to do about it.
 *
 * The link is deliberately non-critical: if this board crashes, is unplugged, or
 * is never fitted at all, the Teensy runs the machine and finishes shots exactly
 * as normal. Nothing here may ever become something the machine depends on.
 *
 * Hardware: LCDwiki / Hosyond 4.0" ESP32-S3 Display
 *   ST7796S 320x480 IPS over 4-line SPI, FT6336U capacitive touch,
 *   ESP32-S3-N16R8 (16 MB flash, 8 MB OPI PSRAM).
 *
 * Serial arrangement, which matters:
 *   - USB-CDC is the console (ARDUINO_USB_CDC_ON_BOOT=1)
 *   - UART0 on IO43/IO44 is the Teensy link
 * The S3's native USB is what frees UART0 for the link. Without it, boot chatter
 * would be injected straight into the Teensy's RX1.
 */

#include <Arduino.h>

#include "audio.h"
#include "display.h"
#include "headConfig.h"
#include "es8311.h"
#include "link.h"
#include "net.h"

namespace {

    /**
     * Link to the Teensy. Serial1 is remapped onto the board's UART0 header
     * pins; the USB console is a separate device and is unaffected.
     */
    constexpr uint32_t kLinkBaud = 115200;

    /**
     * How long after this board boots a controller Hello is treated as part of
     * the same power-up rather than as a separate controller restart.
     *
     * The Teensy takes a few seconds to reach its Hello: a courtesy wait for a
     * USB console, then sensor init and the boot self check. Twenty seconds
     * covers that comfortably while still being far shorter than any real
     * interval between a power-up and a later reboot.
     */
    constexpr uint32_t kColdStartGraceMs = 20000;

    constexpr uint32_t kHeartbeatMs = 2000;
    uint32_t lastHeartbeat = 0;

    bool lcdReady = false;

    /**
     * Redraw pacing.
     *
     * Previously a flat 250 ms timer, which on top of the Teensy's 200 ms
     * telemetry period put 225 ms of average lag on everything - fine for a
     * boiler temperature, hopeless for a knob.
     *
     * Now the redraw is driven by frames ARRIVING rather than by the clock, so
     * a value pushed early by the Teensy reaches the screen promptly. kRedrawMs
     * becomes a floor that stops a burst of frames thrashing the SPI bus, and
     * kIdleRedrawMs still repaints occasionally with no link so the "stale"
     * indication updates.
     */
    constexpr uint32_t kRedrawMs = 60;
    constexpr uint32_t kIdleRedrawMs = 500;
    uint32_t lastRedraw = 0;
    uint32_t lastDrawnFrame = 0;

    /**
     * @brief Tiny serial console, so display orientation and inversion can be
     *        tuned interactively instead of through a reflash each time.
     */
    void handleCommand(const String& line) {
        if (line.startsWith("rot")) {
            const int r = line.substring(3).toInt();
            display::setRotation(static_cast<uint8_t>(r));
            Serial.printf("rotation %d  (%dx%d)\n", display::rotation(), display::lcd.width(), display::lcd.height());
        }
        else if (line == "inv") {
            display::toggleInvert();
            Serial.println("inversion toggled");
        }
        else if (line.startsWith("bl")) {
            const int b = line.substring(2).toInt();
            display::setBrightness(static_cast<uint8_t>(b));
            Serial.printf("brightness %d\n", b);
        }
        else if (line.startsWith("cmd ")) {
            const String body = line.substring(4);
            machinelink::sendCommand(body.c_str());
            Serial.printf("sent: %s\n", body.c_str());
        }
        else if (line.startsWith("txtest")) {
            // Is this board's link TX pin actually driving?
            //
            // The Teensy's `linkraw` can say whether ANY bytes reach its RX,
            // but not why none do. This drives IO44 directly as a GPIO, fast,
            // which the Teensy's UART sees as framing garbage. So:
            //
            //   Teensy sees junk bytes -> the wire is fine and the fault is in
            //                             how this board's UART is configured
            //   Teensy sees nothing    -> the signal is not getting there at
            //                             all; no firmware change will fix it
            //
            // Takes the pin away from Serial1 and gives it back afterwards.
            const int secs = line.substring(6).toInt();
            const uint32_t window = (secs > 0 && secs < 30) ? secs * 1000U : 8000U;

            Serial.printf("txtest: bit-banging IO%d for %lu ms - run 'linkraw' on the Teensy NOW\n",
                          display::kPinLinkTx, static_cast<unsigned long>(window));

            Serial1.flush();
            pinMode(display::kPinLinkTx, OUTPUT);

            const uint32_t start = millis();
            uint32_t edges = 0;

            while ((millis() - start) < window) {
                digitalWrite(display::kPinLinkTx, HIGH);
                delayMicroseconds(87); // ~ one bit at 115200
                digitalWrite(display::kPinLinkTx, LOW);
                delayMicroseconds(87);
                edges += 2;
            }

            // Hand the pin back to the UART.
            Serial1.begin(115200, SERIAL_8N1, display::kPinLinkRx, display::kPinLinkTx);

            Serial.printf("txtest: done, %lu edges. Serial1 restored.\n",
                          static_cast<unsigned long>(edges));
        }
        else if (line == "net" || line == "wifi") {
            net::printStatus();
        }
        else if (line == "wifireset") {
            // Upstream exposes this as a POST endpoint. Same effect: forget the
            // stored network and come back up in the setup portal.
            Serial.println("forgetting the network and restarting...");
            net::forgetNetworkAndRestart();
        }
        else if (line.startsWith("cfg ")) {
            // "cfg <key> <value>". Pushes one setting to the Teensy, in this
            // board's units - seconds for durations, as upstream stores them.
            const String rest = line.substring(4);
            const int sp = rest.indexOf(' ');

            if (sp <= 0) {
                Serial.println("usage: cfg <key> <value>   e.g. cfg brew.pre_infusion.time 4.5");
            }
            else {
                const String key = rest.substring(0, sp);
                const String value = rest.substring(sp + 1);
                machinelink::sendConfig(key.c_str(), value.c_str());
                Serial.printf("sent config: %s = %s\n", key.c_str(), value.c_str());
                Serial.println("watch the Teensy console for the ack - 'refused' means a");
                Serial.println("validating setter declined it, 'notmine' means not a control key");
            }
        }
        else if (line.startsWith("drawbench")) {
            const int n = line.substring(9).toInt();
            const uint8_t iters = (n > 0 && n < 100) ? static_cast<uint8_t>(n) : 20;
            Serial.printf("full redraw: %lu us average over %u passes\n",
                          static_cast<unsigned long>(display::benchmarkRedraw(iters)), iters);
        }
        else if (line == "redraw") {
            display::showBringupScreen();
            Serial.println("redrawn");
        }
        else if (line == "play") {
            Serial.println(audio::playStartupSound() ? "playing" : "could not play - see 'audio'");
        }
        else if (line.startsWith("vol")) {
            audio::setVolumePercent(static_cast<uint8_t>(line.substring(3).toInt()));
            Serial.printf("volume %d%%\n", audio::volumePercent());
        }
        else if (line == "audio") {
            audio::printStatus();
        }
        else if (line.startsWith("tone")) {
            const int hz = line.substring(4).toInt();
            Serial.printf("%s %d Hz test tone - 'stop' to end it\n", audio::startTone(hz > 0 ? hz : 1000) ? "playing" : "could not start", hz > 0 ? hz : 1000);
        }
        else if (line == "stop") {
            audio::stop();
            Serial.println("stopped, amp shut down");
        }
        else if (line.startsWith("amp")) {
            // Polarity test. The board's documentation says LOW enables the
            // amplifier; a silent speaker looks the same either way, so this
            // allows settling it by ear.
            const bool on = line.substring(3).toInt() != 0;
            audio::forceAmp(on);
            Serial.printf("amp pin forced %s (%s per the datasheet)\n", on ? "LOW" : "HIGH", on ? "enabled" : "shut down");
        }
        else if (line.startsWith("dpin")) {
            audio::setDataPin(line.substring(4).toInt());
        }
        else if (line == "regs") {
            es8311::dumpRegisters();
        }
        else if (line == "help" || line.isEmpty()) {
            Serial.println("commands: rot <0-3> | inv | bl <0-255> | redraw | cmd <body>");
            Serial.println("          txtest [s]  bit-bang the link TX pin, to prove the wire");
            Serial.println("          play | vol <0-100> | audio | tone <hz> | stop | amp <0|1> | regs | dpin <6|8>");
            Serial.println("  cmd ping=1        ask the Teensy to reply");
            Serial.println("  cmd flowreset=1   zero the flow accumulator");
            Serial.println("  rot 0,2 = portrait 320x480   rot 1,3 = landscape 480x320");
        }
        else {
            Serial.printf("unknown: %s\n", line.c_str());
        }
    }

    /**
     * Longest console line accepted.
     *
     * Was 32, which was ample for `rot 1` and `bl 128` and became wrong the
     * moment config arrived: `cfg cooling_flush.target_temperature 88` is 39
     * characters, and upstream has keys like
     * `hardware.sensors.temperature.rtd_reference` that are 42 on their own.
     *
     * Sized against the link frame rather than guessed: a config body is
     * `k=<key>,v=<value>` and must fit linkproto::kMaxFrame, so a console line
     * that fits here cannot build a frame that does not fit there.
     */
    constexpr size_t kConsoleLineMax = 160;

    void serviceConsole() {
        static String line;
        static bool overflowed = false;

        while (Serial.available() > 0) {
            const char c = static_cast<char>(Serial.read());

            if (c == '\r') {
                continue;
            }

            if (c == '\n') {
                line.trim();

                // SAY SO rather than silently acting on a truncated line. The
                // old code dropped the tail quietly, so an over-long command
                // ran against a mangled argument and looked like it had simply
                // been rejected - which cost a round of head-scratching when a
                // long config key stopped working and nothing said why.
                if (overflowed) {
                    Serial.printf("line too long (max %u), ignored: %s...\n",
                                  static_cast<unsigned>(kConsoleLineMax), line.c_str());
                }
                else {
                    handleCommand(line);
                }

                line = "";
                overflowed = false;

                continue;
            }

            if (line.length() < kConsoleLineMax) {
                line += c;
            }
            else {
                overflowed = true;
            }
        }
    }

    void printBanner() {
        Serial.println();
        Serial.println("========================================");
        Serial.println("CleverCoffee ESP32-S3 display head");
        Serial.println("Argenta SME - head only, no actuators");
        Serial.println("========================================");
        Serial.printf("PSRAM: %u bytes free\n", static_cast<unsigned>(ESP.getFreePsram()));
        Serial.printf("Flash: %u bytes\n", static_cast<unsigned>(ESP.getFlashChipSize()));
        Serial.printf("Link : UART0 RX%d TX%d @ %lu baud\n", display::kPinLinkRx, display::kPinLinkTx, static_cast<unsigned long>(kLinkBaud));
    }

} // namespace

void setup() {
    Serial.begin(115200);

    const uint32_t waitStart = millis();

    while (!Serial && (millis() - waitStart) < 1500) {
        // Brief courtesy pause for a bench console; never block a real boot.
    }

    printBanner();

    // The link to the Teensy. Opened before the display so that, once the
    // protocol exists, a Teensy already running has somewhere to send to while
    // this board is still drawing its first frame.
    machinelink::init();

    if (display::init()) {
        Serial.println("display: ST7796S initialised");
        lcdReady = true;
        display::lcd.fillScreen(TFT_BLACK);
        Serial.printf("display: %dx%d, rotation %d\n", display::lcd.width(), display::lcd.height(), display::rotation());
        Serial.println("commands: rot <0-3> | inv | bl <0-255> | redraw");
    }
    else {
        Serial.println("display: INIT FAILED - check the SPI pins against the LCDwiki page");
    }

    // AFTER the display, because the codec shares the touch panel's I2C bus and
    // display::init() is what opens it. A missing codec or speaker is not an
    // error worth stopping for - it just means no sound.
    if (audio::init()) {
        audio::playStartupSound();
    }

    // LAST, because WiFiManager's autoConnect blocks for up to its connect
    // timeout. By this point the link is open and the screen is drawing, so a
    // ten-second stall shows a "connecting" message instead of a dead panel -
    // and the Teensy has been running the machine throughout regardless.
    net::mountFilesystem();

    // After the filesystem (the config lives on it) and after the link is
    // open, so the first edit has somewhere to go. Before net::begin() so the
    // web server has a populated registry the moment it starts listening.
    headconfig::begin();

    net::begin();

    // Tell the controller what the settings actually are. The head holds the
    // master copy, so without this the Teensy would keep running on whatever
    // its EEPROM cache happened to hold. See TEENSY-PORT-BRIEF.md 9.6.
    headconfig::pushAllToController();
}

void loop() {
    serviceConsole();
    machinelink::loop();
    net::loop();
    headconfig::loop();

    // Feeds the I2S DMA while a sound is playing; a no-op the rest of the time.
    // Called before the redraw so a slow screen paint cannot starve it.
    audio::loop();

    // Poll the touch panel. This was missing entirely once the machine screen
    // replaced the bring-up screen, so every touch handler and hit box in
    // display.cpp was dead code - the buttons were drawn and could never be
    // pressed. Nothing reported an error because nothing was wrong: the code
    // was correct and simply never ran.
    if (lcdReady) {
        display::loop();
    }

    // A controller restart means every value on screen came from the previous
    // instance of it. Repaint rather than carry stale numbers across the gap,
    // and play the startup sound so a reboot is audible from across the room -
    // the machine restarting is worth noticing whichever processor did it.
    static uint32_t lastHello = 0;

    if (const uint32_t hellos = machinelink::helloCount(); hellos != lastHello) {
        lastHello = hellos;

        if (lcdReady) {
            display::invalidate();
        }

        // Do NOT replay on a cold start. Powering the machine on boots both
        // boards, so the Teensy's first Hello arrives a few seconds after this
        // board already played the sound at its own boot - chiming twice for
        // one power-up. Past that window a Hello means the controller alone
        // restarted, which is exactly the event worth hearing.
        if (millis() > kColdStartGraceMs && !audio::isPlaying()) {
            audio::playStartupSound();
        }

        Serial.println("controller restarted - repainting");

        // It came up on its EEPROM cache, which may be older than what is on
        // this board. The head holds the master, so it says so.
        headconfig::pushAllToController();
    }

    if (lcdReady) {
        const uint32_t now = millis();
        const uint32_t frames = machinelink::framesReceived();

        // Repaint when there is something new to show and the floor has
        // elapsed, or occasionally anyway so a stale readout still says so.
        const bool haveNewData = (frames != lastDrawnFrame) && ((now - lastRedraw) >= kRedrawMs);
        const bool idleRefresh = (now - lastRedraw) >= kIdleRedrawMs;

        if (haveNewData || idleRefresh) {
            lastRedraw = now;
            lastDrawnFrame = frames;
            display::drawMachineScreen();
        }
    }

    if (const uint32_t now = millis(); now - lastHeartbeat >= kHeartbeatMs) {
        lastHeartbeat = now;

        Serial.printf("alive, uptime %lus  link=%s frames=%lu badsum=%lu\n",
                      static_cast<unsigned long>(now / 1000),
                      machinelink::isConnected() ? "OK" : "stale",
                      static_cast<unsigned long>(machinelink::framesReceived()),
                      static_cast<unsigned long>(machinelink::badChecksums()));
    }
}
