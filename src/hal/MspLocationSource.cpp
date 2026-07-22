#include "MspLocationSource.h"

#include <Arduino.h>

namespace ff {

void MspLocationSource::begin(Stream& serial, uint32_t poll_interval_ms) {
    msp_.begin(serial);
    poll_interval_ms_ = poll_interval_ms;
    started_ = true;
}

void MspLocationSource::service() {
    if (!started_ || (millis() - last_poll_ms_) < poll_interval_ms_) {
        return;
    }
    last_poll_ms_ = millis();

    msp_raw_gps_t gps{};
    if (!msp_.request(MSP_RAW_GPS, &gps, sizeof(gps))) {
        // No response this cycle; keep the previous fix but mark it stale.
        cached_.valid = false;
        return;
    }

    cached_.valid = (gps.fixType != MSP_GPS_NO_FIX);
    cached_.lat = gps.lat;                                    // deg * 1e7
    cached_.lon = gps.lon;                                    // deg * 1e7
    cached_.alt_m = gps.alt;                                  // metres
    cached_.speed_cms = static_cast<uint16_t>(gps.groundSpeed);   // cm/s
    cached_.course_ddeg = static_cast<uint16_t>(gps.groundCourse); // decidegrees
    // Arm state is not read here yet; left false until the MSP status exchange
    // is wired in.
}

}  // namespace ff
