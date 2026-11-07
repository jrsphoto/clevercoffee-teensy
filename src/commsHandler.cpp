/**
 * @file commsHandler.cpp
 *
 * @brief The link to the ESP32-S3 display head (Serial1, D0/D1)
 */

#include "commsHandler.h"

#include <Arduino.h>
#include <linkProtocol.h>

#include "encoderHandler.h"
#include "flowHandler.h"
#include "heaterHandler.h"
#include "hardware/outputs.h"
#include "hardware/pinmapping.h"
#include "configStore.h"
#include "safetyHandler.h"
#include "shotHandler.h"
#include "tempHandler.h"
#include "pumpHandler.h"
#include "switchHandler.h"
#include "weightHandler.h"
#include "updateMode.h"

namespace comms {

    namespace {

        linkproto::Parser parser;

        uint32_t lastTelemetryMs = 0;
        uint32_t lastRxMs = 0;

        uint32_t sent = 0;
        uint32_t received = 0;

        /**
         * @brief Send one frame.
         *
         * Writes and returns. Serial1's buffer absorbs it; if the head is absent
         * the bytes simply go nowhere, which is the intended behaviour - the
         * Teensy never waits on the head for anything.
         */
        void send(const linkproto::FrameType type, const char* body) {
            char frame[linkproto::kMaxFrame];
            const size_t n = linkproto::buildFrame(frame, sizeof(frame), type, body);

            if (n == 0) {
                return;
            }

            Serial1.write(frame, n);
            sent++;
        }

        /**
         * @brief One short word describing what the machine is doing.
         *
         * Ordered by what the operator most needs to know, not by the
         * machine's internals. What the pump is doing matters less than WHY it
         * is doing it, so a shot reports "brewing" rather than "pump" - the
         * pump duty is a separate field anyway.
         *
         * Heating comes last of the active states because the heater is on for
         * most of the machine's life and would otherwise mask everything else.
         *
         * Keep every word under 16 characters: that is the head's buffer size
         * (link.h, State::status).
         */
        const char* stateWord() {
            if (safety::isFaulted()) {
                return "fault";
            }

            if (shot::isBackflushing()) {
                return "backflush";
            }

            if (shot::isFlushing()) {
                return "flushing";
            }

            // Deliberately NOT shot::stateName(): that is written for a human
            // reading a console and returns "preinfusion pause", which is 17
            // characters and contains a space - too long for the head's buffer
            // and a space in a frame field is asking for trouble. Short,
            // single-word forms here, chosen for the wire rather than for
            // prose.
            if (shot::isRunning()) {
                switch (shot::state()) {
                    case shot::State::CoolingFlush:
                        // Worth its own word rather than hiding inside
                        // "brewing": the operator asked for coffee and the
                        // machine is running water instead, so the screen has
                        // to say why or it looks like a fault.
                        return "coolflush";

                    case shot::State::Recovery:
                        return "recovery";

                    case shot::State::Preinfusion:
                        return "preinfuse";

                    case shot::State::PreinfusionPause:
                        return "soak";

                    default:
                        return "brewing";
                }
            }

            if (outputs::isOn(outputs::Channel::Heater)) {
                return "heating";
            }

            if (pump::isRunning()) {
                return "pump";
            }

            return "idle";
        }

        /**
         * Floor on expedited frames. Spinning the encoder fast must not turn
         * the link into a flood: at 40 ms a continuous spin costs 25 frames a
         * second, about 24% of the wire, and still looks instant to a person.
         */
        constexpr uint32_t kMinTelemetryGapMs = 40;

        long lastSentEncoder = 0;
        bool lastSentBrew = false;

