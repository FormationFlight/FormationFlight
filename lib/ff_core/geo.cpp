#include "geo.h"

#include <cmath>

namespace ff {
namespace geo {

double distanceM(double lat1, double lon1, double lat2, double lon2) {
    const double lat1r = toRad(lat1);
    const double lon1r = toRad(lon1);
    const double lat2r = toRad(lat2);
    const double lon2r = toRad(lon2);
    const double u = std::sin((lat2r - lat1r) / 2.0);
    const double v = std::sin((lon2r - lon1r) / 2.0);
    return 2.0 * kEarthRadiusM *
           std::asin(std::sqrt(u * u + std::cos(lat1r) * std::cos(lat2r) * v * v));
}

double bearingDeg(double lat1, double lon1, double lat2, double lon2) {
    const double dlon = toRad(lon2 - lon1);
    const double lat1r = toRad(lat1);
    const double lat2r = toRad(lat2);
    const double a1 = std::sin(dlon) * std::cos(lat2r);
    double a2 = std::sin(lat1r) * std::cos(lat2r) * std::cos(dlon);
    a2 = std::cos(lat1r) * std::sin(lat2r) - a2;
    a2 = std::atan2(a1, a2);
    if (a2 < 0.0) {
        a2 += 2.0 * kPi;
    }
    return toDeg(a2);
}

void pointAtDistance(double lat, double lon, double dist_m, double bearing_deg,
                     double& out_lat, double& out_lon) {
    const double lat1 = toRad(lat);
    const double lon1 = toRad(lon);
    const double ang = dist_m / kEarthRadiusM;
    const double brg = toRad(bearing_deg);

    const double lat2 = std::asin(std::sin(lat1) * std::cos(ang) +
                                  std::cos(lat1) * std::sin(ang) * std::cos(brg));
    const double lon2 = lon1 + std::atan2(std::sin(brg) * std::sin(ang) * std::cos(lat1),
                                          std::cos(ang) - std::sin(lat1) * std::sin(lat2));
    out_lat = toDeg(lat2);
    out_lon = toDeg(lon2);
}

}  // namespace geo
}  // namespace ff
