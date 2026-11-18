/**
 * @file net.cpp
 *
 * @brief WiFi and the web server on the display head. See net.h.
 */

#include "net.h"

#include <Arduino.h>
#include <cmath>

// INCLUDE ORDER MATTERS HERE, and not alphabetically.
//
// WiFiManager pulls in the ESP-IDF's nghttp http_parser.h, whose http_method
// enum declares HTTP_GET, HTTP_POST and friends - the same names
// ESPAsyncWebServer uses for its WebRequestMethod enum. Whichever comes second
// loses, with a wall of "conflicts with a previous declaration".
//
// WiFiManager first is the order upstream uses in its main.cpp, and it is the
// one that builds. Do not tidy these into alphabetical order.
#include <WiFiManager.h>

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WiFi.h>

#include "ParameterRegistry.h"

#include "display.h"
#include "headConfig.h"
#include "link.h"

namespace net {

    namespace {

        /**
         * Access point name, and the password for it.
         *
         * Upstream's defaults (defaults.h: hostname "clevercoffee", WM_PASS
         * "CleverCoffee"). Kept rather than changed so that anyone who has set
         * up an upstream machine finds the same thing here.
         *
         * The AP is password-protected on purpose. It is briefly a network that
         * can configure a machine with a 1500 W heater on it, and "it is only
         * up for a minute" is not a reason to leave that open.
         */
        constexpr char kHostname[] = "clevercoffee";
        constexpr char kApPassword[] = "CleverCoffee";

        /**
         * This firmware's version, for the About page.
         *
         * AUTO_VERSION is defined by auto_firmware_version.py (see
         * platformio.ini) as VERSION.txt + git branch + commit. The fallback is
         * deliberately not a plausible-looking number: a build that lost the
         * version script should say so, not invent "0.0.0".
         */
#ifdef AUTO_VERSION
    #define CC_STR_HELPER(x) #x
    #define CC_STR(x) CC_STR_HELPER(x)
        constexpr char kHeadVersion[] = CC_STR(AUTO_VERSION);
#else
        constexpr char kHeadVersion[] = "unknown (built without the version script)";
#endif

        /** Where this fork lives. Shown on the About page. */
        constexpr char kRepositoryUrl[] = "https://github.com/jrsphoto/clevercoffee-teensy";

        /** Upstream's values: 10 s to connect, 3 tries. 5 s proved too short. */
        constexpr uint8_t kConnectTimeoutSec = 10;
        constexpr uint8_t kConnectRetries = 3;

        /** How long the portal stays up before giving up and running offline. */
        constexpr uint16_t kPortalTimeoutSec = 180;

        /** NVS flag: open the portal on the next boot. See the header. */
        constexpr char kPrefNamespace[] = "ccnet";
        constexpr char kPrefPortalKey[] = "portal";

        Preferences prefs;

        AsyncWebServer server(80);
        WiFiManager wm;

        /**
         * Server-sent events, upstream's path and upstream's event name.
         *
         * This is how the home page gets anything at all: frontend/js/temp.js
         * subscribes here, writes each `new_temps` event into the big
         * temperature readout, and appends it to both charts. Without it the
         * page draws its cards and then sits there empty, which is exactly what
         * it did.
         */
        AsyncEventSource events("/events");

        State current = State::Idle;
        /** Are the web PAGES on the filesystem? See mountFilesystem(). */
        bool fsMounted = false;

        /** Did LittleFS mount at all? True with config.json and no pages. */
        bool fsPresent = false;
        bool listening = false;
        uint32_t requests = 0;

        /**
         * What we last asked the controller to do about diagnostics.
         *
         * The controller does not report whether its diagnostics are on, so
         * this is a record of the request, not proof. The page shows it as
         * the switch position; whether frames are actually arriving is
         * answered separately by controllerSeen and diagAgeMs, which come
         * from the frames themselves.
         */
        bool diagRequested = false;

        char addressBuf[24] = "-";
        char networkBuf[40] = "-";

