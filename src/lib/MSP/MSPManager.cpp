#include "MSPManager.h"
#include "../Statistics/StatsManager.h"
#include "main.h"

MSPManager::MSPManager()
{
    msp = new MSP();
}

MSPManager *mspManager = nullptr;

MSPManager *MSPManager::getSingleton()
{
    if (mspManager == nullptr)
    {
        mspManager = new MSPManager();
    }
    return mspManager;
}

// Initializes MSPManager
void MSPManager::begin(Stream &stream)
{
    msp->begin(stream);
    ready = true;
}

// Returns the flight controller's state - 0 for disarmed, 1 for armed.
uint8_t MSPManager::getState()
{
    if (!ready)
    {
        return 0;
    }
    uint32_t modes;
    msp->getActiveModes(&modes);
    return bitRead(modes, 0);
}

// Requests the name of the flight controller over MSP without caching
void MSPManager::getName(char *name, size_t length)
{
    msp->request(MSP_NAME, name, length);
}

// Returns the MSPHost variant of the flight controller; cached once we have a valid response,
// or if the host scan period timeout has elapsed
MSPHost MSPManager::getFCVariant()
{
    static char variant[5] = "";
    static bool cached = false;
    if (!cached)
    {
        msp->request(MSP_FC_VARIANT, variant, sizeof(variant));
    }
    if (strncmp(variant, "INAV", 4) == 0)
    {
        cached = true;
        return HOST_INAV;
    }
    else if (strncmp(variant, "GCS", 3) == 0)
    {
        cached = true;
        return HOST_GCS;
    }
    else if (strncmp(variant, "ARDU", 4) == 0)
    {
        cached = true;
        return HOST_ARDU;
    }
    else if (strncmp(variant, "BTFL", 4) == 0)
    {
        cached = true;
        return HOST_BTFL;
    }
    if (sys.phase > MODE_HOST_SCAN)
    {
        cached = true;
    }
    return HOST_NONE;
}

// Return whether the host provided is a flight controller, ergo understands GPS & analog values
bool MSPManager::hostIsFlightController(MSPHost host)
{
    return (host == HOST_INAV || host == HOST_ARDU || host == HOST_BTFL);
}

// Returns the FC's version, cached after the first positive response we get
msp_fc_version_t MSPManager::getFCVersion()
{
    static msp_fc_version_t version;
    static bool cached = false;
    if (cached)
    {
        return version;
    }
    cached = msp->request(MSP_FC_VERSION, &version, sizeof(version));
    return version;
}

// Sends a MSP request for the analog values of the FC; will be all-zero if the request failed
msp_analog_t MSPManager::getAnalogValues()
{
    msp_analog_t analog;
    if (!msp->request(MSP_ANALOG, &analog, sizeof(analog)))
    {
        memset(&analog, 0, sizeof(analog));
    }
    return analog;
}

// Sends a MSP request for the GPS position of the FC; will be all-zero if the request failed
msp_raw_gps_t MSPManager::getLocation()
{
    msp_raw_gps_t gps;
    if (!msp->request(MSP_RAW_GPS, &gps, sizeof(gps)))
    {
        // Force the response to 0
        memset(&gps, 0, sizeof(gps));
    }
    return gps;
}

void MSPManager::sendLocation(GNSSLocation loc)
{
    msp_raw_gps_t gps;
    gps.alt = loc.alt;
    gps.fixType = loc.fixType;
    gps.groundCourse = loc.groundCourse * 10;
    const double kmh_to_cms = 27.77;
    gps.groundSpeed = loc.groundSpeed * kmh_to_cms;
    gps.hdop = loc.hdop;
    gps.lat = loc.lat * (1 / 10000000);
    gps.lon = loc.lon * (1 / 10000000);
    gps.numSat = loc.numSat;
    msp->command(MSP_SET_RAW_GPS, &gps, sizeof(gps), 0);
    gnssUpdatesSent++;
}

