#pragma once

#include "../../../src/lib/Follow/FollowDeps.h"
#include <map>
#include <vector>

struct SentWaypoint {
    int32_t lat_1e7;
    int32_t lon_1e7;
    int32_t alt_cm;
    int16_t headingDeg;
};

struct SentGvar {
    uint8_t index;
    int32_t value;
};

// Test double for IFollowMsp. Every outbound call is recorded (not just
// applied) so tests can assert both that something was sent and what.
class FakeMsp : public IFollowMsp {
public:
    // 0 = disarmed, matching the real MSPManager::getState() convention
    // FollowManager.cpp's `msp->getState() == 0` check relies on.
    uint8_t state = 0;
    bool gcsNavActive = true;
    int32_t altitudeCm = 0;
    InavPlatformType platformType = INAV_PLATFORM_MULTIROTOR;
    bool headingHoldActive = true;

    // channel1Based -> pulse width (us). An absent key simulates
    // getRcChannelUs() returning false -- "no FC connected, or channel out
    // of MSP_RC's range," the one case resolveAxisOffset() falls back to
    // configuredM for.
    std::map<uint8_t, uint16_t> rcChannelUs;

    std::vector<SentWaypoint> sentWaypoints;
    std::vector<int16_t> sentHeadings;
    std::vector<SentGvar> sentGvars;

    uint8_t getState() override { return state; }
    bool isGCSNavActive() override { return gcsNavActive; }

    bool getRcChannelUs(uint8_t channel1Based, uint16_t *outUs) override
    {
        auto it = rcChannelUs.find(channel1Based);
        if (it == rcChannelUs.end())
        {
            return false;
        }
        *outUs = it->second;
        return true;
    }

    int32_t local_altitude_cm() override { return altitudeCm; }
    InavPlatformType getPlatformType() override { return platformType; }

    void sendFollowWaypoint(int32_t lat_1e7, int32_t lon_1e7, int32_t alt_cm, int16_t headingDeg) override
    {
        sentWaypoints.push_back({lat_1e7, lon_1e7, alt_cm, headingDeg});
    }

    void sendSetHead(int16_t headingDeg) override
    {
        sentHeadings.push_back(headingDeg);
    }

    bool isHeadingHoldActive() override { return headingHoldActive; }

    void sendGvar(uint8_t index, int32_t value) override
    {
        sentGvars.push_back({index, value});
    }
};