        void setAddress(const IPAddress& ip) {
            snprintf(addressBuf, sizeof(addressBuf), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
        }

        // -------------------------------------------------------------------
        // Temperature history for the charts
        //
        // Upstream's scheme, kept: a round-robin of int16 hundredths, one
        // record every other sample, so twenty minutes costs 3.6 kB of static
        // RAM rather than a growing allocation on a board that also holds a
        // framebuffer. See sendTempEvent() in src/embeddedWebserver.h.
        //
        // THE ONE DIVERGENCE: a sample whose sensor was unhealthy is stored as
        // kNoReading and serialised as JSON null, not as a number. uPlot draws
        // a gap for null, which is the truth. Upstream has no concept of a bad
        // reading here and would plot whatever the variable happened to hold -
        // a flat line at some plausible temperature is the worst possible way
        // to show a dead RTD. This matches the rule the rest of the firmware
        // follows: a stale reading is NO reading, never the last good value.
        // -------------------------------------------------------------------

        // THE CADENCE IS ONE RECORD EVERY THREE SECONDS, NOT TWO, so 600
        // records is half an hour of history rather than the twenty minutes
        // the comment here used to claim. sampleAndPublish() is called once a
        // second and records when `skippedValues % kSecondsToSkip == 0` with
        // skippedValues starting at zero, which skips two seconds and keeps
        // the third. frontend/js/temp.js spaces its x-axis by `3*i` seconds to
        // match, so the charts have always been drawn correctly - only the
        // comment was wrong. Upstream counts the same way.
        constexpr int kHistoryLength = 600;    ///< 30 minutes at one record / 3 s
        constexpr int kSecondsToSkip = 2;      ///< upstream's decimation
        constexpr int16_t kNoReading = INT16_MIN;

        int16_t tempHistory[3][kHistoryLength] = {};
        int historyCurrentIndex = 0;
        int historyValueCount = 0;
        int skippedValues = 0;

        /** Last time a sample was taken. Upstream samples once a second. */
        uint32_t lastSampleMs = 0;
        constexpr uint32_t kSampleIntervalMs = 1000;

        int mod(const int a, const int b) {
            return (a % b + b) % b;
        }

        /** Hundredths for storage, or kNoReading when there is no reading. */
        int16_t encodeSample(const float value, const bool ok) {
            if (!ok) {
                return kNoReading;
            }

            return static_cast<int16_t>(value * 100.0f);
        }

        /** One history series as a JSON array, with null for missing samples. */
        void printHistorySeries(AsyncResponseStream* response, const int series) {
            bool first = true;

            for (int i = mod(historyCurrentIndex - historyValueCount, kHistoryLength);
                 i != mod(historyCurrentIndex, kHistoryLength);
                 i = mod(i + 1, kHistoryLength)) {
                if (!first) {
                    response->print(',');
                }

                first = false;

                if (tempHistory[series][i] == kNoReading) {
                    response->print("null");
                }
                else {
                    response->print(tempHistory[series][i] * 0.01f, 2);
                }
            }
        }

        /**
         * @brief The three live numbers, in the shape temp.js expects.
         *
         * Nulls rather than zeroes for anything unknown, for the reason given
         * above the history buffer. temp.js is this fork's own file and renders
         * a null as "--" instead of throwing on .toFixed() - the two changes go
         * together.
         */
        void buildTempJson(char* out, const size_t len) {
            const auto& st = machinelink::state();
            const bool linkOk = machinelink::isConnected();
            const bool boilerOk = linkOk && st.boilerOk;

            char boilerText[12] = "null";
            char targetText[12] = "null";
            char dutyText[12] = "null";

            if (boilerOk) {
                snprintf(boilerText, sizeof(boilerText), "%.2f", st.boilerC);
            }

            // The setpoint and the duty are the controller's own numbers rather
            // than sensor readings, so the link being up is the only condition.
            if (linkOk) {
                snprintf(targetText, sizeof(targetText), "%.2f", st.setpointC);
                snprintf(dutyText, sizeof(dutyText), "%.2f", st.heaterDuty);
            }

            snprintf(out, len, R"({"currentTemp":%s,"targetTemp":%s,"heaterPower":%s})",
                     boilerText, targetText, dutyText);
        }

        /**
         * @brief Take one sample: record it, and push it to any open browser.
         *
         * Called once a second from loop(). Reads ONLY what the link last
         * reported and does not recompute anything - same discipline as
         * telemetryJson().
         *
         * When the link is down every field is unknown, not zero. A browser
         * must not be shown a confident 0.0 C because a cable fell out.
         */
        void sampleAndPublish() {
            const auto& st = machinelink::state();
            const bool linkOk = machinelink::isConnected();

            // The boiler is the "current temperature" the home page shows, as
            // upstream does. The group probe has its own readout on the panel.
            const bool boilerOk = linkOk && st.boilerOk;
            const int16_t boiler = encodeSample(st.boilerC, boilerOk);
            const int16_t target = encodeSample(st.setpointC, linkOk);
            const int16_t duty = encodeSample(static_cast<float>(st.heaterDuty), linkOk);

            if (skippedValues > 0 && skippedValues % kSecondsToSkip == 0) {
                tempHistory[0][historyCurrentIndex] = boiler;
                tempHistory[1][historyCurrentIndex] = target;
                tempHistory[2][historyCurrentIndex] = duty;

                historyCurrentIndex = (historyCurrentIndex + 1) % kHistoryLength;

                // Caps at 599, not 600, so one of the stored records is never
                // served. That is upstream's code and it is REQUIRED by the
                // loop in printHistorySeries(): at a count of 600 its start
                // index equals its end index and it would emit nothing at all.
                // Leave it.
                historyValueCount = min(kHistoryLength - 1, historyValueCount + 1);
                skippedValues = 0;
            }
            else {
                skippedValues++;
            }

            if (events.count() == 0) {
                return;
            }

            char body[128];
            buildTempJson(body, sizeof(body));

            events.send(body, "new_temps", millis());
        }

        /**
         * @brief Telemetry as JSON, for the browser.
         *
         * Built from what the link last reported, and it says so: `linkOk`
         * false means these numbers are STALE, not that the machine has a
         * problem. A browser showing confident numbers from a dead link would
         * be the same lie the display is careful not to tell.
         *
         * Deliberately does not reproduce the Teensy's safety logic or
         * recompute anything. It reports what the controller said, and nothing
         * more.
         */
        String telemetryJson() {
            const auto& st = machinelink::state();

            JsonDocument doc;

            doc["linkOk"] = machinelink::isConnected();
            doc["msSinceFrame"] = machinelink::msSinceLastFrame();

            doc["boilerC"] = st.boilerC;
            doc["boilerOk"] = st.boilerOk;
            doc["groupC"] = st.groupC;
            doc["groupOk"] = st.groupOk;
            doc["setpointC"] = st.setpointC;
            doc["heaterDuty"] = st.heaterDuty;
            doc["heaterEnabled"] = st.heaterEnabled;

            doc["flowMlPerSec"] = st.flowMlPerSec;
            doc["volumeMl"] = st.volumeMl;
            doc["pumpPercent"] = st.pumpPercent;

            // weightCalibrated is sent alongside the weight rather than being
            // folded into it, for the reason given in link.h: an uncalibrated
            // scale reported as grams is a plausible wrong number, which is
            // worse than no number.
            doc["weight"] = st.weightG;
            doc["weightOk"] = st.weightOk;
            doc["weightCalibrated"] = st.weightCalibrated;

            // The controller's calibration factors. Reported, never configured
            // here - see link.h. Null before the first frame rather than 0,
            // which would read as a real (and impossible) factor.
            if (isnan(st.calFactor1)) {
                doc["calFactor1"] = nullptr;
            }
            else {
                doc["calFactor1"] = st.calFactor1;
            }

            if (isnan(st.calFactor2)) {
                doc["calFactor2"] = nullptr;
            }
            else {
                doc["calFactor2"] = st.calFactor2;
            }

            doc["status"] = st.status;
            doc["fault"] = st.fault;

            // The encoder count rides in the DIAG frame, which only flows
            // while diagnostics are switched on. Null rather than a stale or
            // zero count when none has arrived, for the same reason as the
            // calibration factors above: a number nobody can vouch for is
            // worse than an admitted gap.
            if (machinelink::diag().everReceived) {
                doc["encoder"] = machinelink::diag().encoder;
            }
            else {
                doc["encoder"] = nullptr;
            }

            String out;
            serializeJson(doc, out);

            return out;
        }

        /**
         * @brief Substitute %TOKEN% in served HTML.
         *
         * Ported from upstream's staticProcessor (embeddedWebserver.h). Their
         * pages are assembled from fragments: index.html opens with %HEADER%,
         * replaced by the contents of html_fragments/header.html.
         *
         * Without this the browser receives a literal "%HEADER%" and the page
         * has no navigation - which is what happened on the first attempt, and
         * looked like a broken page rather than a missing short function.
         *
         * Upstream's VAR_SHOW_ branch is NOT ported: it answers from their
         * ParameterRegistry, which does not exist on this board yet. Those
         * tokens resolve to empty for now, the same as any unknown token, and
         * this grows a branch when the parameters work lands.
         *
         * The heap check is upstream's and worth keeping: a fragment is read
         * whole into a String, and on a board already running WiFi, a web
         * server and a 480x320 framebuffer, "read an arbitrary file into RAM"
         * deserves a guard.
         */
        String templateProcessor(const String& var) {
            // Upstream's VAR_SHOW_* convention, answered here rather than from
            // a parameter registry. The About page's Version card is built
             // from these; before they existed the token resolved to empty and
            // the card was simply blank.
            if (var == "VAR_SHOW_PRODUCT") {
                return "CleverCoffee Teensy";
            }

            if (var == "VAR_SHOW_BOARDS") {
                return "Teensy 4.1 controller + ESP32-S3 display head";
            }

            if (var == "VAR_SHOW_VERSION") {
                // AUTO_VERSION comes from auto_firmware_version.py: VERSION.txt
                // plus the git branch and commit. See platformio.ini.
                return kHeadVersion;
            }

            if (var == "VAR_SHOW_CONTROLLER_VERSION") {
                const char* fw = machinelink::controllerFirmware();

                // Says WHY it is unknown rather than printing a dash. The
                // common cause is the Teensy having booted before this board
                // was listening, which is not a fault and should not read like
                // one.
                if (fw[0] == '\0' || (fw[0] == '-' && fw[1] == '\0')) {
                    return machinelink::isConnected()
                               ? F("not announced since this board started")
                               : F("unknown - no link to the controller");
                }

                return fw;
            }

            if (var == "VAR_SHOW_REPOSITORY") {
                return kRepositoryUrl;
            }

            String name(var);
            name.toLowerCase();

            const String path = "/html_fragments/" + name + ".html";

            if (File file = LittleFS.open(path, "r")) {
                if (file.size() * 2 < ESP.getFreeHeap()) {
                    const String body = file.readString();
                    file.close();

                    return body;
                }

                file.close();
                Serial.printf("web: fragment %s too large for free heap\n", path.c_str());
            }

            // An unknown token becomes empty rather than being left as
            // %TOKEN%, so a missing fragment degrades to a gap instead of
            // printing markup at the reader.
            return {};
        }

        /**
         * Two decimal places, as src/embeddedWebserver.h's round2() does it.
         * Copied rather than included: that header pulls in the whole ESP32-era
         * web server, which this board does not have.
         */
        double round2(const double value) {
            return std::round(value * 100.0) / 100.0;
        }

        // -------------------------------------------------------------------
        // I2C scan, on behalf of the browser
        //
        // THE TEENSY OWNS THE I2C BUS, so this board cannot scan it and has to
        // ask. The front end's flow (frontend/js/app.js) is: POST to start,
        // then poll the GET until `scanning` goes false. That shape is
        // upstream's and is kept, so their JavaScript needs no changes.
        //
        // The result arrives as the reason text of the controller's command
        // ack - space-separated hex - which is why link.h grew a command-ack
        // handler. A scan is a question, not a change, and nothing in
        // telemetry could report the answer.
        // -------------------------------------------------------------------

        constexpr int kMaxScanDevices = 16;

        uint8_t scanDevices[kMaxScanDevices];
        int scanDeviceCount = 0;
        bool scanInProgress = false;
        bool scanHasRun = false;
        uint32_t scanStartedMs = 0;

        /** A scan the controller never answered must not hang the button. */
        constexpr uint32_t kScanTimeoutMs = 3000;

        void onCommandAck(const char* cmd, const bool ok, const char* why) {
            if (strcmp(cmd, "i2cscan") != 0) {
                return;
            }

            // Runs on the loop task; the /api/i2cscan handler writes the same
            // five variables from the web task. See headconfig::mutex().
            const headconfig::Guard guard(headconfig::mutex());

            scanDeviceCount = 0;

            if (ok && strcmp(why, "none") != 0) {
                // "04 20 3C" - parsed rather than trusted for length, because
                // this crosses a wire.
                const char* at = why;

                while (*at != '\0' && scanDeviceCount < kMaxScanDevices) {
                    while (*at == ' ') {
                        at++;
                    }

                    if (*at == '\0') {
                        break;
                    }

                    scanDevices[scanDeviceCount++] = static_cast<uint8_t>(strtol(at, nullptr, 16));

                    while (*at != '\0' && *at != ' ') {
                        at++;
                    }
                }
            }

            scanInProgress = false;
            scanHasRun = true;
        }

        /**
         * @brief GET /parameters - the schema and current values.
         *
         * Shaped to match the ESP32 fork's own endpoint, src/embeddedWebserver.h,
         * because the front end in frontend/ is that fork's and drives
         * everything off this response. Match THAT file, not upstream/master:
         * they differ on the hardware section range (upstream 11-15, the fork
         * 11-16, extended for the Encoder section).
         *
         * Four things here were got wrong the first time and all four made the
         * browser UI unusable, so they are spelled out:
         *
         *  1. The id field is called "name", not "id". The front end keys
         *     every template branch, every v-model and the help lookup off
         *     param.name (frontend/html/index.html: v-if="param.name ==
         *     'brew.setpoint'"). Emitting "id" makes all of them undefined, no
         *     branch matches, and every settings page renders EMPTY.
         *
         *  2. offset and limit must be honoured. fetchParameters()
         *     (frontend/js/app.js) pages 5 at a time and stops only on a short
         *     or empty page. Returning the whole registry every time means
         *     neither stop condition can fire: measured 235 requests in 10.5 s,
         *     offset climbing past 1170, for as long as the tab stayed open.
         *
         *  3. "show" and "reboot" must be present. postParameters() builds its
         *     form body only from parameters whose .show is truthy, so without
         *     it a save from a browser silently posts nothing.
         *
         *  4. "filter" is a SECTION RANGE, not a prefix of the key. Prefix
         *     matching only coincidentally looks right for filter=hardware and
         *     is wrong everywhere else - filter=all, which the settings pages
         *     actually request, matched nothing at all.
         *
         * No enum "options" array is emitted. This board registers no kEnum
         * parameters and this front end never reads param.options; add it with
         * the first enum, copying paramToJson() in src/embeddedWebserver.h.
         *
         * Streamed rather than built in a String. The registry is small today
         * but this is the response that grows with every setting added, and a
         * 16 KB String on a board also running WiFi and a framebuffer is the
         * kind of thing that works until it suddenly does not.
         */
        void sendParameters(AsyncWebServerRequest* request) {
            requests++;

            if (!headconfig::ready()) {
                request->send(503, "application/json",
                              "{\"error\":\"config store not ready\"}");

                return;
            }

            String filter;

            if (request->hasParam("filter")) {
                filter = request->getParam("filter")->value();
            }

            // The fork's defaults, and the same ones the front end sends.
            int offset = 0;
            int limit = 5;

            if (request->hasParam("offset")) {
                offset = request->getParam("offset")->value().toInt();
            }

            if (request->hasParam("limit")) {
                limit = request->getParam("limit")->value().toInt();
            }

            // A malformed query must not make the window walk backwards.
            if (offset < 0) {
                offset = 0;
            }

            if (limit < 0) {
                limit = 0;
            }

            AsyncResponseStream* response = request->beginResponseStream("application/json");
            response->print("{\"parameters\":[");

            // Runs on the web task. Every shouldShow() and getValue() below
            // reads the config store, which the loop task may be serialising
            // to the filesystem right now, and which a concurrent save may be
            // editing. See headconfig::mutex(). Held to the end of the
            // function: the response is buffered in memory, so nothing in
            // here waits on the network.
            const headconfig::Guard guard(headconfig::mutex());

            bool first = true;
            int matched = 0;
            int sent = 0;

            for (const auto& param : ParameterRegistry::getInstance().getParameters()) {
                if (!param || !param->shouldShow()) {
                    continue;
                }

                const int section = param->getSection();
                bool include;

                if (filter == "hardware") {
                    include = section >= sHardwareOledSection && section <= sHardwareEncoderSection;
                }
                else if (filter == "behavior") {
                    include = section >= sPIDSection && section <= sSystemSection;
                }
                else if (filter == "other") {
                    include = section == sOtherSection;
                }
                else if (filter == "all") {
                    include = true;
                }
                else {
                    include = section == sPIDSection || section == sTempSection || section == sOtherSection;
                }

                if (!include) {
                    continue;
                }

                if (matched++ < offset) {
                    continue;
                }

                if (sent >= limit) {
                    break;
                }

                if (!first) {
                    response->print(",");
                }

                first = false;

                response->print("{\"type\":");
                response->print(static_cast<int>(param->getType()));
                response->print(",\"name\":\"");
                response->print(param->getId());
                response->print("\",\"displayName\":\"");
                response->print(param->getDisplayName());
                response->print("\",\"section\":");
                response->print(section);
                response->print(",\"position\":");
                response->print(param->getPosition());
                response->print(",\"hasHelpText\":");
                response->print(param->hasHelpText() ? "true" : "false");
                response->print(",\"show\":");
                response->print(param->shouldShow() ? "true" : "false");
                response->print(",\"reboot\":");
                response->print(param->requiresReboot() ? "true" : "false");
                response->print(",\"value\":");

                // Typed the way paramToJson() types it, so an integer setting
                // arrives as 5 and not 5.00.
                switch (param->getType()) {
                    case kInteger:
                    case kColor:
                        response->print(static_cast<int>(param->getValue()));
                        break;

                    case kUInt8:
                        response->print(static_cast<int>(static_cast<uint8_t>(param->getValue())));
                        break;

                    case kCString:
                        response->print("\"");
                        response->print(param->getStringValue());
                        response->print("\"");
                        break;

                    default: {
                        // NEVER hand a raw double to print() here.
                        //
                        // Print::printFloat() in the ESP32 core emits the bare
                        // word `nan` for a NaN and `inf` for an infinity.
                        // Neither is valid JSON, so the browser's JSON.parse()
                        // throws on the WHOLE response and every settings page
                        // renders empty - the same symptom as the four defects
                        // in the comment above, from a completely different
                        // cause.
                        //
                        // It is reachable. The two scale calibration factors
                        // are controller-owned (see headConfig.cpp) and read
                        // NaN until the first telemetry frame carries them, so
                        // an unplugged Teensy, or one sitting in update mode,
                        // takes the entire web UI down with it.
                        // telemetryJson() already guards the same two values
                        // for /api/status; this is that guard, here.
                        const double v = param->getValue();

                        if (std::isnan(v) || std::isinf(v)) {
                            response->print("null");
                        }
                        else {
                            response->print(round2(v), 2);
                        }

                        break;
                    }
                }

                // Enum choices, as paramToJson() in src/embeddedWebserver.h
                // emits them. The front end renders a <select> from this, so
                // without it the Scale Type dropdown is simply empty.
                if (param->getType() == kEnum) {
                    const char* const* options = param->getEnumOptions();
                    const size_t count = param->getEnumCount();

                    response->print(",\"options\":[");

                    for (size_t i = 0; i < count && options != nullptr && options[i] != nullptr; i++) {
                        if (i > 0) {
                            response->print(",");
                        }

                        response->print("{\"value\":");
                        response->print(static_cast<int>(i));
                        response->print(",\"label\":\"");
                        response->print(options[i]);
                        response->print("\"}");
                    }

                    response->print("]");
                }

                response->print(",\"min\":");
                response->print(round2(param->getMinValue()), 2);
                response->print(",\"max\":");
                response->print(round2(param->getMaxValue()), 2);
                response->print("}");

                sent++;
            }

            // The front end ignores these three, but both the fork and
            // upstream send them and matching costs nothing.
            response->printf(R"(],"offset":%d,"limit":%d,"returned":%d})", offset, limit, sent);
            request->send(response);
        }

        /**
         * @brief POST /parameters - change settings.
         *
         * Each form field is one parameter id. Writing it goes through the
         * registered setter, which writes this board's master copy and pushes
         * the value to the Teensy - see headConfig.cpp.
         *
         * THE TEENSY MAY REFUSE, and that is not an error here. Its setters
         * enforce the real limits (a shot time cap that cannot be disabled, a
         * setpoint that cannot approach the over-temperature cutoff), and they
         * are the authority. The reply says what was accepted locally; the
         * machine's own state is what telemetry reports afterwards.
         */
        void applyParameters(AsyncWebServerRequest* request) {
            requests++;

            if (!headconfig::ready()) {
                request->send(503, "text/plain", "config store not ready");

                return;
            }

            auto& registry = ParameterRegistry::getInstance();
            int applied = 0;
            int unknown = 0;

            for (size_t i = 0; i < request->params(); i++) {
                const AsyncWebParameter* p = request->getParam(i);

                if (!p->isPost()) {
                    continue;
                }

                const auto param = registry.getParameterById(p->name().c_str());

                if (!param) {
                    unknown++;

                    continue;
                }

                if (param->getType() == kCString) {
                    param->setStringValue(p->value());
                }
                else {
                    param->setValue(p->value().toDouble());
                }

                applied++;
            }

            char body[96];
            snprintf(body, sizeof(body), "applied %d, unknown %d", applied, unknown);
            request->send(unknown > 0 ? 207 : 200, "text/plain", body);
        }

        /**
         * A reboot asked for by a browser, carried out from loop().
         *
         * Not done inside the handler: the reply has to reach the browser
         * first, and the web task is the wrong place to stop the world from.
         * The front end shows an alert off the response, so restarting before
         * it is flushed looks like the request failed.
         */
        enum class PendingAction : uint8_t {
            None,
            Restart,
            ForgetNetwork,
        };

        PendingAction pendingAction = PendingAction::None;
        uint32_t pendingActionAtMs = 0;

        /** Long enough for the response to be written out and acknowledged. */
        constexpr uint32_t kPendingActionDelayMs = 600;

        /**
         * POST, spelled as the literal 2.
         *
         * NOT the name HTTP_POST. Everything in the include-order note at the
         * top of this file applies to the route table as well: with
         * http_parser's enum winning, HTTP_POST is 3, and as an
         * ESPAsyncWebServer method bitmask 3 means GET|POST. Registering a
         * destructive route under that name would make a plain GET fire it -
         * a browser prefetch, a link checker or somebody pasting the URL could
         * reboot the board or erase its WiFi credentials.
         *
         * This is also why /parameters works: it is registered twice, and the
         * HTTP_GET line is matched first.
         */
        constexpr WebRequestMethodComposite kPostOnly = 2;

        void registerRoutes() {
            // The event stream the home page lives on. Registered as a handler
            // rather than a route, which is how ESPAsyncWebServer does SSE.
            events.onConnect([](AsyncEventSourceClient* client) {
                // Upstream sends a greeting with a 10 s reconnect hint, so a
                // browser that loses the head comes back on its own.
                client->send("hello", nullptr, millis(), 10000);
            });

            server.addHandler(&events);

            // Telemetry for the browser. Read-only by construction - there is
            // no endpoint here that can change the machine, because changing
            // the machine means sending the Teensy a request it may refuse,
            // and that lands with the parameters work rather than here.
            // Upstream's endpoint, same path and same JSON shape, because
            // frontend/js/app.js is upstream's and drives everything off it.
            server.on("/parameters", HTTP_GET, sendParameters);
            server.on("/parameters", HTTP_POST, applyParameters);

            // The chart history. Upstream's path and JSON shape, because
            // frontend/js/temp.js is the fork's and reads exactly these three
            // array names.
            //
            // THE CHARTS DO NOT EXIST UNTIL THIS ANSWERS 200. getTimeseries()
            // builds both uPlot objects inside its success handler, so a 404
            // here leaves two empty boxes on the home page and no later event
            // can fill them - the "nothing on the home page" symptom.
            server.on("/timeseries", HTTP_GET, [](AsyncWebServerRequest* request) {
                requests++;

                AsyncResponseStream* response = request->beginResponseStream("application/json");
                response->addHeader("Connection", "close");

                response->print("{\"currentTemps\":[");
                printHistorySeries(response, 0);
                response->print("],\"targetTemps\":[");
                printHistorySeries(response, 1);
                response->print("],\"heaterPowers\":[");
                printHistorySeries(response, 2);
                response->print("]}");

                request->send(response);
            });

            // Upstream's single-shot version of the three numbers the event
            // stream carries. The front end does not call it, but it is the
            // easy thing to curl when checking whether the feed is alive.
            server.on("/temperatures", HTTP_GET, [](AsyncWebServerRequest* request) {
                requests++;

                char body[128];
                buildTempJson(body, sizeof(body));
                request->send(200, "application/json", body);
            });

            server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* request) {
                requests++;
                request->send(200, "application/json", telemetryJson());
            });

