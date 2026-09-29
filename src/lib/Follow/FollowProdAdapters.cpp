#include "FollowProdAdapters.h"
#include "FollowManager.h"
#include "../MSP/MSPManager.h"
#include "../GNSS/GNSSManager.h"
#include "../Peers/PeerManager.h"

uint8_t MSPFollowAdapter::getState() { return MSPManager::getSingleton()->getState(); }
bool MSPFollowAdapter::isGCSNavActive() { return MSPManager::getSingleton()->isGCSNavActive(); }
bool MSPFollowAdapter::getRcChannelUs(uint8_t channel1Based, uint16_t *outUs) { return MSPManager::getSingleton()->getRcChannelUs(channel1Based, outUs); }
int32_t MSPFollowAdapter::local_altitude_cm() { return MSPManager::getSingleton()->local_altitude_cm(); }
InavPlatformType MSPFollowAdapter::getPlatformType() { return MSPManager::getSingleton()->getPlatformType(); }
void MSPFollowAdapter::sendFollowWaypoint(int32_t lat_1e7, int32_t lon_1e7, int32_t alt_cm, int16_t headingDeg) { MSPManager::getSingleton()->sendFollowWaypoint(lat_1e7, lon_1e7, alt_cm, headingDeg); }
void MSPFollowAdapter::sendSetHead(int16_t headingDeg) { MSPManager::getSingleton()->sendSetHead(headingDeg); }
bool MSPFollowAdapter::isHeadingHoldActive() { return MSPManager::getSingleton()->isHeadingHoldActive(); }
void MSPFollowAdapter::sendGvar(uint8_t index, int32_t value) { MSPManager::getSingleton()->sendGvar(index, value); }

double GNSSFollowAdapter::horizontalDistanceTo(GNSSLocation b) { return GNSSManager::getSingleton()->horizontalDistanceTo(b); }
int16_t GNSSFollowAdapter::courseTo(GNSSLocation b) { return GNSSManager::getSingleton()->courseTo(b); }

const peer_t *PeerFollowAdapter::getPeerById(uint8_t id) { return PeerManager::getSingleton()->getPeerById(id); }
const peer_t *PeerFollowAdapter::getPeer(uint8_t index) { return PeerManager::getSingleton()->getPeer(index); }

FollowManager *FollowManager::getSingleton()
{
    static FollowManager *followManager = nullptr;
    if (followManager == nullptr)
    {
        static MSPFollowAdapter mspAdapter;
        static GNSSFollowAdapter gnssAdapter;
        static PeerFollowAdapter peerAdapter;
        followManager = new FollowManager(&mspAdapter, &gnssAdapter, &peerAdapter);
        followManager->loadFromEEPROM();
    }
    return followManager;
}
