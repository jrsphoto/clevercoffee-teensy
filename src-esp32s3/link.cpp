/**
 * @file link.cpp
 *
 * @brief The head's end of the Teensy link (UART0, IO43/IO44)
 */

#include "link.h"

#include <Arduino.h>
#include <linkProtocol.h>
#include <stdlib.h>

#include "display.h"

namespace machinelink {

    namespace {
        uint32_t hellos = 0;
    }

    namespace {

        linkproto::Parser parser;
        State current;

        uint32_t lastFrameMs = 0;
        uint32_t frames = 0;
        uint32_t lastHeartbeatMs = 0;

        float floatField(const char* key, const float fallback) {
            char buf[24];

            if (!parser.get(key, buf, sizeof(buf))) {
                return fallback;
            }

            return static_cast<float>(atof(buf));
        }

        void applyTelemetry() {
            current.boilerC = floatField("bt", current.boilerC);
            current.groupC = floatField("gt", current.groupC);
            current.flowMlPerSec = floatField("fl", current.flowMlPerSec);
            current.volumeMl = floatField("vol", current.volumeMl);
            current.weightG = floatField("wt", current.weightG);
            current.pumpPercent = floatField("pmp", current.pumpPercent);
            current.setpointC = floatField("sp", current.setpointC);
            current.heaterDuty = floatField("hd", current.heaterDuty);

            char buf[24];

            if (parser.get("bok", buf, sizeof(buf))) {
                current.boilerOk = (buf[0] == '1');
            }

            if (parser.get("gok", buf, sizeof(buf))) {
                current.groupOk = (buf[0] == '1');
            }

            if (parser.get("wok", buf, sizeof(buf))) {
                current.weightOk = (atoi(buf) != 0);
            }

            if (parser.get("hen", buf, sizeof(buf))) {
                current.heaterEnabled = (atoi(buf) != 0);
            }

            if (parser.get("wcal", buf, sizeof(buf))) {
                current.weightCalibrated = (atoi(buf) != 0);
            }

            if (parser.get("enc", buf, sizeof(buf))) {
                current.encoder = atol(buf);
            }

            parser.get("st", current.status, sizeof(current.status));
            parser.get("flt", current.fault, sizeof(current.fault));
        }

    } // namespace

    void init() {
        Serial1.begin(linkproto::kBaud, SERIAL_8N1, display::kPinLinkRx, display::kPinLinkTx);
    }

    void loop() {
        // Announce ourselves periodically so the Teensy can report whether a
        // head is attached. Purely informational on its side - it changes
        // nothing about how the machine behaves.
        if (const uint32_t now = millis(); (now - lastHeartbeatMs) >= linkproto::kHeartbeatIntervalMs) {
            lastHeartbeatMs = now;

            char frame[linkproto::kMaxFrame];
            const size_t n = linkproto::buildFrame(frame, sizeof(frame), linkproto::FrameType::Heartbeat, "who=head");

            if (n > 0) {
                Serial1.write(reinterpret_cast<const uint8_t*>(frame), n);
            }
        }

        while (Serial1.available() > 0) {
            if (!parser.feed(static_cast<char>(Serial1.read()))) {
                continue;
            }

            frames++;
            lastFrameMs = millis();

            switch (parser.type()) {
                case linkproto::FrameType::Telemetry:
                    applyTelemetry();
                    break;

                case linkproto::FrameType::Ack:
                    Serial.printf("link ack: %s\n", parser.body());
                    break;

                case linkproto::FrameType::Hello:
                    // The controller booted. Everything shown until now came
                    // from the previous instance of it.
                    ++hellos;
                    Serial.printf("link hello (controller restarted): %s\n", parser.body());
                    break;

                default:
                    break;
            }
        }
    }

    uint32_t helloCount() {
        return hellos;
    }

    const State& state() {
        return current;
    }

    bool isConnected() {
        return (frames > 0) && ((millis() - lastFrameMs) < linkproto::kPeerTimeoutMs);
    }

    uint32_t msSinceLastFrame() {
        return millis() - lastFrameMs;
    }

    uint32_t framesReceived() {
        return frames;
    }

    uint32_t badChecksums() {
        return parser.badChecksums();
    }

    void sendCommand(const char* body) {
        char frame[linkproto::kMaxFrame];
        const size_t n = linkproto::buildFrame(frame, sizeof(frame), linkproto::FrameType::Command, body);

        if (n > 0) {
            Serial1.write(reinterpret_cast<const uint8_t*>(frame), n);
        }
    }

    void sendConfig(const char* key, const char* value) {
        char body[linkproto::kMaxFrame];

        if (snprintf(body, sizeof(body), "k=%s,v=%s", key, value) >= static_cast<int>(sizeof(body))) {
            // Silently dropping would be worse than saying so: a setting that
            // never arrived looks exactly like one the Teensy ignored.
            Serial.printf("config: key+value too long to send: %s\n", key);

            return;
        }

        char frame[linkproto::kMaxFrame];
        const size_t n = linkproto::buildFrame(frame, sizeof(frame), linkproto::FrameType::Config, body);

        if (n > 0) {
            Serial1.write(reinterpret_cast<const uint8_t*>(frame), n);
        }
    }

} // namespace machinelink