            // The hardware page's "Scan for I2C Devices" button. Upstream's
            // path and upstream's two-step shape: POST starts, GET polls.
            server.on("/api/i2cscan", HTTP_ANY, [](AsyncWebServerRequest* request) {
                requests++;

                // 2, not HTTP_POST, and that is NOT laziness.
                //
                // WiFiManager drags in the ESP-IDF's http_parser.h, whose
                // http_method enum also defines HTTP_GET and HTTP_POST - see
                // the include-order note at the top of this file. Its values
                // are not ESPAsyncWebServer's: http_parser says POST = 3,
                // WebRequestMethod says POST = 0b10 = 2. GET happens to be 1
                // in both, which is why every other route here works and this
                // comparison silently never matched - the button posted, the
                // scan never started, and nothing logged an error.
                //
                // src/embeddedWebserver.h uses the same bare 2 with the same
                // comment, so upstream met this too.
                constexpr int kMethodPost = 2;

                // The scan state is written from the loop task too, by
                // onCommandAck. See headconfig::mutex(). Claiming the scan and
                // sending the request are split so the lock is not held across
                // the UART write.
                bool startScan = false;

                {
                    const headconfig::Guard guard(headconfig::mutex());

                    if (request->method() == kMethodPost && !scanInProgress) {
                        scanInProgress = true;
                        scanStartedMs = millis();
                        startScan = true;
                    }

                    // Give up rather than poll forever if the controller never
                    // answers - with the link down, that is the normal case,
                    // and a spinner that never stops says nothing useful.
                    if (scanInProgress && (millis() - scanStartedMs) > kScanTimeoutMs) {
                        scanInProgress = false;
                        scanHasRun = true;
                        scanDeviceCount = 0;
                    }
                }

                if (startScan) {
                    machinelink::sendCommand("i2cscan=1");
                }

                const headconfig::Guard guard(headconfig::mutex());

                AsyncResponseStream* response = request->beginResponseStream("application/json");

                response->print("{\"hasRun\":");
                response->print(scanHasRun ? "true" : "false");
                response->print(",\"scanning\":");
                response->print(scanInProgress ? "true" : "false");
                response->print(",\"devices\":[");

                for (int i = 0; i < scanDeviceCount; i++) {
                    if (i > 0) {
                        response->print(",");
                    }

                    response->print(scanDevices[i]);
                }

                response->print("]}");
                request->send(response);
            });

