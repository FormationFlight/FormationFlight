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

// Returns the FC's active-mode bitmap (MSP_STATUS + MSP_BOXIDS), cached briefly
// so getState()/isGCSNavActive() calls within the same cycle share one poll
// instead of each triggering their own pair of MSP round trips.
uint32_t MSPManager::getActiveModesCached()
{
    static uint32_t modes = 0;
    static unsigned long cached = 0;

    if (millis() - cached < 100)
    {
        return modes;
    }

    msp->getActiveModes(&modes);
    cached = millis();
    return modes;
}

// Returns the flight controller's state - 0 for disarmed, 1 for armed.
uint8_t MSPManager::getState()
{
    if (!ready || !hostIsFlightController(this->getFCVariant()))
    {
        return 0;
    }
    return bitRead(getActiveModesCached(), 0);
}

// Returns whether GCS NAV is currently active on the FC (follow-mode trigger, §5[C] option 2).
bool MSPManager::isGCSNavActive()
{
    if (!ready || !hostIsFlightController(this->getFCVariant()))
    {
        return false;
    }
    return bitRead(getActiveModesCached(), MSP_MODE_GCSNAV);
}

// Returns whether INAV's HEADING HOLD ("MAG") box is active on the FC — see
// sendSetHead()'s comment (MSPManager.h) for why this gates whether a
// commanded heading actually reaches the yaw-rate PID while in NAV POSHOLD_3D.
bool MSPManager::isHeadingHoldActive()
{
    if (!ready || !hostIsFlightController(this->getFCVariant()))
    {
        return false;
    }
    return bitRead(getActiveModesCached(), MSP_MODE_MAG);
}

