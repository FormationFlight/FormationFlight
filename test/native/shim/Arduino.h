#pragma once

// Minimal native-only stand-in for the Arduino core, sized to exactly what
// src/lib/Follow/*, src/lib/Peers/PeerManager.h, src/lib/GNSS/GNSSManager.*,
// and src/lib/MSP/MSP.h actually use. Resolved via this
// env's -I test/native/shim, which is ordered ahead of any real Arduino.h on
// the include path (there isn't one for platform=native) -- production
// targets never see this file.
//
// Not a general-purpose Arduino emulation: add to this only when a new
// native-buildable file in the Follow test's dependency graph needs it.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <string>

using std::sin;
using std::cos;
using std::asin;
using std::atan2;
using std::sqrt;

typedef unsigned char byte;

#ifndef M_PI
#define M_PI 3.1415926535897932384626433832795
#endif
#ifndef PI
#define PI M_PI
#endif
#ifndef TWO_PI
#define TWO_PI (PI * 2.0)
#endif
#ifndef HALF_PI
#define HALF_PI (PI / 2.0)
#endif

#define radians(deg) ((deg) * PI / 180.0)
#define degrees(rad) ((rad) * 180.0 / PI)

#undef constrain
#define constrain(amt, low, high) ((amt) < (low) ? (low) : ((amt) > (high) ? (high) : (amt)))

// Not macros: unlike constrain()/radians()/degrees(), min/max are common
// identifiers (std::numeric_limits<T>::max(), etc.) that a textual #define
// would mangle wherever the standard library uses them after this header is
// included. Plain overloads in the global namespace only shadow unqualified
// min()/max() calls (all FollowManager.cpp needs), not std::min/std::max.
template <typename T> T min(T a, T b) { return a < b ? a : b; }
template <typename T> T max(T a, T b) { return a > b ? a : b; }

// millis() is test-controllable (declared here, backed by a settable
// counter defined in Arduino.cpp) so GVAR heartbeat / EEPROM rate-limit
// tests can drive time deterministically instead of racing the wall clock.
unsigned long millis();
void native_millis_set(unsigned long ms);
void native_millis_advance(unsigned long ms);

// Arduino's String, trimmed to the subset FollowManager.cpp/GNSSManager.*
// actually call: construct from a C string, assign, read back via c_str().
class String {
public:
    String() = default;
    String(const char *s) : value(s ? s : "") {}
    String(const String &other) = default;
    String &operator=(const char *s) { value = s ? s : ""; return *this; }
    String &operator=(const String &other) = default;
    const char *c_str() const { return value.c_str(); }
    size_t length() const { return value.length(); }
    bool operator==(const String &other) const { return value == other.value; }
    bool operator==(const char *s) const { return value == s; }
    String operator+(const char *s) const { String r; r.value = value + s; return r; }
    String operator+(const String &other) const { String r; r.value = value + other.value; return r; }
private:
    std::string value;
};

struct SerialShim {
    void printf(const char *fmt, ...) {
        va_list args;
        va_start(args, fmt);
        vfprintf(stdout, fmt, args);
        va_end(args);
    }
    void println(const char *s) { printf("%s\n", s); }
};
extern SerialShim Serial;