            // Both firmware versions, for scripts and for "what is actually on
            // these two boards right now".
            //
            // The head's is compiled in. The CONTROLLER'S IS WHATEVER IT
            // ANNOUNCED over the link, and that is the only way this board can
            // know: it cannot read the Teensy's flash. `controllerReported`
            // says whether the string is a real answer, so a script can tell
            // "the Teensy is running X" from "nobody has told me".
            server.on("/api/version", HTTP_GET, [](AsyncWebServerRequest* request) {
                requests++;

                const char* fw = machinelink::controllerFirmware();
                const bool reported = !(fw[0] == '\0' || (fw[0] == '-' && fw[1] == '\0'));

                JsonDocument doc;
                doc["product"] = "CleverCoffee Teensy";
                doc["boards"] = "Teensy 4.1 controller + ESP32-S3 display head";
                doc["head"] = kHeadVersion;
                doc["controller"] = reported ? fw : "unknown";
                doc["controllerReported"] = reported;
                doc["linkOk"] = machinelink::isConnected();
                doc["repository"] = kRepositoryUrl;

                String out;
                serializeJson(doc, out);
                request->send(200, "application/json", out);
            });

            /**
             * Everything the Diags page shows, in one request.
             *
             * Deliberately one endpoint rather than three: the page polls it
             * on a timer, and one request a second is cheaper for this board
             * than three. It mixes controller data, which arrives over the
             * link, with this board's own, which it knows directly.
             *
             * `controllerSeen` is false until the first diagnostics frame
             * lands. The page must show that rather than a screen of zeroes,
             * which is the same rule the panel follows for a dead sensor.
             */
            server.on("/api/diag", HTTP_GET, [](AsyncWebServerRequest* request) {
                requests++;

                // ?on=1 / ?on=0 switches the controller's diagnostics frame.
                // A query parameter on the GET rather than a second endpoint,
                // because the page then has one thing to talk to and the
                // reply always carries the resulting state.
                //
                // Off is the default on the controller and it does not
                // persist, so closing the page and rebooting both end up
                // quiet without anyone having to remember.
                if (request->hasParam("on")) {
                    const bool wanted = request->getParam("on")->value() != "0";

                    machinelink::sendCommand(wanted ? "diag=1" : "diag=0");
                    diagRequested = wanted;
                }

                const auto& d = machinelink::diag();
                const auto& st = machinelink::state();
                const bool live = machinelink::isConnected();

                JsonDocument doc;

                doc["enabled"] = diagRequested;
                doc["linkOk"] = live;
                doc["controllerSeen"] = d.everReceived;
                doc["diagAgeMs"] = d.ageMs;

                JsonObject out = doc["outputs"].to<JsonObject>();
                out["heater"] = (d.outputs & 0x01) != 0;
                out["groupValve"] = (d.outputs & 0x02) != 0;
                out["fill"] = (d.outputs & 0x04) != 0;
                out["coldInlet"] = (d.outputs & 0x08) != 0;

                JsonObject s = doc["sensors"].to<JsonObject>();
                // null, not a number, when the reading is not trustworthy -
                // the same rule the charts and the panel follow.
                if (live && st.boilerOk) {
                    s["boilerC"] = st.boilerC;
                }
                else {
                    s["boilerC"] = nullptr;
                }

                if (live && st.groupOk) {
                    s["groupC"] = st.groupC;
                }
                else {
                    s["groupC"] = nullptr;
                }

                s["boilerOk"] = st.boilerOk;
                s["groupOk"] = st.groupOk;
                s["boilerAgeMs"] = d.boilerAgeMs;
                s["groupAgeMs"] = d.groupAgeMs;
                s["flowMlPerSec"] = st.flowMlPerSec;
                s["volumeMl"] = st.volumeMl;
                s["flowPulses"] = d.flowPulses;
                s["flowRejected"] = d.flowRejected;
                s["weight"] = st.weightG;
                s["weightOk"] = st.weightOk;
                s["weightCalibrated"] = st.weightCalibrated;
                s["brewSwitchLevel"] = d.brewSwitchLevel;
                s["waterPinLevel"] = d.waterPinLevel;
                s["encoder"] = d.encoder;

                JsonObject c = doc["control"].to<JsonObject>();
                c["status"] = st.status;
                c["fault"] = st.fault;
                c["setpointC"] = st.setpointC;
                c["heaterDuty"] = st.heaterDuty;
                c["heaterEnabled"] = st.heaterEnabled;
                c["overTempLimitC"] = d.overTempLimitC;
                c["pumpPercent"] = st.pumpPercent;
                c["pumpRefusal"] = d.pumpRefusal;
                c["backflushArmed"] = st.backflushArmed;

                JsonObject b = doc["boards"].to<JsonObject>();
                b["controllerUptimeS"] = d.controllerUptimeS;
                b["controllerFreeHeap"] = d.freeHeap;
                b["controllerFirmware"] = machinelink::controllerFirmware();
                b["headFirmware"] = kHeadVersion;
                b["headUptimeS"] = millis() / 1000UL;
                b["headFreeHeap"] = ESP.getFreeHeap();
                b["headFreePsram"] = ESP.getFreePsram();
                b["rssi"] = WiFi.RSSI();

                JsonObject l = doc["link"].to<JsonObject>();
                l["framesReceived"] = machinelink::framesReceived();
                l["badChecksum"] = machinelink::badChecksums();
                l["msSinceFrame"] = machinelink::msSinceLastFrame();

                String outStr;
                serializeJson(doc, outStr);
                request->send(200, "application/json", outStr);
            });

