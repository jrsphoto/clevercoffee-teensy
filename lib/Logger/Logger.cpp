#include <utility>

#include "Logger.h"

#if !CC_LOGGER_SERIAL_ONLY
#include <WiFi.h>
#endif

int logLevel;

#if CC_LOGGER_SERIAL_ONLY
Logger::Logger(const uint16_t port) :
    port_(port) {
}
#else
Logger::Logger(const uint16_t port) :
    port_(port), server_(port) {
}
#endif

Logger& Logger::getInstanceImpl(const uint16_t port) {
    static Logger instance{port};
    return instance;
}

void Logger::init(const uint16_t port) {
    getInstanceImpl(port);
}

bool Logger::begin() {
#if !CC_LOGGER_SERIAL_ONLY
    if (WiFi.status() == WL_CONNECTED) {
        Logger::getInstance().server_.begin();
    }
#endif

    // If the serial interface has not been started, start it now:
    if (!Serial) {
        Serial.begin(115200);
    }
    return true;
}

bool Logger::update() {
#if CC_LOGGER_SERIAL_ONLY
    // No network clients to service on the Teensy. The log level is not driven
    // by a web config here either - it crosses the UART from the ESP32 head, so
    // there is deliberately nothing to poll.
    return true;
#else
    if (Logger::getInstance().server_.hasClient()) {
        // If we are already connected to another client, then reject the new connection, otherwise accept the connection.
        if (Logger::getInstance().client_.connected()) {
            LOG(WARNING, "Serial Server Connection rejected");
            Logger::getInstance().server_.available().stop();
        }
        else {
            LOG(INFO, "Serial Server Connection accepted");
            Logger::getInstance().client_ = Logger::getInstance().server_.available();
        }
    }

    // update if the loglevel has changed
    Logger::Level level = static_cast<Logger::Level>(logLevel);

    if (Logger::getCurrentLevel() != level) {
        Logger::setLevel(level);
        LOGF(INFO, "Log level changed to %s", Logger::get_level_identifier(level).c_str());
    }

    return true;
#endif
}

uint16_t Logger::getPort() {
    return Logger::getInstance().getPort();
}

void Logger::log(const Level level, const String& file, const __FlashStringHelper* function, uint32_t line, const char* logmsg) {
    char time[12];
    current_time(time);

#if !CC_LOGGER_SERIAL_ONLY
    if (WiFi.status() == WL_CONNECTED && client_.connected()) {
        client_.print(time);
        client_.print(get_level_identifier(level).c_str());
        client_.print(" ");
        if (level < Level::DEBUG) {
            client_.print(file.c_str());
            client_.print(":");
            client_.print(line);
            client_.print("@");
            client_.print(function);
            client_.print("() ");
        }
        client_.print(logmsg);
        client_.print("\n");
    }
    else
#endif
    {
        Serial.print(time);
        Serial.print(get_level_identifier(level).c_str());
        Serial.print(" ");
        if (level < Level::DEBUG) {
            Serial.print(file.c_str());
            Serial.print(":");
            Serial.print(line);
            Serial.print("@");
            Serial.print(function);
            Serial.print("() ");
        }
        Serial.print(logmsg);
        Serial.print("\n");
    }
}

void Logger::logf(const Level level, const String& file, const __FlashStringHelper* function, uint32_t line, const char* format, ...) {
    // reimplement printf method so that we can take dynamic list of parameters as printf does
    // (we can't simply pass these on to existing printf methods it seems)

    va_list arg;
    va_start(arg, format);
    char temp[64]; // allocate a temp buffer
    char* buffer = temp;
    size_t len = vsnprintf(temp, sizeof(temp), format, arg);
    va_end(arg);

    // if temp buffer was too short, create new one with enough room (includes previous bytes)
    if (len > sizeof(temp) - 1) {
        buffer = new char[len + 1];

        if (!buffer) {
            return;
        }

        va_start(arg, format);
        vsnprintf(buffer, len + 1, format, arg);
        va_end(arg);
    }

    log(level, file, function, line, buffer);

    if (buffer != temp) {
        delete[] buffer;
    }
}

#if CC_LOGGER_SERIAL_ONLY
void Logger::current_time(char* timestamp) {
    // No RTC is set on the Teensy, and a confidently wrong wall clock is worse
    // than none. Uptime is what actually helps when reading a bench log.
    const uint32_t ms = millis();
    snprintf(timestamp, 12, "[%4lu.%03lu] ", (unsigned long)(ms / 1000UL), (unsigned long)(ms % 1000UL));
}
#else
void Logger::current_time(char* timestamp) {
    time_t rawtime;
    struct tm* timeinfo;
    time(&rawtime);
    timeinfo = localtime(&rawtime);
    snprintf(timestamp, 12, "[%02d:%02d:%02d] ", timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
}
#endif

String Logger::get_level_identifier(Logger::Level lvl) {
    switch (lvl) {
        case Level::TRACE:
            return "  TRACE";
        case Level::DEBUG:
            return "  DEBUG";
        case Level::INFO:
            return "   INFO";
        case Level::WARNING:
            return "WARNING";
        case Level::ERROR:
            return "  ERROR";
        case Level::FATAL:
            return "  FATAL";
        default:
            return " SILENT";
    }
}