        void sendTelemetry() {
            char body[linkproto::kMaxFrame];

            // wcal says whether the weight is in grams or in raw ADC counts.
            // Sent as its own field rather than inferred at the far end,
            // because the head must never label raw counts as grams - a
            // plausible wrong weight on screen is worse than no weight.
            snprintf(body, sizeof(body),
                     "bt=%.1f,gt=%.1f,bok=%d,gok=%d,sp=%.1f,hd=%.0f,hen=%d,fl=%.2f,vol=%.1f,wt=%.1f,wok=%d,wcal=%d,pmp=%.0f,enc=%ld,st=%s,flt=%s",
                     temps::boilerC(),
                     temps::groupC(),
                     temps::boilerHealthy() ? 1 : 0,
                     temps::groupHealthy() ? 1 : 0,
                     heater::setpointC(),
                     heater::dutyPercent(),
                     heater::enabled() ? 1 : 0,
                     flow::rateMlPerSec(),
                     flow::totalMl(),
                     weight::grams(),
                     weight::healthy() ? 1 : 0,
                     weight::calFactor() == 1.0f ? 0 : 1,
                     pump::actualSpeed(),
                     static_cast<long>(encoder::counter()),
                     stateWord(),
                     safety::isFaulted() ? safetyFaultName(safety::latchedFault) : "none");

            send(linkproto::FrameType::Telemetry, body);
        }

        void ack(const char* what, const bool ok, const char* reason) {
            char body[96];
            snprintf(body, sizeof(body), "cmd=%s,ok=%d,why=%s", what, ok ? 1 : 0, reason);
            send(linkproto::FrameType::Ack, body);
        }

        /**
         * @brief Apply one config value pushed by the head.
         *
         * Body is `k=<dotted.key>,v=<value>`. The head holds the master copy
         * and the whole schema; this side only validates, applies, persists and
         * acknowledges. See TEENSY-PORT-BRIEF.md 9.6.
         *
         * Every value goes through configStore, which routes it to the owning
         * handler's validating setter. Nothing here writes a control variable
         * directly, which is what stops a wrong web form doing something a
         * person at the panel could not.
         *
         * A key this firmware does not own is acked as "notmine" rather than
         * refused. The head pushes its entire config, which legitimately
         * contains WiFi, MQTT and display settings; treating those as errors
         * would make a normal handshake look like a fault.
         */
        void handleConfig() {
            char key[64];
            char value[48];

            if (!parser.get("k", key, sizeof(key)) || !parser.get("v", value, sizeof(value))) {
                ack("cfg", false, "malformed");

                return;
            }

            const char* why = "";
            const bool ok = configStore::apply(key, value, &why);

            // The ack names the KEY rather than just "cfg", so the head can
            // match a reply to the specific setting it sent during a burst.
            char what[72];
            snprintf(what, sizeof(what), "cfg:%s", key);
            ack(what, ok, why);
        }

        /**
         * @brief Act on a command from the head.
         *
         * Everything here is a REQUEST. Each one goes through the same gates a
         * local action would, so the head gains no privilege by being on the
         * other end of a wire.
         */
        void handleCommand() {
            char value[48];

            if (parser.get("flowreset", value, sizeof(value))) {
                flow::reset();
                ack("flowreset", true, "ok");

                return;
            }

            if (parser.get("clearfault", value, sizeof(value))) {
                // Clearing a fault is a deliberate operator action, and it does
                // NOT re-energise anything - outputs stay off until something
                // asks for them again.
                if (!safety::isFaulted()) {
                    ack("clearfault", false, "nofault");

                    return;
                }

                outputs::allOff();
                safety::clearFault();
                ack("clearfault", true, "cleared");

                return;
            }

            if (parser.get("update", value, sizeof(value))) {
                const char* reason = "";

                if (!updateMode::isSafe(&reason)) {
                    ack("update", false, reason);

                    return;
                }

                // Tell the head before rebooting, so it can show something
                // useful rather than just watching telemetry stop.
                ack("update", true, "rebooting");
                Serial1.flush();

                updateMode::requestAndReboot(&reason); // does not return
            }

            if (parser.get("setpoint", value, sizeof(value))) {
                // heater::setSetpointC() does its own range check and refuses
                // anything near the over-temperature limit, so the head cannot
                // ask for something dangerous even if its UI is wrong.
                const double before = heater::setpointC();
                heater::setSetpointC(atof(value));

                ack("setpoint", heater::setpointC() != before || atof(value) == before, "ok");

                return;
            }

            if (parser.get("heater", value, sizeof(value))) {
                const bool want = (atoi(value) != 0);
                const bool done = heater::setEnabled(want);

                ack("heater", done, done ? "ok" : heater::blockedBy());

                return;
            }

            if (parser.get("tare", value, sizeof(value))) {
                weight::startTare();
                ack("tare", true, "ok");

                return;
            }

            if (parser.get("ping", value, sizeof(value))) {
                ack("ping", true, "pong");

                return;
            }

            ack("?", false, "unknown");
        }

    } // namespace