            /**
             * Momentary output test, from the Diags page.
             *
             * `/api/hold?ch=valve|fill|inlet&on=1`, repeated by the browser
             * while the button is held. The controller drops the output about
             * a second after the requests stop, so a closed tab or a dropped
             * network ends it exactly like letting go does.
             *
             * No heater channel, deliberately. See the note in the
             * controller's command handler.
             */
            server.on("/api/hold", HTTP_GET, [](AsyncWebServerRequest* request) {
                requests++;

                if (!request->hasParam("ch")) {
                    request->send(400, "application/json", "{\"error\":\"no channel\"}");

                    return;
                }

                const String ch = request->getParam("ch")->value();
                const bool on = request->hasParam("on") && request->getParam("on")->value() != "0";

                if (ch != "valve" && ch != "fill" && ch != "inlet") {
                    request->send(400, "application/json", "{\"error\":\"unknown channel\"}");

                    return;
                }

                char cmd[40];
                snprintf(cmd, sizeof(cmd), "hold=%s,on=%d", ch.c_str(), on ? 1 : 0);
                machinelink::sendCommand(cmd);

                // Reports what was SENT, not what the controller did with it.
                // The controller is free to refuse - it does while a shot is
                // running - and the Diags page shows the real output states a
                // moment later from the diagnostics frame itself.
                request->send(200, "application/json", "{\"sent\":true}");
            });

