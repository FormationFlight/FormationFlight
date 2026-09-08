#pragma once

#include "../GNSS/GNSSManager.h"
#include "../Peers/PeerManager.h"

// Thin seams over the three hardware-backed managers FollowManager reads
// from and writes to. FollowManager holds a pointer to each, injected via its constructor;
// production wiring (FollowProdAdapters.h/.cpp, used by getSingleton())
// forwards to the real singletons, tests inject fakes instead. slotToLatLon()'s
// use of GNSSManager::calculatePointAtDistance() and the free function
// peer_is_stale() are deliberately not part of these interfaces -- both are
// pure/static already (see FollowManager.cpp / PeerManager.h) and need no seam.

class IFollowMsp {
public:
    virtual ~IFollowMsp() = default;
    virtual uint8_t getState() = 0;
    virtual bool isGCSNavActive() = 0;
    virtual bool getRcChannelUs(uint8_t channel1Based, uint16_t *outUs) = 0;
    virtual int32_t local_altitude_cm() = 0;
    virtual InavPlatformType getPlatformType() = 0;
    virtual void sendFollowWaypoint(int32_t lat_1e7, int32_t lon_1e7, int32_t alt_cm, int16_t headingDeg) = 0;
    virtual void sendSetHead(int16_t headingDeg) = 0;
    virtual bool isHeadingHoldActive() = 0;
    virtual void sendGvar(uint8_t index, int32_t value) = 0;
};

class IFollowGnss {
public:
    virtual ~IFollowGnss() = default;
    virtual double horizontalDistanceTo(GNSSLocation b) = 0;
    virtual int16_t courseTo(GNSSLocation b) = 0;
};

class IFollowPeers {
public:
    virtual ~IFollowPeers() = default;
    virtual const peer_t *getPeerById(uint8_t id) = 0;
    virtual const peer_t *getPeer(uint8_t index) = 0;
};