// Sends a particular peer
void MSPManager::sendRadar(peer_t *peer)
{
    msp_radar_pos_t position;
    position.id = peer->id;
    position.state = (peer->lost == 2) ? 2 : peer->state;
    position.lat = peer->gps.lat * 10;              // x 10E7
    position.lon = peer->gps.lon * 10;              // x 10E7
    position.alt = peer->gps.alt * 100;             // cm
    position.heading = peer->gps.groundCourse / 10; // From ° x 10 to °
    position.speed = peer->gps.groundSpeed;         // cm/s
    position.lq = peer->lq;
    msp->command2(MSP2_COMMON_SET_RADAR_POS, &position, sizeof(position), 0);
    peerUpdatesSent++;
}

// Enables or disables spoofing fake peers
void MSPManager::enableSpoofing(bool enabled)
{
    this->spoofingPeers = enabled;
}

// Schedules the next transmission loop at the given timestamp
void MSPManager::scheduleNextAt(unsigned long timestamp)
{
    nextSendTime = timestamp;
    peerIndex = 0;
}

void MSPManager::loop()
{
    if (sys.phase > MODE_OTA_SYNC && millis() >= nextSendTime)
    {
        if (hostIsFlightController(getFCVariant()))
        {
            // We used to get state & analog values here; necessary?
        }

        // Send MSP radar positions to the FC
        StatsManager::getSingleton()->startTimer();
        if (this->spoofingPeers)
        {
            GNSSLocation spoofOrigin = GNSSManager::getSingleton()->getLocation();
            if (spoofOrigin.fixType == GNSS_FIX_TYPE_NONE) {
                // Pick an arbitrary point to spoof peers at if we don't know where we are
                // 45.171546, 5.722387 is Grenoble, France where OlivierC-FR comes from as an homage to his project iNav Radar
                spoofOrigin.lat = 45.171546;
                spoofOrigin.lon = 5.722387;
            }
            uint8_t id = peerIndex + 1;
            // Generate peers in 100m offsets away in a circle around the user 
            GNSSLocation peerLocation = GNSSManager::generatePointAround(spoofOrigin, peerIndex, cfg.lora_nodes, 100 * (peerIndex + 1));
            peer_t peer{
                .id = id,
                .state = 1,
                .lost = 0,
                .updated = millis(),
                .lq = 4,
                .gps = {
                    .lat = (int)(peerLocation.lat * 1000000),
                    .lon = (int)(peerLocation.lon * 1000000),
                    .alt = 100,
                    .groundSpeed = 0,
                    .groundCourse = 0,
                },
                .name = "FAK",
            };
            if (peer.id > 0 && peerIndex + 1 != curr.id)
            {
                MSPManager::getSingleton()->sendRadar(&peer);
            }
        }
        else
        {
            peer_t *peer = PeerManager::getSingleton()->getPeer(peerIndex);
            // Only send if the peer has been seen and it's not us
            if (peer->id > 0 && peerIndex + 1 != curr.id)
            {
                if (!DEBUG)
                {
                    MSPManager::getSingleton()->sendRadar(peer);
                }
            }
        }
        StatsManager::getSingleton()->storeTimerAndRestart(STATS_KEY_MSP_SENDTIME_US);
        // Move to the next peer
        if (peerIndex < cfg.lora_nodes - 1) {
            peerIndex++;
            // Schedule a new transmission after the current one
            nextSendTime = nextSendTime + cfg.slot_spacing;
        } else {
            // Avoid running the last slot twice; push our next send out 1 full cycle
            // this will be reduced by the next TX event
            nextSendTime = cfg.slot_spacing * cfg.lora_nodes;
        }
    }
}

void MSPManager::statusJson(JsonDocument *doc)
{
    msp_analog_t analog = getAnalogValues();
    (*doc)["spoofingPeers"] = this->spoofingPeers;
    (*doc)["peerUpdatesSent"] = this->peerUpdatesSent;
    (*doc)["gnssUpdatesSent"] = this->gnssUpdatesSent;
    (*doc)["vbat"] = analog.vbat * 0.1;
    (*doc)["mahDrawn"] = analog.mAhDrawn;
    (*doc)["amps"] = analog.amperage * 0.01;
}