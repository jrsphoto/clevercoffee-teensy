/**
 * @file headConfig.cpp
 *
 * @brief The head's parameter registry: upstream's machinery, this machine's list
 *
 * Upstream keeps its whole configuration schema in ParameterRegistry::initialize()
 * - about 1350 lines registering roughly a hundred parameters. That function
 * cannot come across, because nearly every entry binds to an ESP32-era global
 * this board does not have: steamON, machineState, the OLED, the status LEDs.
 *
 * Everything else in ParameterRegistry IS generic, so it is reused verbatim:
 * the inline add*ConfigParam helpers in the header, getParameterById() and
 * syncGlobalVariables(). Only the singleton definition and initialize() are
 * provided here. That is why this file exists rather than a fork of
 * ParameterRegistry.cpp - the divergence is one function, not a file.
 *
 * THE SEAM, which is the whole point (TEENSY-PORT-BRIEF.md 9.6)
 *
 * Upstream's helpers do two things per parameter: write the JSON config store,
 * and optionally mirror the value into a `globalVar` pointer that the control
 * code reads. On a single board those are the same machine. Here they are not:
 * the control code is on the Teensy, across a wire.
 *
 * So the globalVar pointer is replaced by a push over the link. Each setter
 * registered here writes the head's config - which is the persistent master -
 * and then sends the key and value to the Teensy, which validates it through
 * its own setters and caches it in EEPROM. The Teensy may refuse; the head
 * does not assume otherwise, and the config store keeps what the operator
 * asked for either way.
 *
 * KEYS MATCH THE TEENSY'S TABLE, deliberately and one for one. configStore.cpp
 * on the controller owns the same dotted paths, and both of them use upstream's
 * names wherever an upstream equivalent exists. If a key here has no
 * counterpart there the Teensy answers "notmine", which is not an error - it is
 * how the head's WiFi, MQTT and display settings are ignored by a controller
 * that has no business with them.
 */

#include <Arduino.h>
#include <functional>
#include <map>
#include <string>

#include "Config.h"
#include "ParameterRegistry.h"

#include "headConfig.h"
#include "link.h"

// The static member ParameterRegistry.cpp would normally define.
ParameterRegistry ParameterRegistry::_singleton;

// Generic, copied unchanged from upstream's ParameterRegistry.cpp so that the
// only thing this file actually diverges on is the parameter list.
std::shared_ptr<Parameter> ParameterRegistry::getParameterById(const char* id) {
    if (const auto it = _parameterMap.find(id); it != _parameterMap.end()) {
        return it->second;
    }

    return nullptr;
}

void ParameterRegistry::syncGlobalVariables() const {
    for (const auto& param : _parameters) {
        if (param && param->getGlobalVariablePointer()) {
            if (param->getType() == kCString) {
                param->syncToGlobalVariable(param->getStringValue());
            }
            else {
                param->syncToGlobalVariable(param->getValue());
            }
        }
    }
}

namespace {

    /**
     * What a key held before it was changed, kept until the controller says
     * whether it accepted the new value.
     *
     * The controller is authoritative and validates everything through its
     * own setters, so it can refuse. If it does, this board must not keep the
     * rejected value: the web UI would then display a setting the machine is
     * not honouring, which is the interface lying about the machine. So the
     * old value is held here and put back when a refusal arrives.
     *
     * Keyed by the dotted path. Small - only values changed since the last
     * ack are in flight, which in practice is one web form submission.
     */
    std::map<std::string, double> pendingPrevious;

    /** Refusals since boot, for the console. */
    uint32_t refusedCount = 0;

    /**
     * @brief Send one value to the controller.
     *
     * Formatted with %g so a whole number travels as "5" rather than
     * "5.000000", which keeps frames short and keeps the Teensy's console
     * readable when it echoes them back.
     */
    void pushToController(const char* key, const double value) {
        char text[24];
        snprintf(text, sizeof(text), "%g", value);
        machinelink::sendConfig(key, text);
    }

    /**
     * @brief The controller's answer to a config write.
     *
     * Runs from the main loop, not from the web handler, because the ack
     * arrives whenever the controller gets to it. That is why a POST cannot
     * report the controller's verdict directly: it would have to block the
     * async web task waiting for a serial round trip. Instead the value is
     * corrected here, and the browser sees the correction on its next
     * /parameters poll.
     *
     * "notmine" is NOT a refusal. It means the key is not a control setting -
     * WiFi, MQTT, display brightness - and the controller rightly ignored it.
     * Those keep their value here, which is where they belong.
     */
    void onConfigAck(const char* key, const bool ok, const char* why) {
        const auto it = pendingPrevious.find(key);

        if (it == pendingPrevious.end()) {
            return;
        }

        const double previous = it->second;
        pendingPrevious.erase(it);

        if (ok || strcmp(why, "notmine") == 0) {
            return;
        }

        refusedCount++;

        // Put it back, WITHOUT pushing again - the controller already has the
        // value it chose to keep, and re-sending the old one would be a
        // second write it never asked for.
        auto& store = headconfig::config();
        store.set<double>(key, previous);

        Serial.printf("config: controller REFUSED %s (%s) - reverted to %g\n", key, why, previous);
    }

} // namespace

