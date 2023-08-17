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
    if (!ready || !hostIsFlightController(this->getFCVariant()))
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
    if (sys.phase > MODE_HOST_SCAN)
    {
        cached = true;
    }
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
    static msp_analog_t analog;
    static unsigned long cached = 0;

    if (!hostIsFlightController(this->getFCVariant()))
    {
        memset(&analog, 0, sizeof(analog));
        return analog;
    }

    if (millis() - cached < 1000)
    {
        return analog;
    }

    if (!msp->request(MSP_ANALOG, &analog, sizeof(analog)))
    {
        memset(&analog, 0, sizeof(analog));
        return analog;
    }
    cached = millis();
    return analog;
}

// Sends a MSP request for the GPS position of the FC; will be all-zero if the request failed
msp_raw_gps_t MSPManager::getLocation()
{
    msp_raw_gps_t gps;
    if (!hostIsFlightController(this->getFCVariant()))
    {
        memset(&gps, 0, sizeof(gps));
        return gps;
    }
    if (!msp->request(MSP_RAW_GPS, &gps, sizeof(gps)))
    {
        // Force the response to 0
        memset(&gps, 0, sizeof(gps));
    }
    return gps;
}
uint8_t MSPManager::mapFixType2Msp(GNSS_FIX_TYPE fixType)
{
    switch (fixType) {
        case GNSS_FIX_TYPE_2D:
            return 2;
        case GNSS_FIX_TYPE_3D:
            return 3;
        default:
            return 0;
    }
}

void MSPManager::sendLocation(GNSSLocation loc)
{
    static msp_sensor_gps_t gps2 = {};
    gps2.gpsWeek = 0xFFFF; // if it is not coming from gps, 0xffff means not supported
    gps2.fixType = this->mapFixType2Msp(loc.fixType);
    gps2.mslAltitude = loc.alt; // cm
    gps2.groundCourse = loc.groundCourse * 10;
    gps2.hdop = loc.hdop * 100;

    gps2.latitude = loc.lat * 10000000;
    gps2.longitude = loc.lon * 10000000;
    gps2.satellitesInView = loc.numSat;

    gps2.instance = 0;

    // TODO: The following data should come from the actual GPS unit

    // TODO: These are velocity vectors. Update gnss location to include 3d speed?
    // 2d speed modulus can be computed by using nedVelEast nedVelNorth
    // cm/s
    // if gnss location is 2d, velNorth = vel * cos(radians(groundCourse * 10))
    // and velEast = vel * sin(radians(groundCourse * 10))

    gps2.nedVelNorth = loc.groundSpeed * cos(radians(loc.groundCourse * 10));
    gps2.nedVelEast = loc.groundSpeed * sin(radians(loc.groundCourse * 10));
    gps2.nedVelDown = 0;

    gps2.horizontalPosAccuracy = 0;
    gps2.verticalPosAccuracy = 0;

    unsigned long m = millis();
    gps2.year = 1970;
    gps2.day = 1;
    gps2.hour = 0;
    gps2.month = 1;
    gps2.hour = (m / (60 * 60 * 1000)) % 24;
    gps2.min = (m / (60 * 1000)) % 60;
    gps2.sec = (m / 1000) % 60;

    gps2.horizontalPosAccuracy = 1;
    gps2.verticalPosAccuracy = 1;

    gps2.trueYaw = 0xFFFF; // 0xFFFF should mean unsupported.
    // The TOW count is a value ranging from 0 to 403,199 whose meaning is the number of 1.5 second periods elapsed since the beginning of the GPS week. 
    gps2.msTOW = (uint32_t)((m / 1500.0f) + 0.5) % 403199;

    msp->command2(MSP2_SENSOR_GPS, &gps2, sizeof(gps2), 0);

    gnssUpdatesSent++;
}

// Sends a particular peer
void MSPManager::sendRadar(const peer_t *peer)
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

        const peer_t *peer = PeerManager::getSingleton()->getPeer(peerIndex);
        // Only send if the peer has been seen and it's not us
        if (peer->id > 0 && peerIndex + 1 != curr.id)
        {
            if (!DEBUG)
            {
                MSPManager::getSingleton()->sendRadar(peer);
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
    (*doc)["peerUpdatesSent"] = this->peerUpdatesSent;
    (*doc)["gnssUpdatesSent"] = this->gnssUpdatesSent;
    (*doc)["vbat"] = analog.vbat * 0.1;
    (*doc)["mahDrawn"] = analog.mAhDrawn;
    (*doc)["amps"] = analog.amperage * 0.01;
}