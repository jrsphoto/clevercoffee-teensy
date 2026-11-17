#include "Timer.h"

#include <utility>

#include "Arduino.h"
#include "Logger.h"

Timer::Timer(std::function<void()> func, const unsigned long interval, bool start_paused) :
    callback_(std::move(func)), interval_(interval), next_(millis()), running_(!start_paused) {
}

void Timer::pause() {
    running_ = false;
}

void Timer::resume() {
    running_ = true;
}

void Timer::operator()() {
    // SIGNED DIFFERENCE, not `millis() >= next_`.
    //
    // millis() wraps at 2^32 ms, which is 49.7 days - reachable on a machine
    // left powered. With a plain comparison, the moment `next_` wraps past
    // the end while millis() has not yet, every call looks overdue, so the
    // callback fires on EVERY main-loop pass until the clock catches up. That
    // window is as long as the interval: for the temperature sensors, 400 ms
    // of back-to-back sampling, each one a 75 ms blocking read.
    //
    // Subtracting first and testing the sign is wrap-safe across the whole
    // range, because the subtraction wraps the same way the clock does.
    // Found in review 2026-11-17.
    if (running_ && static_cast<int32_t>(millis() - next_) >= 0) {
        LOG(TRACE, "Timer expired, calling function");

        // Update timer
        next_ = millis() + interval_;

        // Call method:
        callback_();
    }
}

void Timer::reset() {
    next_ = millis();
}
