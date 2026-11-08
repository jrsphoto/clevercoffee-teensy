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
#include "i2cHandler.h"
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

        /**
         * This firmware's version, announced to the head in the Hello frame.
         *
         * AUTO_VERSION comes from auto_firmware_version.py (platformio.ini).
         * The fallback deliberately does not look like a version number: a
         * build that lost the script should say so rather than report
         * something plausible and wrong.
         */
#ifdef AUTO_VERSION
    #define CC_STR_HELPER(x) #x
    #define CC_STR(x) CC_STR_HELPER(x)
        constexpr char kFirmwareVersion[] = CC_STR(AUTO_VERSION);
#else
        constexpr char kFirmwareVersion[] = "unversioned-build";
#endif

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

        /**
         * @brief Say who we are and what version we are running.
         *
         * Sent at our boot, and again whenever the head says hello - it boots
         * much more often than this board does, and its About page has no
         * other way to learn what is running here.
         */
        void sendHello() {
            char body[96];
            snprintf(body, sizeof(body), "who=teensy,fw=%s", kFirmwareVersion);
            send(linkproto::FrameType::Hello, body);
        }

        void sendTelemetry() {
            char body[linkproto::kMaxFrame];

            // wcal says whether the weight is in grams or in raw ADC counts.
            // Sent as its own field rather than inferred at the far end,
            // because the head must never label raw counts as grams - a
            // plausible wrong weight on screen is worse than no weight.
            // c1/c2 are the scale's CALIBRATION FACTORS, and they are here
            // because the controller owns them: they are measured against a
            // real mass on this side, not configured on the other. Reporting
            // them lets the head DISPLAY the true factors instead of keeping a
            // master copy it would push back - which it did once, overwriting
            // a measured calibration with the 1.0 "uncalibrated" default.
            snprintf(body, sizeof(body),
                     "bt=%.1f,gt=%.1f,bok=%d,gok=%d,sp=%.1f,hd=%.0f,hen=%d,fl=%.2f,vol=%.1f,wt=%.1f,wok=%d,wcal=%d,pmp=%.0f,enc=%ld,c1=%.0f,c2=%.0f,bfa=%d,st=%s,flt=%s",
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
                     weight::calFactor(),
                     weight::calFactor2(),
                     shot::backflushArmed() ? 1 : 0,
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

            if (parser.get("i2cscan", value, sizeof(value))) {
                // The Teensy owns the I2C bus, so the head cannot scan it and
                // has to ask. Answered with an Ack carrying the addresses as
                // hex, because the alternative - a new frame type - would need
                // both sides changed for one diagnostic.
                //
                // Synchronous, and that is fine: a full sweep is a few
                // milliseconds of bus traffic, nothing in the control path
                // waits on it, and the heater permit expiry would catch a loop
                // that somehow stalled here.
                uint8_t addresses[i2c::kMaxScanResults];
                const int found = i2c::scanInto(addresses, i2c::kMaxScanResults);
                const int shown = found < i2c::kMaxScanResults ? found : i2c::kMaxScanResults;

                char list[80];
                size_t at = 0;

                for (int i = 0; i < shown && at + 4 < sizeof(list); i++) {
                    at += snprintf(list + at, sizeof(list) - at, "%s%02X", i > 0 ? " " : "", addresses[i]);
                }

                list[at] = '\0';

                ack("i2cscan", true, found > 0 ? list : "none");

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

            // --- Requests from the panel's home grid -----------------------
            //
            // THE HEAD ASKS; THIS SIDE DECIDES. Every one of these goes
            // through the same entry point the brew switch and the console
            // use, so the gates are identical whichever way the request
            // arrived: a latched fault, an unhealthy boiler probe or a shot
            // already running all refuse it here, and the hard time backstop
            // applies regardless.
            //
            // This is the architecture working as written - "a touch is a
            // request; the Teensy decides" - and NOT a new privilege for the
            // display. It cannot drive an output; it can only ask.

            if (parser.get("shot", value, sizeof(value))) {
                const int index = atoi(value);

                if (index < 0 || index >= shot::kProfileCount) {
                    ack("shot", false, "noprofile");

                    return;
                }

                const bool started = shot::startProfile(static_cast<uint8_t>(index));
                ack("shot", started, started ? "ok" : "refused");

                return;
            }

            if (parser.get("stop", value, sizeof(value))) {
                // STOP WHATEVER IS RUNNING. One command rather than the panel
                // picking between shotstop, flush=0 and backflush=0 from the
                // state word it last received.
                //
                // That matters: telemetry is up to a frame old, so a panel
                // deciding which stop to send could choose the wrong one at
                // exactly the moment a sequence changes phase - and the one
                // control nobody can afford to have miss is the one labelled
                // STOP. This side knows what it is actually doing.
                const char* what = "nothing";

                if (shot::isBackflushing()) {
                    shot::stopBackflush("panel stop");
                    what = "backflush";
                }
                else if (shot::isFlushing()) {
                    shot::stopFlush("panel stop");
                    what = "flush";
                }
                else if (shot::isRunning()) {
                    shot::stop("panel stop");
                    what = "shot";
                }

                ack("stop", true, what);

                return;
            }

            if (parser.get("shotstop", value, sizeof(value))) {
                if (!shot::isRunning()) {
                    ack("shotstop", false, "notrunning");

                    return;
                }

                shot::stop("panel");
                ack("shotstop", true, "ok");

                return;
            }

            if (parser.get("flush", value, sizeof(value))) {
                // 1 starts, 0 stops. The flush has its own cap
                // (flush.max_time) which cannot be disabled, so a panel that
                // crashes mid-flush does not leave the pump running.
                if (atoi(value) == 0) {
                    shot::stopFlush("panel");
                    ack("flush", true, "ok");

                    return;
                }

                const bool started = shot::startFlush("panel");
                ack("flush", started, started ? "ok" : "refused");

                return;
            }

            if (parser.get("backflusharm", value, sizeof(value))) {
                // ARMING IS SEPARATE FROM RUNNING, deliberately, and the
                // panel must go through the same two steps as the console.
                // Backflush drives the pump against a blind basket and the
                // MG304 has no internal relief, so "one tap and it runs" is
                // not a convenience worth having.
                const bool want = atoi(value) != 0;
                shot::setBackflushArmed(want);
                ack("backflusharm", true, want ? "armed" : "disarmed");

                return;
            }

            if (parser.get("backflush", value, sizeof(value))) {
                if (atoi(value) == 0) {
                    shot::stopBackflush("panel");
                    ack("backflush", true, "ok");

                    return;
                }

                const bool started = shot::startBackflush("panel");
                ack("backflush", started, started ? "ok" : "refused");

                return;
            }

            ack("?", false, "unknown");
        }

    } // namespace

    void init() {
        Serial1.begin(linkproto::kBaud);

        // ENLARGE THE RECEIVE BUFFER. Teensy's default is 64 bytes, which is
        // about ONE frame of this protocol.
        //
        // That is ample for the head's heartbeat and the occasional command,
        // and badly short for a burst: when the head pushes its whole config
        // at handshake, 26 frames arrive faster than the main loop consumes
        // them and the overflow is discarded silently. Measured, not guessed -
        // 26 frames sent, 19 applied, none refused, badsum and malformed both
        // zero. The missing seven never reached the parser.
        //
        // A dropped config frame is a setting that quietly did not apply,
        // which is the worst way for this to fail: nothing reports an error
        // and the machine simply runs on a different number than the screen
        // shows. 1 kB holds roughly twenty frames, so the loop can fall a long
        // way behind without losing any.
        static uint8_t rxBuffer[1024];
        Serial1.addMemoryForRead(rxBuffer, sizeof(rxBuffer));

        sendHello();

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

                case linkproto::FrameType::Hello:
                    // The head booted and is asking who we are. Answer with
                    // our own Hello, carrying this firmware's version - it has
                    // no other way to find out, since it cannot read this
                    // chip's flash, and we announce ourselves unprompted only
                    // at OUR boot, which is far rarer than a head reflash.
                    //
                    // Safe against a ping-pong: the head stores a Hello and
                    // never replies to one.
                    sendHello();
                    break;

                case linkproto::FrameType::Heartbeat:
                    // Nothing to do beyond noting that something is out there,
                    // which lastRxMs above already did. Deliberately silent:
                    // this one fires every few seconds and a reply would double
                    // the traffic to report a fact neither side acts on.
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
