#include "MspLocationSource.h"

#include <Arduino.h>
#include <cstring>

namespace ff {

void MspLocationSource::begin(Stream& serial, uint32_t request_interval_ms) {
    stream_ = &serial;
    msp_.begin(serial);
    request_interval_ms_ = request_interval_ms;
    started_ = true;
}

void MspLocationSource::service() {
    if (!started_) {
        return;
    }

    // Drain the bytes already buffered on the port -- never wait for more. Cap the
    // work per call so a flooded buffer can't stall the loop.
    for (int budget = 256; budget > 0 && stream_->available() > 0; budget--) {
        if (parser_.feed(static_cast<uint8_t>(stream_->read()))) {
            handleFrame();
        }
    }

    const uint32_t now = millis();

    // Ask the FC for a fresh fix on a rate-limited schedule (non-blocking write).
    if (now - last_request_ms_ >= request_interval_ms_) {
        last_request_ms_ = now;
        msp_.send(MSP_RAW_GPS, nullptr, 0);
    }

    // If the FC has gone quiet, stop claiming a valid fix.
    if (ever_fixed_ && (now - last_fix_ms_) > stale_after_ms_) {
        cached_.valid = false;
    }
}

void MspLocationSource::handleFrame() {
    if (parser_.id() != MSP_RAW_GPS || parser_.size() != sizeof(msp_raw_gps_t)) {
        return;
    }
    msp_raw_gps_t gps;
    std::memcpy(&gps, parser_.payload(), sizeof(gps));

    cached_.valid = (gps.fixType != MSP_GPS_NO_FIX);
    cached_.lat = gps.lat;                                      // deg * 1e7
    cached_.lon = gps.lon;                                      // deg * 1e7
    cached_.alt_m = gps.alt;                                    // metres
    cached_.speed_cms = static_cast<uint16_t>(gps.groundSpeed);   // cm/s
    cached_.course_ddeg = static_cast<uint16_t>(gps.groundCourse); // decidegrees
    // Arm state is not read here yet; left false until MSP status is wired in.

    last_fix_ms_ = millis();
    ever_fixed_ = true;
}

}  // namespace ff
