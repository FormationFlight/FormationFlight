#pragma once

#include "FollowDeps.h"

// Real forwarding adapters over the hardware-backed singletons -- production
// only, never compiled into the native test env (see platformio.ini's
// [env:test_native] build_src_filter). Each method is a one-line forward,
// no new logic; FollowManager::getSingleton() (FollowProdAdapters.cpp) is
// the only place that constructs these.

class MSPFollowAdapter : public IFollowMsp {
public:
    uint8_t getState() override;
    bool isGCSNavActive() override;
    bool getRcChannelUs(uint8_t channel1Based, uint16_t *outUs) override;
    int32_t local_altitude_cm() override;
    InavPlatformType getPlatformType() override;
    void sendFollowWaypoint(int32_t lat_1e7, int32_t lon_1e7, int32_t alt_cm, int16_t headingDeg) override;
    void sendSetHead(int16_t headingDeg) override;
    bool isHeadingHoldActive() override;
    void sendGvar(uint8_t index, int32_t value) override;
};

class GNSSFollowAdapter : public IFollowGnss {
public:
    double horizontalDistanceTo(GNSSLocation b) override;
    int16_t courseTo(GNSSLocation b) override;
};

class PeerFollowAdapter : public IFollowPeers {
public:
    const peer_t *getPeerById(uint8_t id) override;
    const peer_t *getPeer(uint8_t index) override;
};
