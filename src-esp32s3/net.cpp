/**
 * @file net.cpp
 *
 * @brief WiFi and the web server on the display head. See net.h.
 */

#include "net.h"

#include <Arduino.h>

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
#include <WiFi.h>

#include "display.h"
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

        /** Upstream's values: 10 s to connect, 3 tries. 5 s proved too short. */
        constexpr uint8_t kConnectTimeoutSec = 10;
        constexpr uint8_t kConnectRetries = 3;

        /** How long the portal stays up before giving up and running offline. */
        constexpr uint16_t kPortalTimeoutSec = 180;

        AsyncWebServer server(80);
        WiFiManager wm;

        State current = State::Idle;
        bool fsMounted = false;
        bool listening = false;
        uint32_t requests = 0;

        char addressBuf[24] = "-";
        char networkBuf[40] = "-";

        void setAddress(const IPAddress& ip) {
            snprintf(addressBuf, sizeof(addressBuf), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
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

        void registerRoutes() {
            // Telemetry for the browser. Read-only by construction - there is
            // no endpoint here that can change the machine, because changing
            // the machine means sending the Teensy a request it may refuse,
            // and that lands with the parameters work rather than here.
            server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* request) {
                requests++;
                request->send(200, "application/json", telemetryJson());
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

        bool connected = wm.autoConnect(kHostname, kApPassword);

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

        registerRoutes();
        server.begin();
        listening = true;

        Serial.printf("web: server listening on port 80%s\n", fsMounted ? "" : " (API only, no pages)");
    }

    void loop() {
        // ESPAsyncWebServer runs on its own task, so there is nothing to pump
        // here. This exists so the call site reads like every other handler and
        // so there is somewhere obvious to put reconnection logic later.
        //
        // Deliberately NOT reconnecting aggressively: a head that keeps
        // retrying WiFi spends its time in the radio stack instead of drawing
        // the screen, and the screen is this board's actual job.
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

    bool serverRunning() {
        return listening;
    }

    bool filesystemMounted() {
        return fsMounted;
    }

    uint32_t requestCount() {
        return requests;
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
