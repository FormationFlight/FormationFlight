#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#define GNSS_MAX_PROVIDERS 2
#define GNSS_MAX_LISTENERS 2
#define GNSS_FRESH_INTERVAL_MS 100

enum GNSS_FIX_TYPE {
    GNSS_FIX_TYPE_NONE = 0,
    GNSS_FIX_TYPE_2D = 1,
    GNSS_FIX_TYPE_3D = 2
};

struct GNSSLocation {
  GNSS_FIX_TYPE  fixType;
  uint8_t  numSat;
  double  lat;
  double  lon;
  double  alt;
  double  groundSpeed;
  double  groundCourse;
  double hdop;
  unsigned long lastUpdate;
};

// A system that can provide us our location, like an FC with GPS or an on-board GPS module (TTGO T-Beam, etc.)
class GNSSProvider {
public:
    virtual GNSSLocation getLocation();
    virtual void loop();
    virtual String getStatusString();
    virtual String getName();
    bool getEnabled() {
        return enabled;
    }
    void setEnabled(bool status) {
        enabled = status;
    }
private:
    bool enabled = true;
};

// A system which will want to receive our location, like an FC without GPS or a GCS over a network connection
class GNSSListener {
public:
    virtual void update(GNSSLocation location);

};

class GNSSManager {
public:
    GNSSManager();
    static GNSSManager* getSingleton();
    void addProvider(GNSSProvider *provider);
    void setProviderStatus(uint8_t index, bool status);
    void addListener(GNSSListener *listener);
    GNSSLocation getLocation();
    // Return the 3-character name of the current GPS provider
    String getCurrentProviderNameShort();
    void loop();
    void statusJson(JsonDocument *doc);
    double horizontalDistanceTo(GNSSLocation b);
    int16_t courseTo(GNSSLocation b);
    static GNSSLocation calculatePointAtDistance(GNSSLocation loc, double distanceMeters, double bearing);
    static GNSSLocation generatePointAround(GNSSLocation loc, int n, int count, double distance);
    GNSSLocation spoofedLocation;
    bool spoofLocationEnabled = false;
private:
    GNSSProvider* providers[GNSS_MAX_PROVIDERS] = {nullptr};
    uint8_t currentProvider = 0;
    GNSSListener* listeners[GNSS_MAX_LISTENERS] = {nullptr};
};