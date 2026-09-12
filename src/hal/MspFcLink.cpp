#include "MspFcLink.h"

#include <Arduino.h>
#include <cstring>

namespace ff {

namespace {
constexpr uint32_t kGpsIntervalMs = 200;
constexpr uint32_t kStatusIntervalMs = 100;
constexpr uint32_t kAltitudeIntervalMs = 100;
constexpr uint32_t kRcIntervalMs = 100;
constexpr uint32_t kIdentRetryMs = 1000;
constexpr uint32_t kBoxIdsIntervalMs = 5000;
// Cached values are only trusted this long after the last reply.
constexpr uint32_t kFixStaleMs = 2000;
constexpr uint32_t kStatusStaleMs = 1000;

constexpr size_t kTxBuf = kMspV2Overhead + kMspSetWpPayloadSize;
}  // namespace

void MspFcLink::begin(Stream& serial) {
    stream_ = &serial;
    started_ = true;
}

bool MspFcLink::connected() const { return haveVariant_ && modesFresh(); }

bool MspFcLink::modesFresh() const {
    return haveStatus_ && (now_ - lastStatusMs_) <= kStatusStaleMs;
}

void MspFcLink::service(uint32_t now_ms) {
    if (!started_) {
        return;
    }
    now_ = now_ms;

    // Drain what's already buffered -- never wait for more. Cap the work per
    // call so a flooded port can't stall the loop.
    for (int budget = 256; budget > 0 && stream_->available() > 0; budget--) {
        if (parser_.feed(static_cast<uint8_t>(stream_->read()))) {
            handleFrame(now_ms);
        }
    }

    // Identity: ask until answered. Mixer only exists on INAV.
    if (due(now_ms, lastIdentReqMs_, kIdentRetryMs) &&
        (!haveVariant_ || !haveVersion_ || (isInav() && platform_ == FcPlatform::Unknown))) {
        lastIdentReqMs_ = now_ms;
        if (!haveVariant_) request1(kMspFcVariant);
        if (!haveVersion_) request1(kMspFcVersion);
        if (isInav() && platform_ == FcPlatform::Unknown) request2(kMsp2InavMixer);
    }

    if (haveVariant_ && (!haveBoxIds_ || due(now_ms, lastBoxIdsReqMs_, kBoxIdsIntervalMs)) &&
        due(now_ms, lastBoxIdsReqMs_, kIdentRetryMs)) {
        lastBoxIdsReqMs_ = now_ms;
        request1(kMspBoxIds);
    }

    if (due(now_ms, lastGpsReqMs_, kGpsIntervalMs)) {
        lastGpsReqMs_ = now_ms;
        request1(kMspRawGps);
    }

    if (haveVariant_ && due(now_ms, lastStatusReqMs_, kStatusIntervalMs)) {
        lastStatusReqMs_ = now_ms;
        if (isInav()) {
            request2(kMsp2InavStatus);
        } else {
            request1(kMspStatus);
        }
    }

    if (needAltitude_ && haveVariant_ && due(now_ms, lastAltReqMs_, kAltitudeIntervalMs)) {
        lastAltReqMs_ = now_ms;
        request1(kMspAltitude);
    }
    if (needRc_ && haveVariant_ && due(now_ms, lastRcReqMs_, kRcIntervalMs)) {
        lastRcReqMs_ = now_ms;
        request1(kMspRc);
    }

    // If the FC has gone quiet, stop claiming a valid fix.
    if (everFixed_ && (now_ms - lastFixMs_) > kFixStaleMs) {
        loc_.valid = false;
    }
    if (!modesFresh()) {
        loc_.armed = false;
    }
}

void MspFcLink::handleFrame(uint32_t now_ms) {
    const uint8_t* p = parser_.payload();
    const size_t len = parser_.size();

    if (parser_.version() == 1) {
        switch (parser_.id()) {
            case kMspRawGps: {
                FcRawGps gps;
                if (!decodeRawGps(p, len, gps)) return;
                loc_.valid = gps.fix_type != kMspGpsNoFix;
                loc_.lat = gps.lat;
                loc_.lon = gps.lon;
                loc_.alt_m = gps.alt_m;
                loc_.speed_cms = static_cast<uint16_t>(gps.speed_cms);
                loc_.course_ddeg = static_cast<uint16_t>(gps.course_ddeg);
                lastFixMs_ = now_ms;
                everFixed_ = true;
                break;
            }
            case kMspFcVariant:
                if (decodeFcVariant(p, len, variant_)) haveVariant_ = true;
                break;
            case kMspFcVersion:
                if (decodeFcVersion(p, len, version_)) haveVersion_ = true;
                break;
            case kMspBoxIds: {
                size_t n = len;
                if (n > sizeof(boxIds_)) n = sizeof(boxIds_);
                std::memcpy(boxIds_, p, n);
                boxIdCount_ = n;
                haveBoxIds_ = true;
                break;
            }
            case kMspStatus: {
                const uint8_t* flags;
                size_t flagLen;
                if (!statusModeFlags(p, len, flags, flagLen)) return;
                modes_ = decodeActiveModes(flags, flagLen, boxIds_, boxIdCount_);
                haveArmedFlag_ = false;
                haveStatus_ = true;
                lastStatusMs_ = now_ms;
                loc_.armed = armed();
                break;
            }
            case kMspAltitude:
                decodeAltitudeCm(p, len, altitudeCm_);
                break;
            case kMspRc:
                // A short/odd reply leaves untouched channels as they were.
                decodeRc(p, len, rc_, kMspMaxRcChannels);
                break;
            default:
                break;
        }
        return;
    }

    // MSP v2 replies.
    switch (parser_.id()) {
        case kMsp2InavStatus: {
            const uint8_t* flags;
            size_t flagLen;
            if (!inavStatusModeFlags(p, len, flags, flagLen)) return;
            modes_ = decodeActiveModes(flags, flagLen, boxIds_, boxIdCount_);
            haveArmedFlag_ = inavStatusArmed(p, len, armedFlag_);
            haveStatus_ = true;
            lastStatusMs_ = now_ms;
            loc_.armed = armed();
            break;
        }
        case kMsp2InavMixer:
            decodeMixerPlatform(p, len, platform_);
            break;
        default:
            break;
    }
}

// ---- IFollowFc reads ----------------------------------------------------------

bool MspFcLink::armed() {
    if (!modesFresh()) return false;
    return haveArmedFlag_ ? armedFlag_ : modeActive(modes_, FC_MODE_ARM);
}

bool MspFcLink::gcsNavActive() { return modesFresh() && modeActive(modes_, FC_MODE_GCSNAV); }

bool MspFcLink::headingHoldActive() { return modesFresh() && modeActive(modes_, FC_MODE_MAG); }

bool MspFcLink::rcChannelUs(uint8_t channel1Based, uint16_t* out_us) {
    if (channel1Based < 1 || channel1Based > kMspMaxRcChannels) return false;
    if (!connected()) return false;
    // A transient poll miss returns the last successfully parsed value.
    *out_us = rc_[channel1Based - 1];
    return true;
}

// ---- IFollowFc writes ---------------------------------------------------------

void MspFcLink::sendFollowWaypoint(int32_t lat_1e7, int32_t lon_1e7, int32_t alt_cm,
                                   int16_t heading_deg) {
    // MSP_SET_WP #255: INAV follow-me / POSHOLD target. Requires NAV POSHOLD +
    // GCS NAV active on the follower FC. alt is home-relative (p3 bit0 = 0).
    uint8_t payload[kMspSetWpPayloadSize];
    const size_t n = encodeSetWp(kMspFollowWaypoint, kMspWpActionWaypoint, lat_1e7, lon_1e7,
                                 alt_cm, heading_deg, 0, 0, 0, payload, sizeof(payload));
    command1(kMspSetWp, payload, n);
}

void MspFcLink::sendSetHead(int16_t heading_deg) {
    uint8_t payload[kMspSetHeadPayloadSize];
    const size_t n = encodeSetHead(heading_deg, payload, sizeof(payload));
    command1(kMspSetHead, payload, n);
}

void MspFcLink::sendGvar(uint8_t index, int32_t value) {
    // MSP2_INAV_SET_GVAR exists on INAV 9.0+ only; silently no-op elsewhere so
    // callers never need to check.
    if (!isInav() || !haveVersion_ || version_.major < 9) return;
    uint8_t payload[kMspSetGvarPayloadSize];
    const size_t n = encodeSetGvar(index, value, payload, sizeof(payload));
    command2(kMsp2InavSetGvar, payload, n);
}

// ---- Transport ----------------------------------------------------------------

void MspFcLink::request1(uint8_t id) { command1(id, nullptr, 0); }
void MspFcLink::request2(uint16_t id) { command2(id, nullptr, 0); }

void MspFcLink::command1(uint8_t id, const uint8_t* payload, size_t len) {
    if (stream_ == nullptr) return;
    uint8_t buf[kTxBuf];
    const size_t n = buildMspV1(id, payload, len, buf, sizeof(buf));
    if (n) stream_->write(buf, n);
}

void MspFcLink::command2(uint16_t id, const uint8_t* payload, size_t len) {
    if (stream_ == nullptr) return;
    uint8_t buf[kTxBuf];
    const size_t n = buildMspV2(id, payload, len, buf, sizeof(buf));
    if (n) stream_->write(buf, n);
}

}  // namespace ff
