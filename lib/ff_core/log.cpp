#include "log.h"

#include <cstdio>

namespace ff {

const char* logLevelName(LogLevel level) {
    switch (level) {
        case LogLevel::Debug:
            return "debug";
        case LogLevel::Info:
            return "info";
        case LogLevel::Warn:
            return "warn";
        case LogLevel::Error:
            return "error";
    }
    return "info";
}

void LogRing::vadd(LogLevel level, const char* fmt, va_list args) {
    if (level < min_level_) {
        return;
    }
    LogEntry& e = entries_[head_];
    e.ms = (clock_ != nullptr) ? clock_() : 0;
    e.level = level;
    // Truncation is fine and deliberate: a log line long enough to be cut is
    // one that should have been shorter.
    vsnprintf(e.text, sizeof(e.text), fmt, args);
    e.text[kLogTextLen] = '\0';

    head_ = (head_ + 1) % kLogCapacity;
    if (count_ < kLogCapacity) {
        count_++;
    }
    total_++;
    if (level == LogLevel::Warn) {
        warnings_++;
    } else if (level == LogLevel::Error) {
        errors_++;
    }
    // Echoed after the entry is stored, so a sink that blocks or throws away
    // its output cannot cost us the copy the web UI is going to read.
    if (sink_ != nullptr) {
        sink_(e);
    }
}

void LogRing::add(LogLevel level, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vadd(level, fmt, args);
    va_end(args);
}

const LogEntry& LogRing::at(size_t i) const {
    const size_t idx = (head_ + kLogCapacity - 1 - (i % kLogCapacity)) % kLogCapacity;
    return entries_[idx];
}

void LogRing::clear() {
    head_ = 0;
    count_ = 0;
    // total_, warnings_ and errors_ deliberately survive: they are "since boot"
    // counters, and a client's cursor must never walk backwards.
}

LogRing& logRing() {
    static LogRing instance;
    return instance;
}

}  // namespace ff
