#pragma once
//
// Pure great-circle geodesy on a spherical Earth.
//
// Extracted from v1's GNSSManager so the v2 core (Follow slot projection,
// target-distance checks) has no dependency on the hardware-backed manager.
// Formulas are the ones v1 shipped -- haversine distance, initial bearing, and
// destination point -- so results match the legacy implementation.
//
// All angles are degrees, all distances metres. Latitude/longitude are plain
// doubles here; callers convert from the wire's deg*1e7 fixed point.
//
namespace ff {
namespace geo {

constexpr double kEarthRadiusM = 6371000.0;
constexpr double kPi = 3.14159265358979323846;

inline double toRad(double deg) { return deg * kPi / 180.0; }
inline double toDeg(double rad) { return rad * 180.0 / kPi; }

// Haversine surface distance between two points, metres.
double distanceM(double lat1, double lon1, double lat2, double lon2);

// Initial bearing from point 1 to point 2, degrees clockwise from north in
// [0, 360).
double bearingDeg(double lat1, double lon1, double lat2, double lon2);

// Destination point dist_m from (lat, lon) along bearing_deg.
void pointAtDistance(double lat, double lon, double dist_m, double bearing_deg,
                     double& out_lat, double& out_lon);

}  // namespace geo
}  // namespace ff
