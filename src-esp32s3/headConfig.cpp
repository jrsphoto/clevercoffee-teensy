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
#include <set>
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

namespace {

    /**
     * Keys the CONTROLLER owns. Never pushed from here.
     *
     * See addControllerOwned() for why this exists and what it cost to learn.
     */
    std::set<std::string>& controllerOwnedKeys() {
        static std::set<std::string> keys;

        return keys;
    }

    bool isControllerOwned(const char* key) {
        return controllerOwnedKeys().count(key) > 0;
    }

    /** Upstream's Switch::Type ordering: MOMENTARY = 0, TOGGLE = 1. */
    const char* const kBrewSwitchTypes[] = {"Momentary", "Toggle"};

    /**
     * Upstream's scale types, in upstream's order, so the stored numbers mean
     * the same thing on both forks. Type 2 is refused by the controller - see
     * addLinkedEnum.
     */
    const char* const kScaleTypes[] = {
        "HX711 (2 load cell controllers)",
        "HX711 (1 load cell controller)",
        "Bluetooth (not implemented)",
    };

} // namespace

void ParameterRegistry::initialize(Config& config) {
    _config = &config;

    // A numeric setting the CONTROLLER owns. Writes the head's master copy,
    // then pushes it across. Mirrors upstream's addNumericConfigParam, with
    // the globalVar pointer replaced by the link - see the header.
    // Shown always. Named so the registrations below read as what they are.
    const std::function<bool()> always = [] { return true; };

    const auto addLinked = [this, &config, always](const char* path, const char* name, const EditableKind kind,
                                           const int section, const int position,
                                           const double lo, const double hi, const char* help,
                                           const std::function<bool()>& show = nullptr,
                                           const bool reboot = false) {
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
        const auto param = std::make_shared<Parameter>(
            path, name, kind, section, position, getter, setter,
            lo, hi, help[0] != '\0', help, show ? show : always, static_cast<void*>(nullptr));

        param->setRequiresReboot(reboot);

        addParam(param);
    };

    // A boolean the controller owns. Same idea; the Teensy takes 0 or 1.
    const auto addLinkedBool = [this, &config, always](const char* path, const char* name,
                                               const int section, const int position, const char* help,
                                               const std::function<bool()>& show = nullptr,
                                               const bool reboot = false) {
        const std::function<bool()> getter = [this, path]() -> bool {
            return _config->get<bool>(path);
        };

        const std::function<void(bool)> setter = [this, path](const bool v) {
            pendingPrevious[path] = _config->get<bool>(path) ? 1.0 : 0.0;

            _config->set<bool>(path, v);
            markChanged();
            pushToController(path, v ? 1.0 : 0.0);
        };

        const auto param = std::make_shared<Parameter>(
            path, name, kUInt8, section, position, getter, setter,
            help[0] != '\0', help, show ? show : always, static_cast<void*>(nullptr));

        param->setRequiresReboot(reboot);

        addParam(param);
    };

    /**
     * A MEASURED value that belongs to the controller.
     *
     * Everything else here works the other way round: this board holds the
     * master copy and pushes it. That is right for settings a person chooses
     * and wrong for numbers the machine measures, and the difference is not
     * academic.
     *
     * THE SCAR: registering the scale's calibration factors as ordinary
     * settings made this board back-fill them from the shared default of 1.0 -
     * the "uncalibrated" sentinel - and push that at the next handshake. The
     * controller dutifully accepted it through its validating setter and saved
     * it to EEPROM, and a calibration measured against a real 411.7 g mass was
     * gone. Nothing malfunctioned. The architecture simply says the head wins,
     * and the head was carrying a default it had no business having.
     *
     * So these keep NO master copy. The getter reads what the controller last
     * reported, isControllerOwned() keeps them out of the handshake push, and
     * only a person typing a new value sends anything.
     */
    const auto addControllerOwned = [this, always](const char* path, const char* name,
                                               const int section, const int position,
                                               const std::function<double()>& reader,
                                               const char* help,
                                               const std::function<bool()>& show = nullptr) {
        const std::function<void(double)> setter = [path](const double v) {
            // Straight to the controller. Nothing is stored or remembered for
            // reversion here, because this board holds no opinion about the
            // value - the next telemetry frame says what the machine did with
            // it, which is a better answer than our own echo.
            char text[24];
            snprintf(text, sizeof(text), "%g", v);
            machinelink::sendConfig(path, text);
        };

        const auto param = std::make_shared<Parameter>(
            path, name, kDouble, section, position, reader, setter,
            -999999.0, 999999.0, help[0] != '\0', help, show ? show : always,
            static_cast<void*>(nullptr));

        addParam(param);
        controllerOwnedKeys().insert(path);
    };

    /**
     * A pick-one-of-a-list the controller owns.
     *
     * Needed for the scale type, which is upstream's three-way choice between
     * two HX711 wirings and Bluetooth. The Teensy REFUSES Bluetooth, because
     * NimBLE is ESP32-only and that path is designed but not built - so
     * choosing it reverts, which is the honest outcome rather than a machine
     * left with no scale while the page claims otherwise.
     */
    const auto addLinkedEnum = [this, &config, always](const char* path, const char* name,
                                               const int section, const int position,
                                               const char* const* options, const size_t count,
                                               const char* help,
                                               const std::function<bool()>& show = nullptr,
                                               const bool reboot = false) {
        const std::function<double()> getter = [this, path]() -> double {
            return _config->get<int>(path);
        };

        const std::function<void(double)> setter = [this, path](const double v) {
            pendingPrevious[path] = _config->get<int>(path);

            _config->set<int>(path, static_cast<int>(v));
            markChanged();
            pushToController(path, v);
        };

        const auto param = std::make_shared<Parameter>(
            path, name, kEnum, section, position, getter, setter,
            options, count, help[0] != '\0', help, show ? show : always, static_cast<void*>(nullptr));

        param->setRequiresReboot(reboot);

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
    //
    // WHAT IS DELIBERATELY ABSENT, because upstream has it and someone will
    // eventually come looking:
    //
    //   OLED section          - this head drives an ST7796S colour LCD, not a
    //                           monochrome OLED. None of those settings mean
    //                           anything here.
    //   Relay trigger types   - heater, pump and valve polarity are fixed by
    //                           the ULN2003 on the board. A web switch able to
    //                           invert the HEATER drive is the single worst
    //                           knob this machine could offer.
    //   RTD wires / nominal /
    //   reference resistor    - compiled constants in tempHandler.cpp, with
    //                           the reasoning next to them. 4-wire, PT100,
    //                           430R. A wrong value here does not fail, it
    //                           silently reads the wrong temperature on a
    //                           boiler with a 1500 W element in it.
    //   Steam / hot water
    //   switches              - not ported at all; both are manual valves on
    //                           this machine. See brief 9.2.
    //   Status / brew / steam
    //   LEDs                  - not on this board. The encoder's RGB ring is
    //                           the indicator, and it is Teensy-driven.
    //
    // Everything below is real, settable, and goes through the controller's
    // own validating setter.

    // NO LOAD SHED SETTING, AND NO LOAD SHED MECHANISM. John's decision,
    // 2026-11-08 and again 2026-11-09: the machine is specified with a 1000 W
    // element (8.3 A) and a 24 V DC pump, so it never approaches a 120 V
    // outlet's limit and the interlock had nothing to protect. It is now gone
    // from the controller firmware as well - there is no pump::setLoadShed(),
    // no `pumpshed` console command and no gate in pumpHandler.cpp. See
    // pumpHandler.h for the reasoning. Do not re-add it without a measured
    // reason.
    //
    // This leaves the Relays section with nothing in it, so it does not appear
    // at all: the front end only renders sections that have parameters.

    // --- Switches -------------------------------------------------------
    addLinkedEnum("hardware.switches.brew.type", "Brew Switch Type",
                  sHardwareSwitchSection, 100, kBrewSwitchTypes, 2,
                  "Momentary is a spring-return button or lever. Toggle is a switch that "
                  "stays where it is put. Wrong choice and a shot either never starts or "
                  "never stops on its own.");

    // --- Sensors: the scale ---------------------------------------------
    //
    // Upstream's key names and upstream's type numbering, so the two stay
    // cross-referenceable. The calibration factors live here rather than only
    // in the Teensy's EEPROM because they are the numbers most likely to need
    // adjusting, and the two cells need SEPARATE ones: they are mounted
    // mirrored, so one reads negative under load.
    addLinkedBool("hardware.sensors.scale.enabled", "Enable Scale",
                  sHardwareSensorSection, 110,
                  "Off disables brew-by-weight. Brewing by time and by volume still work.");

    addLinkedEnum("hardware.sensors.scale.type", "Scale Type",
                  sHardwareSensorSection, 111, kScaleTypes, 3,
                  "This machine is two load cells under a shared plate, which is type 0: "
                  "a plate spanning two cells splits the load by where the mass sits, so "
                  "only the SUM of both is a real weight. Bluetooth is designed but NOT "
                  "built, and the controller will refuse it.",
                  [this] { return _config->get<bool>("hardware.sensors.scale.enabled"); });

    addLinked("hardware.sensors.scale.samples", "Scale Averaging Samples", kInteger,
              sHardwareSensorSection, 112, 1.0, 20.0, "Fewer is faster and noisier",
              [this] {
                  return _config->get<bool>("hardware.sensors.scale.enabled")
                         && _config->get<int>("hardware.sensors.scale.type") < 2;
              });

    // THE CALIBRATION FACTORS ARE CONTROLLER-OWNED. Read from telemetry, never
    // stored here, and excluded from the handshake push - see
    // isControllerOwned() and the scar it is named after.
    addControllerOwned("hardware.sensors.scale.calibration", "Calibration Factor (cell 1)",
                       sHardwareSensorSection, 113,
                       [] { return machinelink::state().calFactor1; },
                       "Counts per gram for the first cell, as the CONTROLLER currently "
                       "holds it - this board only displays it. 1.0 means uncalibrated, "
                       "and the weight shown is then raw counts: deliberately a silly "
                       "number rather than a plausible wrong one. Calibrate against a "
                       "real mass rather than typing a factor.",
                       [this] {
                           return _config->get<bool>("hardware.sensors.scale.enabled")
                                  && _config->get<int>("hardware.sensors.scale.type") < 2;
                       });

    addControllerOwned("hardware.sensors.scale.calibration2", "Calibration Factor (cell 2)",
                       sHardwareSensorSection, 114,
                       [] { return machinelink::state().calFactor2; },
                       "The second cell's own factor, with its own SIGN. The cells on this "
                       "machine are mirrored, so this one is normally the opposite sign to "
                       "cell 1. Two factors of the same sign on a dual-cell plate is a "
                       "wiring or calibration mistake.",
                       [this] {
                           return _config->get<bool>("hardware.sensors.scale.enabled")
                                  && _config->get<int>("hardware.sensors.scale.type") == 0;
                       });

    addLinked("hardware.sensors.scale.known_weight", "Known Calibration Weight (g)", kDouble,
              sHardwareSensorSection, 115, 1.0, 2000.0,
              "Mass of whatever is put on the plate to calibrate against.",
              [this] {
                  return _config->get<bool>("hardware.sensors.scale.enabled")
                         && _config->get<int>("hardware.sensors.scale.type") < 2;
              });

    // --- Encoder --------------------------------------------------------
    //
    // The section the ESP32 build grew and the port had left as compiled
    // constants in encoderHandler.h. Same keys, same defaults, same help -
    // EXCEPT the address, which is 0x04 on this board. See the override
    // registered in headconfig::begin().
    //
    // Fitted, address and the RGB variant need a restart: the driver is
    // constructed in encoder::init() around a fixed address. The colours apply
    // the moment they are saved.
    const auto encoderOn = [this] {
        return _config->get<bool>("hardware.encoder.enabled");
    };

    const auto encoderRgb = [this] {
        return _config->get<bool>("hardware.encoder.enabled")
               && _config->get<bool>("hardware.encoder.rgb_led");
    };

    const auto encoderColors = [this] {
        return _config->get<bool>("hardware.encoder.enabled")
               && _config->get<bool>("hardware.encoder.rgb_led")
               && _config->get<bool>("hardware.encoder.color_effects.enabled");
    };

    addLinkedBool("hardware.encoder.enabled", "Enable Rotary Encoder",
                  sHardwareEncoderSection, 120,
                  "Enable the DuPPa I2C rotary encoder. Nothing in the control path needs "
                  "it, so running without the knob is supported rather than degraded.",
                  nullptr, true);

    addLinked("hardware.encoder.i2c_address", "I2C Address", kInteger,
              sHardwareEncoderSection, 121, 1.0, 127.0,
              "Set by the A0-A6 solder jumpers on the encoder board. Use the \"Scan for "
              "I2C Devices\" button rather than trusting the datasheet's jumper chart, "
              "which does not always match real hardware. THIS BOARD'S ENCODER IS AT 4 "
              "(0x04), the factory state with no jumpers fitted - which sits in the "
              "I2C-reserved range, so an ordinary scanner skips it and the encoder looks "
              "dead while working perfectly. The ESP32 board's unit was at 102 (0x66).",
              encoderOn, true);

    addLinkedBool("hardware.encoder.rgb_led", "RGB LED",
                  sHardwareEncoderSection, 122,
                  "Enable if the encoder is the RGB LED variant.",
                  encoderOn, true);

    addLinkedBool("hardware.encoder.color_effects.enabled", "Status Color Effects",
                  sHardwareEncoderSection, 123,
                  "Light the encoder's LED by machine status: idle, heating, ready, "
                  "active, error.",
                  encoderRgb);

    addLinked("hardware.encoder.color.idle", "Idle Color", kColor,
              sHardwareEncoderSection, 124, 0.0, 0xFFFFFF,
              "Shown when there is nothing to report. The selected shot profile's colour "
              "overrides this once a profile is chosen.",
              encoderColors);

    addLinked("hardware.encoder.color.heating", "Heating Color", kColor,
              sHardwareEncoderSection, 125, 0.0, 0xFFFFFF,
              "While the PID is driving the heater hard rather than just trimming.",
              encoderColors);

    addLinked("hardware.encoder.color.ready", "Ready Color", kColor,
              sHardwareEncoderSection, 126, 0.0, 0xFFFFFF,
              "At temperature.",
              encoderColors);

    addLinked("hardware.encoder.color.active", "Active Color", kColor,
              sHardwareEncoderSection, 127, 0.0, 0xFFFFFF,
              "Brewing, flushing or backflushing.",
              encoderColors);

    addLinked("hardware.encoder.color.error", "Error Color", kColor,
              sHardwareEncoderSection, 128, 0.0, 0xFFFFFF,
              "A latched fault or an unhealthy boiler probe. This one takes precedence "
              "over everything else, so a profile colour can never mask a fault.",
              encoderColors);

    _ready = true;
}


namespace headconfig {

    namespace {

        Config store;
        bool started = false;

    } // namespace

    bool begin() {
        // THE ENCODER'S ADDRESS IS BOARD-SPECIFIC, and getting this wrong does
        // not produce a disagreement - it produces a dead encoder.
        //
        // The shared Config.h defaults it to 0x66, which is the ESP32 board's
        // unit. The Teensy's is at its factory 0x04, with no jumpers fitted.
        // Because this head holds the master config and pushes it at every
        // handshake, shipping the other board's default would have this board
        // overwrite a working 0x04 with a 0x66 that nothing answers on - the
        // exact failure mode CLAUDE.md warns about, and one that already cost
        // this project an afternoon once.
        //
        // Registered before store.begin() so it is in place for both the
        // fresh-config path and the backfill of a config that predates the
        // key.
        Config::defaultOverrides().insert_or_assign(
            "hardware.encoder.i2c_address", ConfigDef::forInt(0x04, 1, 127));

        // THE SCALE IS FITTED ON THIS MACHINE, and the shared default says
        // false because most upstream machines have none. Caught the same way
        // as the address above: the head pushed `enabled = 0` at the handshake
        // and the verified dual-cell scale switched itself off, which looked
        // like broken hardware rather than a default.
        Config::defaultOverrides().insert_or_assign(
            "hardware.sensors.scale.enabled", ConfigDef::forBool(true));

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

            // Measured values belong to the controller and must NOT be pushed.
            // Pushing one overwrote a real scale calibration with the 1.0
            // "uncalibrated" default - see addControllerOwned().
            if (isControllerOwned(param->getId())) {
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
