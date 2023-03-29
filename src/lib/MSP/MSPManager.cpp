#include "MSPManager.h"

MSPManager::MSPManager()
{
    msp = new MSP();
}

MSPManager *mspManager = nullptr;

MSPManager* MSPManager::getSingleton()
{
    if (mspManager == nullptr)
    {
        mspManager = new MSPManager();
    }
    return mspManager;
}

void MSPManager::begin(Stream &stream)
{
    msp->begin(stream);
    ready = true;
}

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

void MSPManager::getName(char *name, size_t length)
{
    msp->request(MSP_NAME, name, length);
}

MSPHost MSPManager::getFCVariant()
{
    char variant[5];
    msp->request(MSP_FC_VARIANT, variant, sizeof(variant));
    
    if (strncmp(variant, "INAV", 4) == 0)
    {
        return HOST_INAV;
    }
    else if (strncmp(variant, "GCS", 3) == 0)
    {
        return HOST_GCS;
    }
    else if (strncmp(variant, "ARDU", 4) == 0)
    {
        return HOST_ARDU;
    }
    else if (strncmp(variant, "BTFL", 4) == 0)
    {
        return HOST_BTFL;
    }
    return HOST_NONE;
}

bool MSPManager::hostTXCapable(MSPHost host)
{
    return (host == HOST_INAV || host == HOST_ARDU || host == HOST_BTFL);
}

msp_fc_version_t MSPManager::getFCVersion()
{
    static msp_fc_version_t version;
    static bool cached = false;
    if (cached) {
        return version;
    }
    cached = msp->request(MSP_FC_VERSION, &version, sizeof(version));
    return version;
}

msp_analog_t MSPManager::getAnalogValues()
{
    msp_analog_t analog;
    msp->request(MSP_ANALOG, &analog, sizeof(analog));
    return analog;
}

msp_raw_gps_t MSPManager::getLocation()
{
    msp_raw_gps_t gps;
    msp->request(MSP_RAW_GPS, &gps, sizeof(gps));
    return gps;
}
/*
void MSPManager::sendLocation()
{
}
*/
void MSPManager::sendRadar(peer_t *peer)
{
    msp_radar_pos_t position;
    position.id = peer->id;
    position.state = (peer->lost == 2) ? 2 : peer->state;
    position.lat = peer->gps_comp.lat;              // x 10E7
    position.lon = peer->gps_comp.lon;              // x 10E7
    position.alt = peer->gps_comp.alt * 100;        // cm
    position.heading = peer->gps.groundCourse / 10; // From ° x 10 to °
    position.speed = peer->gps.groundSpeed;         // cm/s
    position.lq = peer->lq;
    msp->command2(MSP2_COMMON_SET_RADAR_POS, &position, sizeof(position), 0);
}