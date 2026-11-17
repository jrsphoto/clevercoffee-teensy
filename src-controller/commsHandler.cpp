/**
 * @file commsHandler.cpp
 *
 * @brief The link to the ESP32-S3 display head (Serial1, D0/D1)
 */

#include "commsHandler.h"

#include <Arduino.h>
#include <linkProtocol.h>

// Teensyduino's heap bounds, for the free-RAM figure in the diagnostics
// frame. Must be at global scope: declared inside the anonymous namespace
// below they become namespaced symbols and the link fails looking for them.
extern "C" {
    extern unsigned long _heap_end;
    extern char* __brkval;
}

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
         * Writes and returns. the link UART's buffer absorbs it; if the head is absent
         * the bytes simply go nowhere, which is the intended behaviour - the
         * Teensy never waits on the head for anything.
         */
        /**
         * Frames that would not fit and were therefore NOT SENT.
         *
         * This used to be a bare `return` and nothing else. An oversized frame
         * is discarded whole rather than truncated, so the symptom is
         * telemetry silently stopping - and the frame is at its longest
         * exactly when a fault with a long name is latched, which is when the
         * panel going blank is least acceptable.
         *
         * Counted here and reported by printStatus() so it can never be
         * invisible again. If this is non-zero, something in sendTelemetry()
         * or sendDiag() has outgrown linkproto::kMaxBody.
         */
        uint32_t dropped = 0;

        void send(const linkproto::FrameType type, const char* body) {
            char frame[linkproto::kMaxFrame];
            const size_t n = linkproto::buildFrame(frame, sizeof(frame), type, body);

            if (n == 0) {
                if (dropped == 0) {
                    // Once, loudly. Every frame after this would repeat it at
                    // the telemetry rate and bury everything else.
                    Serial.print("link: FRAME TOO LONG, not sent - type ");
                    Serial.print(static_cast<char>(type));
                    Serial.print(", body ");
                    Serial.print(strlen(body));
                    Serial.print(" bytes against a limit of ");
                    Serial.println(linkproto::kMaxBody);
                    Serial.println("      run 'frametest' to see which frame and by how much");
                }

                dropped++;

                return;
            }

            LINK_SERIAL.write(frame, n);
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
        /**
         * The state words, named once.
         *
         * These are literals used by stateWord() below AND scanned by
         * selfTestFrames() to find the longest, so that a new, longer word
         * cannot quietly push the telemetry frame over its limit. Add a word
         * here and use the constant below; do not write a bare literal in
         * stateWord(), or the self-check will measure the wrong worst case.
         */
        namespace stateword {
            constexpr const char* kFault = "fault";
            constexpr const char* kBackflush = "backflush";
            constexpr const char* kFlushing = "flushing";
            constexpr const char* kCoolFlush = "coolflush";
            constexpr const char* kRecovery = "recovery";
            constexpr const char* kPreinfuse = "preinfuse";
            constexpr const char* kSoak = "soak";
            constexpr const char* kBrewing = "brewing";
            constexpr const char* kHeating = "heating";
            constexpr const char* kPump = "pump";
            constexpr const char* kIdle = "idle";

            constexpr const char* kAll[] = {
                kFault, kBackflush, kFlushing, kCoolFlush, kRecovery,
                kPreinfuse, kSoak, kBrewing, kHeating, kPump, kIdle,
            };
        } // namespace stateword

        const char* stateWord() {
            if (safety::isFaulted()) {
                return stateword::kFault;
            }

            if (shot::isBackflushing()) {
                return stateword::kBackflush;
            }

            if (shot::isFlushing()) {
                return stateword::kFlushing;
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
                        return stateword::kCoolFlush;

                    case shot::State::Recovery:
                        return stateword::kRecovery;

                    case shot::State::Preinfusion:
                        return stateword::kPreinfuse;

                    case shot::State::PreinfusionPause:
                        return stateword::kSoak;

                    default:
                        return stateword::kBrewing;
                }
            }

            // heater::isHeating(), NOT outputs::isOn(Channel::Heater). The
            // heater is driven at the pin by the ISR and never passes through
            // the outputs module, so this test was false even at full power
            // and the panel could NEVER show "heating". Found in review
            // 2026-11-17, alongside the same mistake in the update gate.
            if (heater::isHeating()) {
                return stateword::kHeating;
            }

            if (pump::isRunning()) {
                return stateword::kPump;
            }

            return stateword::kIdle;
        }

        /** @brief Longest state word, for the frame self-check. */
        const char* longestStateWord() {
            const char* longest = stateword::kIdle;

            for (const char* word : stateword::kAll) {
                if (strlen(word) > strlen(longest)) {
                    longest = word;
                }
            }

            return longest;
        }

        /**
         * @brief Longest fault name, for the frame self-check.
         *
         * Walks the whole FaultCode enum rather than naming a winner, so
         * adding a fault with a longer name is caught by the self-check
         * instead of by the panel going blank the first time it latches.
         */
        const char* longestFaultName() {
            // FaultCode and safetyFaultName are at GLOBAL scope in
            // safetyHandler.h, not inside namespace safety.
            const char* longest = safetyFaultName(FaultCode::None);

            for (uint8_t code = 0; code <= static_cast<uint8_t>(FaultCode::WatchdogReset); code++) {
                const char* name = safetyFaultName(static_cast<FaultCode>(code));

                if (strlen(name) > strlen(longest)) {
                    longest = name;
                }
            }

            return longest;
        }

        /** @brief Longest pump refusal string, for the frame self-check. */
        const char* longestPumpRefusal() {
            // The three pumpHandler can produce. The head's buffer is 24, so
            // anything longer than 23 would be dropped by its parser as well
            // as bloating the frame.
            return "pump not initialised";
        }

        /**
         * @brief Report whether one frame's worst case fits, and by how much.
         *
         * @return true if it fits.
         */
        bool reportFrameFit(const char* name, const size_t bodyLength) {
            const bool fits = linkproto::bodyFits(bodyLength);

            Serial.print("link: worst-case ");
            Serial.print(name);
            Serial.print(" body ");
            Serial.print(bodyLength);
            Serial.print(" of ");
            Serial.print(linkproto::kMaxBody);

            if (fits) {
                Serial.print(" bytes, ");
                Serial.print(linkproto::kMaxBody - bodyLength);
                Serial.println(" spare");
            }
            else {
                Serial.print(" bytes - OVER BY ");
                Serial.print(bodyLength - linkproto::kMaxBody);
                Serial.println(", THIS FRAME WILL NOT BE SENT");
            }

            return fits;
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

        /**
         * @brief Free heap, for the diagnostics frame.
         *
         * Teensyduino's allocator grows the heap upward from __brkval toward
         * the top of RAM2, so the gap between them is what is left. There is
         * no library call for this on Teensy 4.
         *
         * The two symbols are declared at file scope above, OUTSIDE this
         * anonymous namespace. Declared in here they become
         * `comms::{anonymous}::__brkval` and the link fails.
         */
        uint32_t freeHeapBytes() {
            return static_cast<uint32_t>(
                reinterpret_cast<char*>(&_heap_end) - __brkval);
        }

        uint32_t lastDiagMs = 0;

        /**
         * Diagnostics are OFF until someone asks for them.
         *
         * John's call, 2026-11-15. Nothing needs this frame to run the
         * machine, so the wire stays quiet unless a person has the Diags page
         * open. Not persisted either: a reboot goes back to quiet, because an
         * unattended machine sending diagnostics to nobody is pure cost.
         */
        bool diagEnabled = false;

        /**
         * A diagnostics output HOLD, from the Diags page.
         *
         * One channel at a time, on only while the browser keeps saying so.
         * `holdUntilMs` is refreshed by each request and the output drops the
         * moment it lapses, so a closed tab, a dropped network or a crashed
         * browser all end it. The same pattern as the heater permit and the
         * pump's speed command: staying on requires continuous proof that
         * somebody is still there.
         *
         * kHoldLapseMs is deliberately several times the browser's repeat
         * interval, so ordinary jitter does not make a held button stutter.
         */
        constexpr uint32_t kHoldLapseMs = 1000;

        bool holdActive = false;
        outputs::Channel holdChannel = outputs::Channel::Valve;
        uint32_t holdUntilMs = 0;

        void releaseHold() {
            if (!holdActive) {
                return;
            }

            outputs::set(holdChannel, false);
            holdActive = false;
        }

        /**
         * @brief Is it safe to hand an output to the Diags page right now?
         *
         * No, if anything else is driving the machine. A shot owns the group
         * valve and the pump, and a hold expiring mid-shot would shut the
         * valve under it. Diagnostics are for an idle machine.
         */
        bool holdAllowed() {
            return !safety::isFaulted() && !shot::isRunning() && !shot::isFlushing()
                   && !shot::isBackflushing();
        }

        /**
         * @brief Diagnostics, once a second. Read by a person, not by the loop.
         *
         * Everything here is either a state the live telemetry has no room
         * for, or a counter that only matters when something is wrong. See
         * the note on FrameType::Diag for why this is a separate frame.
         *
         * `o` is a bitmask rather than four fields purely to save wire: bit 0
         * heater, 1 group valve, 2 fill, 3 cold inlet, matching
         * outputs::Channel order.
         */
        void sendDiag() {
            char body[linkproto::kMaxFrame];

            uint8_t outs = 0;

            for (uint8_t i = 0; i < outputs::kChannelCount; i++) {
                if (outputs::isOn(static_cast<outputs::Channel>(i))) {
                    outs |= static_cast<uint8_t>(1u << i);
                }
            }

            // enc lives HERE rather than in telemetry. Its only two consumers
            // on the head are /api/status and the Diags page, both
            // diagnostics, and printed at full int32 width it cost up to 16
            // bytes of a telemetry frame that was one byte from overflowing.
            snprintf(body, sizeof(body),
                     "o=%u,bsw=%u,wtr=%u,up=%lu,ba=%lu,ga=%lu,fp=%lu,fx=%lu,"
                     "ram=%lu,ot=%.0f,enc=%ld,pmpr=%s",
                     outs,
                     switches::rawLevel(),
                     static_cast<unsigned>(digitalReadFast(PIN_WATERTANKSENSOR)),
                     static_cast<unsigned long>(millis() / 1000UL),
                     static_cast<unsigned long>(temps::boilerAgeMs()),
                     static_cast<unsigned long>(temps::groupAgeMs()),
                     static_cast<unsigned long>(flow::pulseCount()),
                     static_cast<unsigned long>(flow::rejectedCount()),
                     static_cast<unsigned long>(freeHeapBytes()),
                     temps::overTempLimitC(),
                     static_cast<long>(encoder::counter()),
                     pump::lastRefusal());

            send(linkproto::FrameType::Diag, body);
        }

        /**
         * @brief The six booleans, packed into one byte.
         *
         * Bit meanings live in linkProtocol.h so both firmwares read them from
         * the same place. The weight-calibrated bit matters most: it says
         * whether `wt` is GRAMS or raw ADC counts, and the head must never
         * label raw counts as grams, because a plausible wrong weight on
         * screen is worse than no weight.
         */
        uint8_t telemetryFlags() {
            uint8_t flags = 0;

            if (temps::boilerHealthy()) {
                flags |= linkproto::kFlagBoilerOk;
            }

            if (temps::groupHealthy()) {
                flags |= linkproto::kFlagGroupOk;
            }

            if (heater::enabled()) {
                flags |= linkproto::kFlagHeaterEnabled;
            }

            if (weight::healthy()) {
                flags |= linkproto::kFlagWeightOk;
            }

            if (weight::calFactor() != 1.0f) {
                flags |= linkproto::kFlagWeightCal;
            }

            if (shot::backflushArmed()) {
                flags |= linkproto::kFlagBackflushArmed;
            }

            return flags;
        }

        void sendTelemetry() {
            char body[linkproto::kMaxFrame];

            // c1/c2 are the scale's CALIBRATION FACTORS, and they are here
            // because the controller owns them: they are measured against a
            // real mass on this side, not configured on the other. Reporting
            // them lets the head DISPLAY the true factors instead of keeping a
            // master copy it would push back - which it did once, overwriting
            // a measured calibration with the 1.0 "uncalibrated" default.
            //
            // They stay in TELEMETRY rather than moving to the diag frame,
            // even though they change only when somebody runs scalecal: the
            // head's config layer reads them through addControllerOwned(), and
            // diagnostics are off unless the Diags page is open, so moving
            // them would leave the config layer with nothing most of the time.
            snprintf(body, sizeof(body),
                     "bt=%.1f,gt=%.1f,sp=%.1f,hd=%.0f,fl=%.2f,vol=%.1f,wt=%.1f,pmp=%.0f,c1=%.0f,c2=%.0f,ok=%02X,st=%s,flt=%s",
                     temps::boilerC(),
                     temps::groupC(),
                     heater::setpointC(),
                     heater::dutyPercent(),
                     flow::rateMlPerSec(),
                     flow::totalMl(),
                     weight::grams(),
                     pump::actualSpeed(),
                     weight::calFactor(),
                     weight::calFactor2(),
                     telemetryFlags(),
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

            if (parser.get("hold", value, sizeof(value))) {
                // Body is `hold=<channel>,on=<0|1>`. THE HEATER IS NOT
                // AVAILABLE HERE and must not be added: outputs::set checks
                // the fault latch and the boiler sensor, but not the dry-fire
                // interlock, so a heater hold would energize the element
                // without the water check. John's call, 2026-11-15.
                outputs::Channel ch = outputs::Channel::Valve;
                bool known = true;

                if (strcmp(value, "valve") == 0) {
                    ch = outputs::Channel::Valve;
                }
                else if (strcmp(value, "fill") == 0) {
                    ch = outputs::Channel::Fill;
                }
                else if (strcmp(value, "inlet") == 0) {
                    ch = outputs::Channel::Inlet;
                }
                else {
                    known = false;
                }

                char on[8] = "0";
                parser.get("on", on, sizeof(on));

                if (!known) {
                    ack("hold", false, "unknown channel");

                    return;
                }

                if (atoi(on) == 0) {
                    releaseHold();
                    ack("hold", true, "released");

                    return;
                }

                if (!holdAllowed()) {
                    releaseHold();
                    ack("hold", false, "machine busy or faulted");

                    return;
                }

                // Switching channel mid-hold releases the old one first, so
                // two outputs can never be held at once.
                if (holdActive && ch != holdChannel) {
                    releaseHold();
                }

                holdChannel = ch;
                holdUntilMs = millis() + kHoldLapseMs;
                holdActive = true;

                ack("hold", outputs::set(ch, true), "held");

                return;
            }

            if (parser.get("diag", value, sizeof(value))) {
                diagEnabled = (atoi(value) != 0);

                // Send one immediately on switch-on so the page fills in
                // rather than sitting blank for up to a second.
                if (diagEnabled) {
                    sendDiag();
                }

                ack("diag", true, diagEnabled ? "on" : "off");

                return;
            }

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
                LINK_SERIAL.flush();

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

        /**
         * The link's receive buffer, and the ONLY way the port is opened.
         *
         * ENLARGED because Teensy's default is 64 bytes, which is about ONE
         * frame of this protocol. That is ample for the head's heartbeat and
         * the occasional command, and badly short for a burst: when the head
         * pushes its whole config at handshake, 26 frames arrive faster than
         * the main loop consumes them and the overflow is discarded silently.
         * Measured, not guessed - 26 frames sent, 19 applied, none refused,
         * badsum and malformed both zero. The missing seven never reached the
         * parser.
         *
         * A dropped config frame is a setting that quietly did not apply,
         * which is the worst way for this to fail: nothing reports an error
         * and the machine simply runs on a different number than the screen
         * shows. 1 kB holds roughly twenty frames.
         *
         * THIS FUNCTION EXISTS SO THE BUFFER CANNOT BE FORGOTTEN. `rxpin`
         * stops the port to test the pad as a GPIO and restarts it afterwards,
         * and it used to restart it with a bare begin() - dropping straight
         * back to 64 bytes, with the next handshake silently losing settings
         * and nothing anywhere to say so. Found in review 2026-11-17. Never
         * call LINK_SERIAL.begin() directly; call this.
         */
        uint8_t rxBuffer[1024];

        void beginLink() {
            LINK_SERIAL.begin(linkproto::kBaud);
            LINK_SERIAL.addMemoryForRead(rxBuffer, sizeof(rxBuffer));
        }

    } // namespace

    void init() {
        beginLink();

        // Measure the frames before sending any. Costs a few hundred
        // microseconds once, and means an overlong frame is reported at boot
        // rather than discovered by the panel going blank during a fault.
        selfTestFrames();

        sendHello();

        lastTelemetryMs = millis();
    }

    void loop() {
        // Receive first, so a command issued just before this pass is reflected
        // in the telemetry sent during it.
        while (LINK_SERIAL.available() > 0) {
            if (!parser.feed(static_cast<char>(LINK_SERIAL.read()))) {
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

        // Diagnostics, well after the telemetry above so the two do not
        // collide in the same pass any more often than they have to.
        // A hold that stopped being refreshed drops its output. Checked every
        // pass rather than on a timer: this is the thing that makes the button
        // momentary, so it must not wait for anything else.
        if (holdActive && static_cast<int32_t>(now - holdUntilMs) >= 0) {
            releaseHold();
        }

        if (diagEnabled && (now - lastDiagMs) >= 1000) {
            lastDiagMs = now;
            sendDiag();
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

    void sniffRaw(const uint32_t windowMs) {
        // Cap it: this consumes link traffic, so it must not be possible to
        // leave the machine deaf for minutes by typing a large number.
        const uint32_t window = (windowMs == 0 || windowMs > 15000) ? 3000 : windowMs;

        Serial.print("link raw: listening on the link UART (RX=");
        Serial.print(PIN_LINK_RX);
        Serial.print(") for ");
        Serial.print(window);
        Serial.println(" ms - frames arriving during this are CONSUMED");

        constexpr size_t kDumpMax = 160;
        uint8_t dump[kDumpMax];
        size_t dumped = 0;
        uint32_t total = 0;
        uint32_t printableRun = 0;

        const uint32_t start = millis();

        while ((millis() - start) < window) {
            // The watchdog is 4 s and a useful window is longer than that.
            safetyWatchdogFeed();

            while (LINK_SERIAL.available() > 0) {
                const auto b = static_cast<uint8_t>(LINK_SERIAL.read());

                total++;

                if (b >= 0x20 && b < 0x7F) {
                    printableRun++;
                }

                if (dumped < kDumpMax) {
                    dump[dumped++] = b;
                }
            }
        }

        Serial.print("link raw: ");
        Serial.print(total);
        Serial.print(" bytes in ");
        Serial.print(window);
        Serial.println(" ms");

        if (total == 0) {
            // The decisive result. Nothing is reaching this pin at all, so the
            // fault is upstream of the parser: the other board is not
            // transmitting, or the signal is not getting here. No amount of
            // protocol work will change it.
            Serial.println("          NOTHING on the wire. Not a framing problem -");
            Serial.println("          no bytes are reaching RX1 at all.");

            return;
        }

        Serial.print("          ");
        Serial.print(printableRun);
        Serial.print(" of them printable ASCII (the protocol is ASCII, so a healthy");
        Serial.println(" link is nearly all printable)");

        Serial.print("          first ");
        Serial.print(dumped);
        Serial.println(" bytes:");
        Serial.print("          ");

        for (size_t i = 0; i < dumped; i++) {
            const uint8_t b = dump[i];

            if (b == '\n') {
                Serial.println();
                Serial.print("          ");
            }
            else if (b >= 0x20 && b < 0x7F) {
                Serial.write(b);
            }
            else {
                Serial.print("<");
                Serial.print(b, HEX);
                Serial.print(">");
            }
        }

        Serial.println();
    }

    void probeRxPin(const uint32_t windowMs) {
        const uint32_t window = (windowMs == 0 || windowMs > 15000) ? 3000 : windowMs;

        Serial.print("rxpin: taking pin ");
        Serial.print(PIN_LINK_RX);
        Serial.println(" away from the link UART and looking at it directly");

        LINK_SERIAL.end();

        // 1. Is the pin FREE, or is something holding it?
        //
        // With the internal pull-up a free pin reads HIGH; with the pull-down
        // it reads LOW. A pin that reads the same both ways is being driven
        // by something - which is exactly the trap BENCH.md records, where a
        // logic analyser's PWM output left wired to a Teensy pin idled low
        // and shorted it to ground.
        pinMode(PIN_LINK_RX, INPUT_PULLUP);
        delayMicroseconds(500);
        const int withPullup = digitalReadFast(PIN_LINK_RX);

        pinMode(PIN_LINK_RX, INPUT_PULLDOWN);
        delayMicroseconds(500);
        const int withPulldown = digitalReadFast(PIN_LINK_RX);

        Serial.print("rxpin: pull-up reads ");
        Serial.print(withPullup ? "HIGH" : "LOW");
        Serial.print(", pull-down reads ");
        Serial.println(withPulldown ? "HIGH" : "LOW");

        if (withPullup == 1 && withPulldown == 0) {
            Serial.println("       -> the pin FOLLOWS the pull, so it is FREE.");
            Serial.println("          Nothing is driving it. An idle UART line should");
            Serial.println("          be held HIGH by the other board, so a free-floating");
            Serial.println("          pin means nothing is connected to it.");
        }
        else if (withPullup == withPulldown) {
            Serial.print("       -> HELD ");
            Serial.print(withPullup ? "HIGH" : "LOW");
            Serial.println(" against both pulls. Something is driving this pin.");
            Serial.println("          If that something is not the display head, find it:");
            Serial.println("          a stray lead shorting the pin will look exactly like");
            Serial.println("          a dead link.");
        }

        // 2. Does the other board's signal actually ARRIVE?
        //
        // Run the head's `txtest` while this is counting. Edges here means the
        // wire is good and the fault is in how the UART is set up; no edges
        // means the signal is not reaching this pad at all.
        pinMode(PIN_LINK_RX, INPUT_PULLUP);

        Serial.print("rxpin: counting edges for ");
        Serial.print(window);
        Serial.println(" ms - run 'txtest' on the HEAD now");

        uint32_t edges = 0;
        int last = digitalReadFast(PIN_LINK_RX);
        const uint32_t start = millis();

        while ((millis() - start) < window) {
            safetyWatchdogFeed();

            for (int i = 0; i < 2000; i++) {
                const int now = digitalReadFast(PIN_LINK_RX);

                if (now != last) {
                    last = now;
                    edges++;
                }
            }
        }

        Serial.print("rxpin: ");
        Serial.print(edges);
        Serial.println(" edges seen");

        if (edges == 0) {
            Serial.println("       -> the signal is NOT reaching this pad. Not firmware.");
        }
        else {
            Serial.println("       -> the wire is GOOD. The signal arrives; the fault is");
            Serial.println("          in how the UART is configured, not in the wiring.");
        }

        // Give the pin back - through beginLink(), which reattaches the 1 kB
        // receive buffer. A bare begin() here left the port on Teensy's stock
        // 64 bytes, so the next config handshake could silently lose frames.
        beginLink();
        Serial.println("rxpin: link UART restored, with its 1 kB buffer");
    }

    void printStatus() {
        Serial.print("  link (LINK_SERIAL, TX1=");
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
        Serial.print(parser.malformed());
        Serial.print(" dropped=");
        Serial.println(dropped);

        if (dropped > 0) {
            Serial.println("    DROPPED frames were too long to send - run 'frametest'");
        }
    }

    uint32_t droppedFrames() {
        return dropped;
    }

    /**
     * @brief Build each outgoing frame at its WORST case and report the room left.
     *
     * The point is that nobody should ever again work this out by counting
     * characters. The limit is applied twice on the way out, the longest
     * fault name and the widest numbers never occur together in normal
     * running, and the failure mode is a frame that silently is not sent. So
     * this measures it instead, at every boot and on demand.
     *
     * Extreme values on purpose: temperatures as a wide negative, the longest
     * state word and the longest fault name, counters at full width. If this
     * says a frame does not fit, it does not fit in the field either - it is
     * just that the field would wait for a bad day to tell you.
     */
    void selfTestFrames() {
        char body[linkproto::kMaxFrame];
        bool allFit = true;

        // Telemetry, worst case.
        snprintf(body, sizeof(body),
                 "bt=%.1f,gt=%.1f,sp=%.1f,hd=%.0f,fl=%.2f,vol=%.1f,wt=%.1f,pmp=%.0f,c1=%.0f,c2=%.0f,ok=%02X,st=%s,flt=%s",
                 -999.9, -999.9, 999.9, 100.0, 99.99, 99999.9, -9999.9, 100.0,
                 999999.0, 999999.0, 0xFFu, longestStateWord(), longestFaultName());

        allFit &= reportFrameFit("telemetry", strlen(body));

        // Diag, worst case.
        snprintf(body, sizeof(body),
                 "o=%u,bsw=%u,wtr=%u,up=%lu,ba=%lu,ga=%lu,fp=%lu,fx=%lu,"
                 "ram=%lu,ot=%.0f,enc=%ld,pmpr=%s",
                 255u, 1u, 1u, 4294967295UL, 4294967295UL, 4294967295UL,
                 4294967295UL, 4294967295UL, 4294967295UL, 999.0,
                 -2147483647L, longestPumpRefusal());

        allFit &= reportFrameFit("diag", strlen(body));

        if (!allFit) {
            Serial.println("link: AT LEAST ONE FRAME CANNOT BE SENT AT ITS WORST CASE.");
            Serial.println("      It will work until the values get extreme, then vanish.");
            Serial.println("      Shorten a field or move one to another frame.");
        }
    }

} // namespace comms