            // Network and head diagnostics, so a browser can answer "is this
            // thing working" without a serial cable.
            server.on("/api/net", HTTP_GET, [](AsyncWebServerRequest* request) {
                requests++;

                JsonDocument doc;
                doc["state"] = stateName();
                doc["address"] = addressBuf;
                doc["network"] = networkBuf;
                doc["filesystem"] = fsMounted;
                doc["requests"] = requests;
                doc["rssi"] = WiFi.RSSI();
                doc["heapFree"] = ESP.getFreeHeap();
                doc["psramFree"] = ESP.getFreePsram();

                String out;
                serializeJson(doc, out);
                request->send(200, "application/json", out);
            });

            /**
             * The help popovers.
             *
             * Every setting on every page is drawn with a "?" beside it, and
             * every one of them called an endpoint that did not exist. Each
             * click was a 404 and an empty popover.
             *
             * Upstream's shape (src/embeddedWebserver.h), and the front end
             * asks for "/parameterHelp/?param=NAME" with a trailing slash -
             * which matches because ESPAsyncWebServer also accepts a URL that
             * starts with the registered path plus "/".
             */
            server.on("/parameterHelp", HTTP_GET, [](AsyncWebServerRequest* request) {
                requests++;

                if (!request->hasParam("param")) {
                    request->send(422, "text/plain", "parameter is missing");

                    return;
                }

                const String name = request->getParam("param")->value();

                const headconfig::Guard guard(headconfig::mutex());

                const std::shared_ptr<Parameter> param =
                    ParameterRegistry::getInstance().getParameterById(name.c_str());

                if (param == nullptr) {
                    request->send(404, "application/json", "parameter not found");

                    return;
                }

                JsonDocument doc;
                doc["name"] = name;
                doc["helpText"] = param->getHelpText();

                String out;
                serializeJson(doc, out);
                request->send(200, "application/json", out);
            });

            /**
             * Restart - THIS BOARD ONLY.
             *
             * The System page's button says "restart your machine", which on
             * an upstream single-board machine is what it did. Here it is not:
             * the display head cannot reset the controller, deliberately and
             * by design (see the reset-separation note in README). So this
             * reboots the head, the Teensy carries on running the machine
             * throughout, and the reply says so rather than letting someone
             * believe the machine was restarted.
             */
            server.on("/restart", kPostOnly, [](AsyncWebServerRequest* request) {
                requests++;

                pendingAction = PendingAction::Restart;
                pendingActionAtMs = millis();

                request->send(200, "text/plain",
                              "Restarting the display head. The controller keeps running "
                              "the machine - this does not restart the machine itself.\n");
            });