// Requests the name of the flight controller over MSP without caching
void MSPManager::getName(char *name, size_t length)
{
    if (!msp->request(MSP_NAME, name, length))
    {
        memset(name, 0, length);
    }
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
        if (!msp->request(MSP_FC_VARIANT, variant, sizeof(variant)))
        {
            memset(&variant, 0, sizeof(variant));
        }
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

// Returns the connected FC's mixer platform type (MSP2_INAV_MIXER), cached
// once we have a valid response. Unlike getFCVariant(), this has no
// sys.phase-based give-up: its only caller (FollowManager::loop()) never
// runs until sys.phase > MODE_OTA_SYNC, so a "stop trying past MODE_HOST_SCAN"
// bypass would fire on the very first call, before a request is ever sent,
// and platformType would stay stuck at INAV_PLATFORM_UNKNOWN forever.
// Returns INAV_PLATFORM_UNKNOWN until a reply actually lands, so "never
// answered" is distinguishable from a real INAV_PLATFORM_MULTIROTOR (0) reply.
InavPlatformType MSPManager::getPlatformType()
{
    static msp_mixer_config_t mixer{.platformType = INAV_PLATFORM_UNKNOWN};
    static bool cached = false;
    if (!cached && ready && getFCVariant() == HOST_INAV)
    {
        if (msp->request2(MSP2_INAV_MIXER, &mixer, sizeof(mixer)))
        {
            cached = true;
        }
    }
    return (InavPlatformType)mixer.platformType; // INAV_PLATFORM_UNKNOWN if never populated
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

// Returns the FC's home/baro-relative altitude estimate in centimeters (MSP_ALTITUDE), cached
// briefly since the follow module polls this every cycle at FOLLOW_EMIT_HZ.
int32_t MSPManager::local_altitude_cm()
{
    static msp_altitude_t altitude = {};
    static unsigned long cached = 0;

    if (!hostIsFlightController(this->getFCVariant()))
    {
        memset(&altitude, 0, sizeof(altitude));
        return 0;
    }

    if (millis() - cached < 100)
    {
        return altitude.estimatedActualPosition;
    }

    if (!msp->request(MSP_ALTITUDE, &altitude, sizeof(altitude)))
    {
        memset(&altitude, 0, sizeof(altitude));
        return 0;
    }
    cached = millis();
    return altitude.estimatedActualPosition;
}

// Cached MSP_RC poll (~100ms, matching local_altitude_cm()'s cadence) so
// FollowManager::loop() at FOLLOW_EMIT_HZ always sees a fresh-enough read.
// Deliberately does NOT memset-on-failure the way
// local_altitude_cm()/getAnalogValues() do — a dropped MSP_RC frame must
// not read as "channel near zero," so a failed request just leaves the
// last successfully parsed struct in place. Polling only ever happens
// because a caller asked for a specific channel, so a pilot with no axis
// RC-assigned costs zero extra MSP traffic without this function needing
// its own enable flag.
bool MSPManager::getRcChannelUs(uint8_t channel1Based, uint16_t *outUs)
{
    static msp_rc_t rc = {};
    static unsigned long cached = 0;

    if (channel1Based < 1 || channel1Based > MSP_MAX_SUPPORTED_CHANNELS)
    {
        return false;
    }
    if (!hostIsFlightController(this->getFCVariant()))
    {
        return false;
    }

    if (millis() - cached >= 100)
    {
        if (msp->request(MSP_RC, &rc, sizeof(rc)))
        {
            cached = millis();
        }
        // Poll miss: `rc` is left exactly as it was (MSP::recv() only
        // touches the buffer on a checksum-valid response, MSP.cpp:104-144).
    }

    *outUs = rc.channelValue[channel1Based - 1];
    return true;
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

// MSP_SET_WP (#209) - INAV follow-me special waypoint #255.
// Requires NAV POSHOLD + GCS NAV active on the follower FC.
// p1 doubles as heading for this special waypoint only - for
// ordinary mission waypoints (1-60) it means cruise speed instead; the two
// are not the same field just because they share a byte offset.
// NOTE: as of INAV 9.x, the heading is currently inert for a follower in NAV
// POSHOLD_3D — NAV_STATE_POSHOLD_3D_IN_PROGRESS lacks NAV_REQUIRE_MAGHOLD, so
// INAV's yaw-rate PID never actually reads the desiredState.yaw this sets
// (see sendSetHead()'s comment for the real, currently-working path and the
// full firmware-source trail). Sent anyway rather than hardcoded to 0: it's
// free (same message, no extra MSP traffic), and if INAV ever extends
// POSHOLD_3D to honor it, FF already sends the right value with no code
// change needed on our side.
void MSPManager::sendFollowWaypoint(int32_t lat_1e7, int32_t lon_1e7, int32_t alt_cm, int16_t headingDeg)
{
    msp_set_wp_t wp{};
    wp.waypointNumber = 255;
    wp.action = MSP_NAV_STATUS_WAYPOINT_ACTION_WAYPOINT; // must be 1
    wp.lat = lat_1e7;
    wp.lon = lon_1e7;
    wp.alt = alt_cm;         // home-relative, p3 bit0 = 0 below
    wp.p1 = headingDeg; wp.p2 = 0; wp.p3 = 0;
    wp.flag = 0;
    msp->command(MSP_SET_WP, &wp, sizeof(wp));
}

// MSP_SET_HEAD (#211) - explicit heading-hold target.
// One-way, best-effort, no ACK wait, mirrors sendGvar()'s fire-and-forget
// style. Callers must gate on isHeadingHoldActive() themselves — INAV
// accepts this unconditionally but the yaw-rate PID only consumes the target
// it writes when the HEADING HOLD box is active (see MSPManager.h).
void MSPManager::sendSetHead(int16_t headingDeg)
{
    msp_set_head_t head{};
    head.magHoldHeading = headingDeg;
    msp->command(MSP_SET_HEAD, &head, sizeof(head));
}

void MSPManager::sendGvar(uint8_t index, int32_t value)
{
    if (!ready || getFCVariant() != HOST_INAV)
    {
        return;
    }
    // getFCVersion() is already cached for the connection's lifetime
    // (used today by Display.cpp's FC-version readout) — this adds no
    // extra MSP traffic to check support.
    if (getFCVersion().versionMajor < 9)
    {
        return;
    }

    msp_set_gvar_t g{};
    g.index = index;
    g.value = value;
    msp->command2(MSP2_INAV_SET_GVAR, &g, sizeof(g), 0); // fire-and-forget, mirrors sendRadar()
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