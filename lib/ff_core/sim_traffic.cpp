#include "sim_traffic.h"

#include <cmath>

#include "geo.h"

namespace ff {

namespace {

constexpr int kHexSides = 6;

uint16_t courseToDdeg(double deg) {
    double d = std::fmod(deg, 360.0);
    if (d < 0.0) {
        d += 360.0;
    }
    uint32_t ddeg = static_cast<uint32_t>(std::lround(d * 10.0));
    if (ddeg >= 3600) {
        ddeg = 0;  // 360.0 rounds up into the next lap
    }
    return static_cast<uint16_t>(ddeg);
}

}  // namespace

SimPeerState simPeerAt(const SimPeerConfig& cfg, uint32_t elapsed_ms) {
    SimPeerState s;
    s.lat = cfg.lat;
    s.lon = cfg.lon;
    s.alt_m = cfg.alt_m;
    s.speed_cms = 0;
    s.course_ddeg = courseToDdeg(cfg.course_deg);

    const double t = static_cast<double>(elapsed_ms) / 1000.0;
    const double travelled = cfg.speed_ms * t;

    switch (cfg.mode) {
        case SimMode::Static:
            break;

        case SimMode::Line: {
            geo::pointAtDistance(cfg.lat, cfg.lon, travelled, cfg.course_deg, s.lat, s.lon);
            s.speed_cms = static_cast<uint16_t>(std::lround(cfg.speed_ms * 100.0));
            break;
        }

        case SimMode::Circle: {
            if (cfg.radius_m <= 0.0) {
                break;
            }
            // Angle subtended so far, as seen from the centre.
            const double angle_deg = geo::toDeg(travelled / cfg.radius_m);
            geo::pointAtDistance(cfg.lat, cfg.lon, cfg.radius_m, angle_deg, s.lat, s.lon);
            // Travelling anticlockwise-to-clockwise around the centre, the
            // heading is the tangent: 90 degrees off the radial.
            s.course_ddeg = courseToDdeg(angle_deg + 90.0);
            s.speed_cms = static_cast<uint16_t>(std::lround(cfg.speed_ms * 100.0));
            break;
        }

        case SimMode::Hex: {
            const double side = cfg.radius_m;
            if (side <= 0.0) {
                break;
            }
            const double perimeter = side * kHexSides;
            // Distance around the closed loop, wrapped.
            double along = std::fmod(travelled, perimeter);
            if (along < 0.0) {
                along += perimeter;
            }
            const int leg = static_cast<int>(along / side) % kHexSides;
            const double leg_progress = along - (leg * side);

            // A regular hexagon's vertices sit one side length from the centre,
            // 60 degrees apart. The edge leaving vertex i runs at 120 + 60i.
            const double vertex_bearing = leg * (360.0 / kHexSides);
            const double edge_bearing = vertex_bearing + 120.0;

            double vlat = 0.0;
            double vlon = 0.0;
            geo::pointAtDistance(cfg.lat, cfg.lon, side, vertex_bearing, vlat, vlon);
            geo::pointAtDistance(vlat, vlon, leg_progress, edge_bearing, s.lat, s.lon);

            // Altitude ramps linearly to a peak at the half-way vertex and back,
            // so a follower sees a real climb and descent rather than a step.
            const double half = perimeter / 2.0;
            const double frac = (along <= half) ? (along / half) : ((perimeter - along) / half);
            s.alt_m = static_cast<int16_t>(std::lround(cfg.alt_m + kSimHexClimbM * frac));

            s.course_ddeg = courseToDdeg(edge_bearing);
            s.speed_cms = static_cast<uint16_t>(std::lround(cfg.speed_ms * 100.0));
            break;
        }
    }

    return s;
}

PositionPacket simPositionPacket(const SimPeerConfig& cfg, const SimPeerState& state) {
    PositionPacket p{};
    p.uid = cfg.uid;
    p.lat = static_cast<int32_t>(std::lround(state.lat * 1e7));
    p.lon = static_cast<int32_t>(std::lround(state.lon * 1e7));
    p.alt_m = state.alt_m;
    p.speed_cms = state.speed_cms;
    p.course_ddeg = state.course_ddeg;
    // A simulated peer always claims a fix; one without a fix is not followable
    // and would make the simulator useless for testing Follow.
    p.flags = POSITION_FLAG_HAS_FIX;
    return p;
}

}  // namespace ff
