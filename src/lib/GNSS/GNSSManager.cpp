#include "GNSSManager.h"

GNSSManager::GNSSManager()
{
    currentProvider = -1;

}

GNSSManager *gnssManager = nullptr;

GNSSManager* GNSSManager::getSingleton()
{
    if (gnssManager == nullptr)
    {
        gnssManager = new GNSSManager();
    }
    return gnssManager;
}

// Returns the system's understanding of its current location
GNSSLocation GNSSManager::getLocation()
{
    static uint32_t lastUpdate = 0;
    static GNSSLocation loc;
    if (spoofLocationEnabled) {
        lastUpdate = millis();
        return spoofedLocation;
    }
    if (millis() - lastUpdate > GNSS_FRESH_INTERVAL_MS) {
        // Fetch the first provider that has a good fix
        for (uint8_t i = 0; i < GNSS_MAX_PROVIDERS; i++) {
            if (providers[i] != nullptr) {
                GNSSLocation providerLoc = providers[i]->getLocation();
                if (providerLoc.fixType != 0) {
                    loc = providerLoc;
                    currentProvider = i;
                    lastUpdate = millis();
                    break;
                }
            }
        }
    }
    return loc;
}

void GNSSManager::addProvider(GNSSProvider *provider)
{
    for (uint8_t i = 0; i < GNSS_MAX_PROVIDERS; i++) {
        if (providers[i] == nullptr) {
            providers[i] = provider;
            return;
        }
    }
}

void GNSSManager::addListener(GNSSListener *listener)
{
       for (uint8_t i = 0; i < GNSS_MAX_LISTENERS; i++) {
        if (listeners[i] == nullptr) {
            listeners[i] = listener;
            return;
        }
    } 
}

void GNSSManager::loop()
{
    for (uint8_t i = 0; i < GNSS_MAX_PROVIDERS; i++) {
        if (providers[i] == nullptr) {
            continue;
        }
        providers[i]->loop();
    }
    for (uint8_t i = 0; i < GNSS_MAX_LISTENERS; i++) {
        if (listeners[i] == nullptr) {
            continue;
        }
        listeners[i]->update(this->getLocation());
    }
}

void GNSSManager::statusJson(JsonDocument *doc)
{
    (*doc)["activeProvider"] = getCurrentProviderNameShort();
    GNSSLocation loc = getLocation();
    (*doc)["lat"] = loc.lat;
    (*doc)["lon"] = loc.lon;
    (*doc)["alt"] = loc.alt;
    (*doc)["groundSpeed"] = loc.groundSpeed;
    (*doc)["groundCourse"] = loc.groundCourse;
    (*doc)["numSat"] = loc.numSat;
    (*doc)["fixType"] = loc.fixType;
#ifdef GNSS_INJECT
    (*doc)["injectingGNSS"] = true;
    if (spoofLocationEnabled) {
        (*doc)["gnssSpoof"] = true;
    }
#endif

    JsonArray providersArray = doc->createNestedArray("providers");
    for (uint8_t i = 0; i < GNSS_MAX_PROVIDERS; i++)
    {
        if (providers[i] != nullptr) {
            GNSSProvider *provider = providers[i];
            JsonObject o = providersArray.createNestedObject();
            o["status"] = provider->getStatusString();
            o["enabled"] = provider->getEnabled();
        }
    }
}

void GNSSManager::setProviderStatus(uint8_t index, bool status)
{
    if (providers[index] == nullptr) {
        return;
    }
    providers[index]->setEnabled(status);
}

String GNSSManager::getCurrentProviderNameShort()
{
    if(currentProvider >= 0 && currentProvider < GNSS_MAX_PROVIDERS && providers[currentProvider] != nullptr)
        return providers[currentProvider]->getName();

    return "---";
}

double deg2rad(double deg)
{
    return (deg * M_PI / 180);
}

double distanceMeters(double lat1, double lon1, double lat2, double lon2) 
{
    double lat1r, lon1r, lat2r, lon2r, u, v;
    lat1r = deg2rad(lat1);
    lon1r = deg2rad(lon1);
    lat2r = deg2rad(lat2);
    lon2r = deg2rad(lon2);
    u = sin((lat2r - lat1r) / 2);
    v = sin((lon2r - lon1r) / 2);
    return 2.0 * 6371000 * asin(sqrt(u * u + cos(lat1r) * cos(lat2r) * v * v));
}

double courseDegrees(double lat1, double lon1, double lat2, double lon2)
{
    double dlon = radians(lon2 - lon1);
    lat1 = radians(lat1);
    lat2 = radians(lat2);
    double a1 = sin(dlon) * cos(lat2);
    double a2 = sin(lat1) * cos(lat2) * cos(dlon);
    a2 = cos(lat1) * sin(lat2) - a2;
    a2 = atan2(a1, a2);
    if (a2 < 0.0)
    {
        a2 += TWO_PI;
    }
    return degrees(a2);  
}

double GNSSManager::horizontalDistanceTo(GNSSLocation b)
{
    GNSSLocation loc = this->getLocation();
    return distanceMeters(loc.lat, loc.lon, b.lat, b.lon);
}

int16_t GNSSManager::courseTo(GNSSLocation b)
{
    GNSSLocation loc = this->getLocation();
    return courseDegrees(loc.lat, loc.lon, b.lat, b.lon);
}

// Returns the calculated point at a distance and bearing from the given coordinates
GNSSLocation GNSSManager::calculatePointAtDistance(GNSSLocation loc, double distanceMeters, double bearing) {
    double lat1 = radians(loc.lat); // Convert latitude to radians
    double lon1 = radians(loc.lon); // Convert longitude to radians

    double angularDistance = distanceMeters / 6371000; // Calculate the angular distance
    double bearingRadians = deg2rad(bearing); // Convert bearing to radians

    double lat2 = asin(sin(lat1) * cos(angularDistance) +
                       cos(lat1) * sin(angularDistance) * cos(bearingRadians));
    double lon2 = lon1 + atan2(sin(bearingRadians) * sin(angularDistance) * cos(lat1),
                               cos(angularDistance) - sin(lat1) * sin(lat2));

    // Convert back to degrees
    lat2 = degrees(lat2);
    lon2 = degrees(lon2);

    GNSSLocation point;
    point.lat = lat2;
    point.lon = lon2;

    return point;
}

// Generates a single point focused around loc offset by a subset of the bearing circle
// for example if count = 3, n=0 will be at bearing 0°, n=1 will be at bearing 120°, n=2 will be at bearing 240°
GNSSLocation GNSSManager::generatePointAround(GNSSLocation loc, int n, int count, double distance) {
    double bearing = (360.0 / count) * n; // Calculate the bearing between each point
    return calculatePointAtDistance(loc, distance, bearing);
}