void ParameterRegistry::initialize(Config& config) {
    _config = &config;

    // A numeric setting the CONTROLLER owns. Writes the head's master copy,
    // then pushes it across. Mirrors upstream's addNumericConfigParam, with
    // the globalVar pointer replaced by the link - see the header.
    const auto addLinked = [this, &config](const char* path, const char* name, const EditableKind kind,
                                           const int section, const int position,
                                           const double lo, const double hi, const char* help) {
        // The function types are spelled out rather than left as lambdas:
        // Parameter has several constructors and bare lambdas leave the
        // numeric one ambiguous against the general one.
        const std::function<double()> getter = [this, path]() -> double {
            return _config->get<double>(path);
        };

        const std::function<void(double)> setter = [this, path](const double v) {
            // Remembered BEFORE the write, so a refusal can undo it.
            pendingPrevious[path] = _config->get<double>(path);

            _config->set<double>(path, v);
            markChanged();
            pushToController(path, v);
        };

        // showCondition and the (null) globalVar are passed explicitly, not
        // left to default. Both the numeric and the general constructor can
        // accept the shorter argument list, so omitting them is ambiguous -
        // upstream's addNumericConfigParam passes them for the same reason.
        const std::function<bool()> always = [] { return true; };

        const auto param = std::make_shared<Parameter>(
            path, name, kind, section, position, getter, setter,
            lo, hi, help[0] != '\0', help, always, static_cast<void*>(nullptr));

        addParam(param);
    };

    // A boolean the controller owns. Same idea; the Teensy takes 0 or 1.
    const auto addLinkedBool = [this, &config](const char* path, const char* name,
                                               const int section, const int position, const char* help) {
        const std::function<bool()> getter = [this, path]() -> bool {
            return _config->get<bool>(path);
        };

        const std::function<void(bool)> setter = [this, path](const bool v) {
            pendingPrevious[path] = _config->get<bool>(path) ? 1.0 : 0.0;

            _config->set<bool>(path, v);
            markChanged();
            pushToController(path, v ? 1.0 : 0.0);
        };

        const std::function<bool()> always = [] { return true; };

        const auto param = std::make_shared<Parameter>(
            path, name, kUInt8, section, position, getter, setter,
            help[0] != '\0', help, always, static_cast<void*>(nullptr));

        addParam(param);
    };

    // --- Temperature and PID ------------------------------------------
    addLinked("brew.setpoint", "Brew Temperature (C)", kDouble, sTempSection, 10,
              60.0, 110.0, "Target boiler temperature");

    addLinked("pid.regular.kp", "PID Kp", kDouble, sPIDSection, 20, 0.0, 999.0, "");
    addLinked("pid.regular.tn", "PID Tn (s)", kDouble, sPIDSection, 21, 0.0, 999.0, "");
    addLinked("pid.regular.tv", "PID Tv (s)", kDouble, sPIDSection, 22, 0.0, 999.0, "");

    // --- The shot ------------------------------------------------------
    //
    // Durations are in SECONDS here, as upstream stores them. The Teensy's
    // table carries the x1000 conversion, so this side never thinks in
    // milliseconds - see configStore.cpp.
    addLinked("brew.pre_infusion.time", "Pre-infusion Time (s)", kDouble, sBrewSection, 30,
              0.0, 30.0, "Low-speed wetting before extraction");
    addLinked("brew.pre_infusion.pump_percent", "Pre-infusion Pump (%)", kDouble, sBrewSection, 31,
              0.0, 100.0, "Pump speed during pre-infusion");
    addLinked("brew.pre_infusion.pause", "Soak Time (s)", kDouble, sBrewSection, 32,
              0.0, 30.0, "Pump off, letting the puck absorb");
    addLinked("brew.pump_percent", "Extraction Pump (%)", kDouble, sBrewSection, 33,
              0.0, 100.0, "Pump speed during extraction");
    addLinked("brew.by_weight.target_weight", "Target Weight (g)", kDouble, sBrewSection, 34,
              0.0, 500.0, "Stops the shot at this weight. Needs a working scale.");
    addLinked("brew.by_volume.target_volume", "Target Volume (ml)", kDouble, sBrewSection, 35,
              0.0, 500.0, "Stops the shot at this volume. 0 disables it.");
    addLinked("brew.by_time.target_time", "Maximum Shot Time (s)", kDouble, sBrewSection, 36,
              5.0, 180.0,
              "The hard limit. Unlike upstream this cannot be switched off: it is the "
              "only stop condition with no sensor behind it.");

    // --- Manual flush ---------------------------------------------------
    addLinked("flush.pump_percent", "Flush Pump (%)", kDouble, sMaintenanceSection, 40,
              0.0, 100.0, "Pump speed while the brew switch is held");
    addLinked("flush.max_time", "Flush Time Limit (s)", kDouble, sMaintenanceSection, 41,
              1.0, 60.0, "Backstop in case the brew switch sticks. Cannot be disabled.");

    // --- Backflush. Upstream's keys and ranges. --------------------------
    addLinked("backflush.cycles", "Backflush Cycles", kInteger, sMaintenanceSection, 50,
              2.0, 20.0, "");
    addLinked("backflush.fill_time", "Backflush Fill Time (s)", kDouble, sMaintenanceSection, 51,
              3.0, 10.0, "");
    addLinked("backflush.flush_time", "Backflush Flush Time (s)", kDouble, sMaintenanceSection, 52,
              5.0, 20.0, "");

    // --- Cooling flush. No upstream equivalent; see brief 9.5. -----------
    addLinkedBool("cooling_flush.enabled", "Automatic Cooling Flush", sBrewPidSection, 60,
                  "Flushes the overheated slug out of the heat exchanger before a shot. "
                  "Leave OFF until the two numbers below have been measured.");
    addLinked("cooling_flush.target_temperature", "Flush Target (C)", kDouble, sBrewPidSection, 61,
              70.0, 110.0, "Group temperature at or below which no flush is needed");
    addLinked("cooling_flush.ms_per_degree", "Flush ms per Degree", kInteger, sBrewPidSection, 62,
              10.0, 5000.0,
              "UNCALIBRATED placeholder. Depends on exchanger volume and flow rate; "
              "measure it on a machine with water in it.");
    addLinked("cooling_flush.min_idle_time", "Trust Group After (s)", kDouble, sBrewPidSection, 63,
              0.0, 3600.0,
              "How long the group must sit undisturbed before its reading is believed. "
              "Set by the group's copper mass - also uncalibrated.");
    addLinked("cooling_flush.recovery_time", "Recovery Pause (s)", kDouble, sBrewPidSection, 64,
              0.0, 30.0, "Settling time between the flush and extraction");
    addLinked("cooling_flush.max_time", "Flush Cap (s)", kDouble, sBrewPidSection, 65,
              1.0, 60.0, "Backstop. Cannot be disabled.");
    addLinked("cooling_flush.pump_percent", "Cooling Flush Pump (%)", kDouble, sBrewPidSection, 66,
              0.0, 100.0, "");
    addLinkedBool("cooling_flush.calibrated", "Coefficients Measured", sBrewPidSection, 67,
                  "Tick only once the two numbers above come from real measurements.");

    // --- Hardware -------------------------------------------------------
    addLinkedBool("pump.load_shed", "Load Shed (heater and pump never together)",
                  sHardwareRelaySection, 70,
                  "Keeps the machine inside a 15 A circuit.");
    addLinked("hardware.sensors.scale.samples", "Scale Averaging Samples", kInteger,
              sScaleSection, 80, 1.0, 64.0, "Fewer is faster and noisier");

    _ready = true;
}


