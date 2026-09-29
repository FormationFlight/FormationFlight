#pragma once
//
// Simulated peer motion, for bench and hardware-in-the-loop testing.
//
// A formation feature is miserable to test: it needs two aircraft, two GPS
// fixes, and a field. This module produces the other aircraft. Each simulated
// peer is a closed-form function of elapsed time - not an integrator - so the
// same elapsed time always yields the same position, the host tests are exact,
// and a paused or slow loop cannot accumulate drift.
//
// What this is NOT: a replacement for the receive path. The positions computed
// here get encoded into real protocol packets, encrypted with the real group
// key and pushed through Node::onReceive by hal/SimRadio, so the peer table,
// Follow and the MSP radar output cannot tell them from RF. Simulating further
// up the stack would test the simulator instead of the firmware.
//
#include <cstdint>

#include "protocol.h"

namespace ff {

enum class SimMode : uint8_t {
    Static = 0,  // parked, reporting a fixed course
    Line = 1,    // straight out from the origin on `course_deg`
    Circle = 2,  // orbiting the origin at `radius_m`
    Hex = 3,     // closed hexagon patrol with a climb and descent
};

// The peak of the hexagon patrol's altitude ramp, metres above its start.
constexpr double kSimHexClimbM = 80.0;

struct SimPeerConfig {
    uint32_t uid = 0;
    char name[kMaxNameLen + 1] = {0};
    SimMode mode = SimMode::Static;
    double lat = 0.0;          // degrees; origin or centre depending on mode
    double lon = 0.0;
    int16_t alt_m = 0;         // metres MSL at the start of the path
    double speed_ms = 0.0;
    double course_deg = 0.0;   // Static/Line only
    double radius_m = 100.0;   // Circle radius, or Hex side length
};

struct SimPeerState {
    double lat = 0.0;
    double lon = 0.0;
    int16_t alt_m = 0;
    uint16_t speed_cms = 0;
    uint16_t course_ddeg = 0;
};

// The peer's state `elapsed_ms` after its path started.
SimPeerState simPeerAt(const SimPeerConfig& cfg, uint32_t elapsed_ms);

// Fills a PositionPacket from a simulated state, so callers do not have to
// repeat the unit conversions (and get them subtly wrong).
PositionPacket simPositionPacket(const SimPeerConfig& cfg, const SimPeerState& state);

}  // namespace ff
