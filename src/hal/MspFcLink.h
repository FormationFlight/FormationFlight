#pragma once
#include <Stream.h>

#include "follow.h"
#include "msp_fc.h"
#include "msp_parser.h"
#include "node.h"

namespace ff {

// The node's non-blocking MSP link to an attached flight controller.
//
// Owns the receive side of the MSP UART: service() drains whatever bytes are
// already buffered through the incremental MspParser (v1 and v2 framing) and
// folds each reply into a telemetry cache; on rate-limited schedules it issues
// requests as buffered writes. Nothing here ever waits on the port, so the
// beacon path is never stalled -- the property v2's Phase 1e established for the
// GPS poll, now covering everything Follow needs from the FC as well.
//
// Serves two consumers:
//   - ILocationSource: the FC's GPS fix (MSP_RAW_GPS) plus its arm state, for the
//     beacon. Replaces the earlier MspLocationSource.
//   - IFollowFc: modes (GCS NAV / HEADING HOLD / armed), home-relative altitude,
//     RC channels, mixer platform type, and the outbound follow commands.
//
// Identity (variant, version, mixer) is requested until answered. Mode flags
// come from MSP2_INAV_STATUS on INAV -- its full-width box bitmask reaches boxes
// past index 31, which MSP_STATUS's 32-bit field cannot -- and MSP_STATUS
// elsewhere; both are mapped through MSP_BOXIDS into a fixed FcMode bitmap.
// MSP_ALTITUDE and MSP_RC are only polled while Follow says it needs them.
//
// MspRadarOutput shares the same Stream for its writes; that is fine, each write
// is a whole frame.
class MspFcLink : public ILocationSource, public IFollowFc {
public:
    void begin(Stream& serial);
    void service(uint32_t now_ms);

    // True once the FC has identified itself and is still answering status.
    bool connected() const;
    bool isInav() const { return haveVariant_ && fcVariantIsInav(variant_); }
    const char* variant() const { return variant_; }
    FcVersion version() const { return version_; }

    // ILocationSource
    NodeLocation getLocation() override { return loc_; }

    // IFollowFc
    bool armed() override;
    bool gcsNavActive() override;
    bool headingHoldActive() override;
    bool rcChannelUs(uint8_t channel1Based, uint16_t* out_us) override;
    int32_t localAltitudeCm() override { return altitudeCm_; }
    FcPlatform platformType() override { return platform_; }
    void sendFollowWaypoint(int32_t lat_1e7, int32_t lon_1e7, int32_t alt_cm,
                            int16_t heading_deg) override;
    void sendSetHead(int16_t heading_deg) override;
    void sendGvar(uint8_t index, int32_t value) override;
    void setTelemetryNeeds(bool altitude, bool rc) override {
        needAltitude_ = altitude;
        needRc_ = rc;
    }

private:
    void handleFrame(uint32_t now_ms);
    bool modesFresh() const;
    void request1(uint8_t id);
    void request2(uint16_t id);
    void command1(uint8_t id, const uint8_t* payload, size_t len);
    void command2(uint16_t id, const uint8_t* payload, size_t len);
    static bool due(uint32_t now_ms, uint32_t last_ms, uint32_t interval_ms) {
        return (now_ms - last_ms) >= interval_ms;
    }

    Stream* stream_ = nullptr;
    MspParser parser_;
    bool started_ = false;
    uint32_t now_ = 0;

    // Identity (one-shot, retried until answered).
    char variant_[5] = {0};
    bool haveVariant_ = false;
    FcVersion version_;
    bool haveVersion_ = false;
    FcPlatform platform_ = FcPlatform::Unknown;
    uint32_t lastIdentReqMs_ = 0;

    // Box list (re-polled slowly in case the FC's mode set changes).
    uint8_t boxIds_[kMspMaxPayload] = {0};
    size_t boxIdCount_ = 0;
    bool haveBoxIds_ = false;
    uint32_t lastBoxIdsReqMs_ = 0;

    // Position.
    NodeLocation loc_;
    uint32_t lastGpsReqMs_ = 0;
    uint32_t lastFixMs_ = 0;
    bool everFixed_ = false;

    // Modes / arm state.
    uint32_t modes_ = 0;
    bool armedFlag_ = false;      // from INAV armingFlags when available
    bool haveArmedFlag_ = false;
    uint32_t lastStatusMs_ = 0;   // last status reply
    bool haveStatus_ = false;
    uint32_t lastStatusReqMs_ = 0;

    // Follow telemetry, polled on demand.
    bool needAltitude_ = false;
    bool needRc_ = false;
    int32_t altitudeCm_ = 0;
    uint32_t lastAltReqMs_ = 0;
    uint16_t rc_[kMspMaxRcChannels] = {0};
    uint32_t lastRcReqMs_ = 0;
};

}  // namespace ff