namespace headconfig {

    namespace {

        Config store;
        bool started = false;

    } // namespace

    bool begin() {
        // Upstream's Config::begin() loads the JSON from the filesystem and
        // writes a fresh one from the registered defaults if there is none -
        // the right behaviour on a new board, so a missing file is not an
        // error.
        if (!store.begin()) {
            Serial.println("config: could not open or create the store");

            return false;
        }

        ParameterRegistry::getInstance().initialize(store);
        machinelink::setConfigAckHandler(onConfigAck);
        started = true;

        Serial.printf("config: %d parameters registered\n", parameterCount());

        return true;
    }

    void loop() {
        if (started) {
            // Upstream's debounce: a burst of edits becomes one write.
            ParameterRegistry::getInstance().processPeriodicSave();
        }
    }

    Config& config() {
        return store;
    }

    bool ready() {
        return started;
    }

    int parameterCount() {
        return static_cast<int>(ParameterRegistry::getInstance().getParameters().size());
    }

    uint32_t refusedByController() {
        return refusedCount;
    }

    bool hasPendingAcks() {
        return !pendingPrevious.empty();
    }

    void pushAllToController() {
        if (!started) {
            return;
        }

        int sent = 0;

        for (const auto& param : ParameterRegistry::getInstance().getParameters()) {
            if (!param) {
                continue;
            }

            // Strings are not pushed. Every controller-owned setting is
            // numeric; the only string parameters are the head's own, which
            // the Teensy would answer "notmine" to anyway.
            if (param->getType() == kCString) {
                continue;
            }

            // Deliberately NOT recorded in pendingPrevious. These are this
            // board's own stored values, so there is nothing to revert to -
            // a refusal here means the two sides disagree about a DEFAULT,
            // which onConfigAck reports and leaves alone rather than
            // papering over.
            char text[24];
            snprintf(text, sizeof(text), "%g", param->getValue());
            machinelink::sendConfig(param->getId(), text);
            sent++;

            // Paced deliberately. Twenty-odd frames back to back would
            // overrun the Teensy's serial buffer while it is also sending
            // telemetry, and a dropped config frame is a setting that
            // silently did not apply.
            delay(20);
        }

        Serial.printf("config: pushed %d values to the controller\n", sent);
    }

} // namespace headconfig