            /**
             * Forget the stored network and come back up in the setup portal.
             *
             * The same thing the `wifireset` console command does. Destructive
             * only to the WiFi credentials; the machine is unaffected, and the
             * front end asks the operator to confirm before calling it.
             */
            server.on("/wifireset", kPostOnly, [](AsyncWebServerRequest* request) {
                requests++;

                pendingAction = PendingAction::ForgetNetwork;
                pendingActionAtMs = millis();

                request->send(200, "text/plain",
                              "WiFi credentials erased. The head is restarting into its "
                              "setup portal - join the 'clevercoffee' network to set it up "
                              "again. The machine is unaffected.\n");
            });

            /**
             * Download the configuration as a file. Read-only.
             *
             * A plain link on the System page, so it was a 404 with no error
             * handling at all behind it - the browser simply showed its own
             * "not found" page.
             *
             * Served straight off the filesystem rather than re-serialised,
             * so what is downloaded is byte for byte what the head would load
             * at its next boot. Taken under the lock because the loop task
             * writes that same file.
             */
            server.on("/download/config", HTTP_GET, [](AsyncWebServerRequest* request) {
                requests++;

                const headconfig::Guard guard(headconfig::mutex());

                // fsPresent, not fsMounted: the config file is worth
                // downloading even from a board whose web pages were never
                // uploaded - which is exactly the board someone is most
                // likely to be rescuing a configuration from.
                if (!fsPresent || !LittleFS.exists("/config.json")) {
                    request->send(404, "text/plain", "no saved configuration on this board");

                    return;
                }

                request->send(LittleFS, "/config.json", "application/json", true);
            });

            // DELIBERATELY NOT REGISTERED: /factoryreset, /upload/config and
            // /toggleScaleCalibration. John's call, 2026-11-17 - wire the safe
            // ones only. The first two overwrite the whole configuration from
            // a browser, and the third starts a scale calibration, which is
            // the one measurement on this machine that has already been
            // destroyed once by software writing over it. Their buttons are
            // still on the System and Hardware pages and still do nothing.

            if (fsMounted) {
                // Upstream's frontend/ served as-is from LittleFS.
                server.serveStatic("/", LittleFS, "/html/")
                    .setDefaultFile("index.html")
                    .setTemplateProcessor(templateProcessor);
                server.serveStatic("/css/", LittleFS, "/css/");
                server.serveStatic("/js/", LittleFS, "/js/");
                server.serveStatic("/img/", LittleFS, "/img/");
                server.serveStatic("/webfonts/", LittleFS, "/webfonts/");
                server.serveStatic("/html_fragments/", LittleFS, "/html_fragments/");
            }

