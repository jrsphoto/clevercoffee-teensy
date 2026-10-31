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
#include "hardware/outputs.h"
#include "hardware/pinmapping.h"
#include "safetyHandler.h"
#include "tempHandler.h"

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
         */
        const char* stateWord() {
            if (safety::isFaulted()) {
                return "fault";
            }

            if (outputs::isOn(outputs::Channel::Heater)) {
                return "heating";
            }

            if (outputs::isOn(outputs::Channel::Pump)) {
                return "pump";
            }

            return "idle";
        }

        void sendTelemetry() {
            char body[linkproto::kMaxFrame];

            snprintf(body, sizeof(body),
                     "bt=%.1f,gt=%.1f,bok=%d,gok=%d,fl=%.2f,vol=%.1f,enc=%ld,st=%s,flt=%s",
                     temps::boilerC(),
                     temps::groupC(),
                     temps::boilerHealthy() ? 1 : 0,
                     temps::groupHealthy() ? 1 : 0,
                     flow::rateMlPerSec(),
                     flow::totalMl(),
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

        if (const uint32_t now = millis(); (now - lastTelemetryMs) >= linkproto::kTelemetryIntervalMs) {
            lastTelemetryMs = now;
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
