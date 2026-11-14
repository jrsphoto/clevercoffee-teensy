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
        bool fsMounted = false;
        bool listening = false;
        uint32_t requests = 0;

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

        constexpr int kHistoryLength = 600;    ///< 20 minutes at one record / 2 s
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
            doc["encoder"] = st.encoder;

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

                    default:
                        response->print(round2(param->getValue()), 2);
                        break;
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

                if (request->method() == kMethodPost && !scanInProgress) {
                    scanInProgress = true;
                    scanStartedMs = millis();
                    machinelink::sendCommand("i2cscan=1");
                }

                // Give up rather than poll forever if the controller never
                // answers - with the link down, that is the normal case, and
                // a spinner that never stops says nothing useful.
                if (scanInProgress && (millis() - scanStartedMs) > kScanTimeoutMs) {
                    scanInProgress = false;
                    scanHasRun = true;
                    scanDeviceCount = 0;
                }

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
        fsMounted = LittleFS.begin(false);

        if (fsMounted) {
            Serial.printf("fs: LittleFS mounted, %u of %u bytes used\n",
                          static_cast<unsigned>(LittleFS.usedBytes()),
                          static_cast<unsigned>(LittleFS.totalBytes()));
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