            server.onNotFound([](AsyncWebServerRequest* request) {
                requests++;

                // Says which of the two likely causes it is, rather than a bare
                // 404. A fresh board with the firmware flashed but not the
                // filesystem looks exactly like a broken server otherwise -
                // see BENCH.md on the second upload.
                if (!fsMounted) {
                    request->send(503, "text/plain",
                                  "No web UI on this board: the LittleFS image was never uploaded.\n"
                                  "Run: pio run -e esp32s3 -t uploadfs\n"
                                  "The JSON API at /api/status and /api/net works regardless.\n");

                    return;
                }

                request->send(404, "text/plain", "Not found");
            });
        }

    } // namespace

    bool mountFilesystem() {
        // false = do not format on failure. Formatting would turn "you forgot
        // the second upload" into "your web UI has been erased", and the
        // difference matters when the fix is one command.
        fsPresent = LittleFS.begin(false);

        // MOUNTED IS NOT THE SAME AS "THE PAGES ARE THERE", and treating them
        // as one thing quietly destroyed the message below.
        //
        // Config::begin() runs moments after this and calls LittleFS.begin
        // with format-on-failure TRUE, because the config store has to have
        // somewhere to keep config.json and a head with no settings at all is
        // worse than a formatted partition. Nothing here can refuse that, and
        // it should not try.
        //
        // The consequence is the trap: a board flashed without `uploadfs`
        // fails to mount on its first boot, gets formatted by the config
        // store, and from the second boot onward mounts perfectly while
        // holding one JSON file and no pages. This flag used to read true in
        // that state, so the operator got a bare "Not found" instead of the
        // one message that tells them which command they forgot.
        //
        // So the flag now means what its three consumers actually ask of it -
        // the static routes, the not-found message and the status print - and
        // that is "are the PAGES here", not "did the chip mount".
        fsMounted = fsPresent && LittleFS.exists("/html/index.html");

        if (fsMounted) {
            Serial.printf("fs: LittleFS mounted, %u of %u bytes used\n",
                          static_cast<unsigned>(LittleFS.usedBytes()),
                          static_cast<unsigned>(LittleFS.totalBytes()));
        }
        else if (fsPresent) {
            Serial.println("fs: LittleFS mounted but there are no web pages on it");
            Serial.println("    run: pio run -e esp32s3 -t uploadfs");
            Serial.println("    (a config-only filesystem looks like this - see mountFilesystem)");
        }
        else {
            Serial.println("fs: no LittleFS image - the web UI was never uploaded");
            Serial.println("    run: pio run -e esp32s3 -t uploadfs");
            Serial.println("    the JSON API still works; only the pages are missing");
        }

        return fsMounted;
    }

    void begin() {
        // Tell the screen BEFORE blocking, so a panel that sits still for ten
        // seconds is explained rather than alarming.
        current = State::Connecting;
        display::showMessage("WiFi", "connecting...", kHostname);

        WiFi.mode(WIFI_STA);

        // Modem sleep OFF.
        //
        // It is on by default, and the evidence it was active here was a LAN
        // ping round trip of 40 to 120 ms to a device three metres away. On
        // ESP32 it is a known cause of flaky inbound TCP, which matters a lot
        // for a board whose entire network job is answering requests nobody
        // warned it about.
        //
        // The cost is power, and this board is mains powered inside an
        // espresso machine, so there is nothing to save.
        WiFi.setSleep(false);

        // Upstream's settings, same values and same reasons (wiFiSetup()).
        wm.setCleanConnect(true);
        wm.setConnectTimeout(kConnectTimeoutSec);
        wm.setConnectRetries(kConnectRetries);
        wm.setBreakAfterConfig(true);
        wm.setHostname(kHostname);

        // autoConnect must NOT raise the portal by itself, because the portal
        // needs the screen updated first - otherwise an AP appears with
        // nothing anywhere saying its name.
        wm.setEnableConfigPortal(false);
        wm.setDisableConfigPortal(true);

        // Was the portal asked for? Read and CLEAR in one go, so a crash
        // inside the portal cannot turn into a machine that boots into setup
        // for ever.
        bool portalRequested = false;

        if (prefs.begin(kPrefNamespace, false)) {
            portalRequested = prefs.getBool(kPrefPortalKey, false);

            if (portalRequested) {
                prefs.remove(kPrefPortalKey);
            }

            prefs.end();
        }

        bool connected = false;

        if (!portalRequested) {
            connected = wm.autoConnect(kHostname, kApPassword);
        }
        else {
            Serial.println("wifi: setup portal requested - saved network kept, cancel to return to it");
        }

        if (!connected) {
            Serial.println("wifi: no saved network joined - raising the setup portal");

            current = State::Portal;
            snprintf(networkBuf, sizeof(networkBuf), "%s", kHostname);
            setAddress(WiFi.softAPIP());

            char line1[40];
            snprintf(line1, sizeof(line1), "join: %s", kHostname);

            // The password is shown on the machine's own screen on purpose:
            // it is a fixed, published default from upstream, not a secret,
            // and somebody standing at the machine needs it to proceed.
            char line2[40];
            snprintf(line2, sizeof(line2), "pass: %s", kApPassword);

            display::showMessage("WiFi setup", line1, line2);

            wm.setConfigPortalTimeout(kPortalTimeoutSec);
            connected = wm.startConfigPortal(kHostname, kApPassword);
        }

        // A CANCELLED OR TIMED-OUT SETUP SCREEN MUST NOT COST THE NETWORK.
        //
        // On a portal-requested boot - somebody pressing SET UP WIFI on the
        // panel - autoConnect() above is skipped deliberately, so that the
        // portal appears instead of the machine silently reconnecting. The
        // catch was that nothing tried the saved credentials afterwards: the
        // portal timed out, `connected` stayed false, and the head sat offline
        // until the next power cycle with perfectly good credentials in NVS it
        // never looked at.
        //
        // That made SET UP WIFI a trap even after it stopped erasing anything,
        // and it contradicted what requestPortalOnNextBoot()'s own comment in
        // net.h promises. Found 2026-11-17 when John pressed the button.
        //
        // Only on the requested path. A normal boot has already tried
        // autoConnect() and failed, and retrying it here would just double
        // every cold start with no network in range.
        if (!connected && portalRequested) {
            Serial.println("wifi: setup screen closed without a new network - rejoining the saved one");

            current = State::Connecting;
            display::showMessage("WiFi", "reconnecting...", kHostname);

            // The portal leaves the radio in AP mode; autoConnect needs STA.
            WiFi.mode(WIFI_STA);

            connected = wm.autoConnect(kHostname, kApPassword);
        }

        if (connected) {
            current = State::Online;
            setAddress(WiFi.localIP());
            snprintf(networkBuf, sizeof(networkBuf), "%s", WiFi.SSID().c_str());

            Serial.printf("wifi: connected to '%s' as %s\n", networkBuf, addressBuf);

            display::showMessage("WiFi", networkBuf, addressBuf);
        }
        else {
            // NOT a failure of the machine. The Teensy has been running it
            // this whole time and will carry on.
            current = State::Offline;
            snprintf(networkBuf, sizeof(networkBuf), "-");
            snprintf(addressBuf, sizeof(addressBuf), "-");

            Serial.println("wifi: no network. The machine works; only the web UI is unavailable.");
            display::showMessage("No WiFi", "machine is fine", "'wifireset' to retry");
        }

        // The I2C scan's answer comes back as a command ack - see onCommandAck.
        machinelink::setCommandAckHandler(onCommandAck);

        registerRoutes();
        server.begin();
        listening = true;

        Serial.printf("web: server listening on port 80%s\n", fsMounted ? "" : " (API only, no pages)");
    }

    void loop() {
        // ESPAsyncWebServer runs on its own task, so there is nothing to pump
        // here.
        //
        // Deliberately NOT reconnecting aggressively: a head that keeps
        // retrying WiFi spends its time in the radio stack instead of drawing
        // the screen, and the screen is this board's actual job.

        // A reboot a browser asked for, now that its reply has gone out.
        if (pendingAction != PendingAction::None
            && (millis() - pendingActionAtMs) >= kPendingActionDelayMs) {
            const PendingAction action = pendingAction;
            pendingAction = PendingAction::None;

            if (action == PendingAction::ForgetNetwork) {
                forgetNetworkAndRestart();
            }
            else {
                Serial.println("web: restart requested from a browser");
                Serial.flush();
                ESP.restart();
            }
        }

        // Sampling for the charts and the live readout. Once a second, as
        // upstream does it. Runs whether or not a browser is attached, so the
        // chart has history to show the moment somebody opens the page; the
        // event send inside is what gets skipped when nobody is listening.
        if (!listening) {
            return;
        }

        const uint32_t now = millis();

        if (now - lastSampleMs < kSampleIntervalMs) {
            return;
        }

        lastSampleMs = now;
        sampleAndPublish();
    }

    State state() {
        return current;
    }

    const char* stateName() {
        switch (current) {
            case State::Idle:
                return "idle";
            case State::Connecting:
                return "connecting";
            case State::Portal:
                return "setup portal";
            case State::Online:
                return "online";
            case State::Offline:
                return "offline";
        }

        return "?";
    }

    const char* addressText() {
        return addressBuf;
    }

    const char* networkName() {
        return networkBuf;
    }

    const char* headVersion() {
        return kHeadVersion;
    }

    bool serverRunning() {
        return listening;
    }

    bool filesystemMounted() {
        return fsMounted;
    }

    uint32_t requestCount() {
        return requests;
    }

    void requestPortalOnNextBoot() {
        if (prefs.begin(kPrefNamespace, false)) {
            prefs.putBool(kPrefPortalKey, true);
            prefs.end();
        }

        Serial.println("wifi: will open the setup portal on the next boot; credentials kept");
        Serial.flush();

        delay(200);
        ESP.restart();
    }

    void forgetNetworkAndRestart() {
        Serial.println("wifi: forgetting the stored network and restarting into setup");
        Serial.flush();

        wm.resetSettings();
        delay(200);
        ESP.restart();
    }

    void printStatus() {
        Serial.printf("net: %s", stateName());

        if (current == State::Online || current == State::Portal) {
            Serial.printf("  network='%s'  http://%s/", networkBuf, addressBuf);
        }

        Serial.println();
        Serial.printf("     server %s, web UI %s, requests=%lu\n",
                      listening ? "listening on :80" : "not started",
                      fsMounted ? "present" : "NOT UPLOADED (pio run -e esp32s3 -t uploadfs)",
                      static_cast<unsigned long>(requests));

        if (current == State::Online) {
            Serial.printf("     rssi %d dBm, heap %u, psram %u\n",
                          WiFi.RSSI(),
                          static_cast<unsigned>(ESP.getFreeHeap()),
                          static_cast<unsigned>(ESP.getFreePsram()));
        }
    }

} // namespace net
