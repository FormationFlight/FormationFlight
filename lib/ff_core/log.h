#pragma once
//
// A small in-memory log the web UI can read.
//
// This is not a convenience. On every ESP8266 target the console UART *is* the
// MSP UART: printing a diagnostic there injects bytes into the flight
// controller's serial link. So the usual answer, print and watch the console,
// is actively harmful on the boards most likely to need debugging, and a log
// that lives in RAM and comes out over HTTP is the only safe one.
//
// Fixed capacity, no allocation, oldest entry overwritten. Sized for an ESP8285
// with a web server already resident: 48 entries of 72 characters is about
// 3.7 KB, which is affordable where a heap dump is not.
//
#include <cstdarg>
#include <cstddef>
#include <cstdint>

namespace ff {

constexpr size_t kLogCapacity = 48;
constexpr size_t kLogTextLen = 72;

enum class LogLevel : uint8_t {
    Debug = 0,
    Info = 1,
    Warn = 2,
    Error = 3,
};

const char* logLevelName(LogLevel level);

struct LogEntry {
    uint32_t ms = 0;
    LogLevel level = LogLevel::Info;
    char text[kLogTextLen + 1] = {0};
};

// Supplies the timestamp. Injected rather than calling millis() directly, so
// this module stays free of Arduino and the tests can drive time.
using LogClockFn = uint32_t (*)();

// Optional echo, called once per accepted entry.
//
// Only ever installed on a board with a console UART separate from the MSP
// UART - which is to say, not on an ESP8266. The ring stays the primary
// channel regardless: a serial console nobody has open records nothing, and
// the point of this module is that the diagnostics survive until someone asks
// for them.
using LogSinkFn = void (*)(const LogEntry&);

class LogRing {
public:
    void setClock(LogClockFn fn) { clock_ = fn; }
    void setSink(LogSinkFn fn) { sink_ = fn; }
    // Entries below this level are discarded at the call site, so a Debug-heavy
    // build costs nothing but the format call's arguments when set to Info.
    void setMinLevel(LogLevel level) { min_level_ = level; }
    LogLevel minLevel() const { return min_level_; }

    void add(LogLevel level, const char* fmt, ...);
    void vadd(LogLevel level, const char* fmt, va_list args);

    size_t size() const { return count_; }
    size_t capacity() const { return kLogCapacity; }
    // Total ever logged, so a polling client can tell it missed entries. Same
    // contract as FrameLog.
    uint32_t total() const { return total_; }
    // Index 0 is the most recent.
    const LogEntry& at(size_t i) const;
    void clear();

    // Counts since boot, for the status view: a node with errors it has already
    // scrolled past should still say so.
    uint32_t warnings() const { return warnings_; }
    uint32_t errors() const { return errors_; }

private:
    LogEntry entries_[kLogCapacity];
    size_t head_ = 0;
    size_t count_ = 0;
    uint32_t total_ = 0;
    uint32_t warnings_ = 0;
    uint32_t errors_ = 0;
    LogLevel min_level_ = LogLevel::Info;
    LogClockFn clock_ = nullptr;
    LogSinkFn sink_ = nullptr;
};

// The one the firmware uses.
LogRing& logRing();

}  // namespace ff

// Deliberately macros: the level test happens before the arguments are
// evaluated, so a filtered-out debug line costs a comparison.
#define FF_LOG(level, ...)                                  \
    do {                                                    \
        if ((level) >= ff::logRing().minLevel()) {          \
            ff::logRing().add((level), __VA_ARGS__);        \
        }                                                   \
    } while (0)

#define FF_LOGD(...) FF_LOG(ff::LogLevel::Debug, __VA_ARGS__)
#define FF_LOGI(...) FF_LOG(ff::LogLevel::Info, __VA_ARGS__)
#define FF_LOGW(...) FF_LOG(ff::LogLevel::Warn, __VA_ARGS__)
#define FF_LOGE(...) FF_LOG(ff::LogLevel::Error, __VA_ARGS__)