    void init() {
        Serial1.begin(linkproto::kBaud);

        char body[64];
        snprintf(body, sizeof(body), "who=teensy,fw=%s", "0.1.0-phase2");
        send(linkproto::FrameType::Hello, body);

        lastTelemetryMs = millis();
    }

    void loop() {
        // Receive first, so a command issued just before this pass is reflected
        // in the telemetry sent during it.
        while (Serial1.available() > 0) {
            if (!parser.feed(static_cast<char>(Serial1.read()))) {
                continue;
            }

            received++;
            lastRxMs = millis();

            switch (parser.type()) {
                case linkproto::FrameType::Command:
                    handleCommand();
                    break;

                case linkproto::FrameType::Config:
                    handleConfig();
                    break;

                case linkproto::FrameType::Heartbeat:
                case linkproto::FrameType::Hello:
                    // Nothing to do beyond noting that something is out there,
                    // which lastRxMs above already did. Deliberately silent: a
                    // reply would double the traffic to report a fact neither
                    // side acts on.
                    break;

                default:
                    // Telemetry or acks from the head are not expected; ignore
                    // rather than complain, so a future protocol revision that
                    // adds frame types does not flood this console.
                    break;
            }
        }

        const uint32_t now = millis();

        // Push a frame the moment something the OPERATOR is looking at
        // changes, rather than waiting for the next scheduled one.
        //
        // The schedule alone made the panel feel broken: 200 ms of telemetry
        // period plus the head's 250 ms redraw timer is 225 ms of lag on
        // average and 450 ms at worst. That is fine for a boiler temperature,
        // which moves slowly, and far too slow for a knob - turning it and
        // waiting half a second for the screen to agree feels like a fault.
        //
        // Only UI values qualify. Temperatures and flow are not included: they
        // change on every single reading, so triggering on them would simply
        // send continuously and defeat the rate limit.
        //
        // The wire can afford it. A frame is ~110 characters, about 9.5 ms at
        // 115200 baud, so even at the 40 ms floor below the link sits under a
        // quarter utilised.
        const long encoderNow = static_cast<long>(encoder::counter());
        const bool uiChanged = (encoderNow != lastSentEncoder) || (switches::brewRequested() != lastSentBrew);

        const bool scheduled = (now - lastTelemetryMs) >= linkproto::kTelemetryIntervalMs;
        const bool expedited = uiChanged && ((now - lastTelemetryMs) >= kMinTelemetryGapMs);

        if (scheduled || expedited) {
            lastTelemetryMs = now;
            lastSentEncoder = encoderNow;
            lastSentBrew = switches::brewRequested();
            sendTelemetry();
        }
    }

    bool peerPresent() {
        return (received > 0) && ((millis() - lastRxMs) < linkproto::kPeerTimeoutMs);
    }

    uint32_t framesSent() {
        return sent;
    }

    uint32_t framesReceived() {
        return received;
    }

    uint32_t badChecksums() {
        return parser.badChecksums();
    }

    void printStatus() {
        Serial.print("  link (Serial1, TX1=");
        Serial.print(PIN_LINK_TX);
        Serial.print(" RX1=");
        Serial.print(PIN_LINK_RX);
        Serial.print(") : ");
        Serial.print(peerPresent() ? "head PRESENT" : "head absent");
        Serial.print("   sent=");
        Serial.print(sent);
        Serial.print(" recv=");
        Serial.print(received);
        Serial.print(" badsum=");
        Serial.print(parser.badChecksums());
        Serial.print(" malformed=");
        Serial.println(parser.malformed());
    }

} // namespace comms
