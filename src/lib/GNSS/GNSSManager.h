#pragma once

#include <Arduino.h>

#define GNSS_MAX_PROVIDERS 2

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
};

class GNSSProvider {
public:
    GNSSLocation getLocation();
    void begin();
    void loop();
};

class GNSSManager {
public:
    GNSSManager();
    static GNSSManager* getSingleton();
    void addProvider(GNSSProvider *provider);
    GNSSLocation getLocation();
    void loop();
private:
    GNSSProvider* providers[GNSS_MAX_PROVIDERS];
};