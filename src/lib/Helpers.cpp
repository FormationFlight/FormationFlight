#include "Helpers.h"
#include <math.h>
#include <cmath>
#include <Arduino.h>
#include "main.h"


double deg2rad(double deg)
{
    return (deg * M_PI / 180);
}

double rad2deg(double rad)
{
    return (rad * 180 / M_PI);
}

double gpsDistanceBetween(double lat1d, double lon1d, double lat2d, double lon2d)
{
    double lat1r, lon1r, lat2r, lon2r, u, v;
    lat1r = deg2rad(lat1d);
    lon1r = deg2rad(lon1d);
    lat2r = deg2rad(lat2d);
    lon2r = deg2rad(lon2d);
    u = sin((lat2r - lat1r) / 2);
    v = sin((lon2r - lon1r) / 2);
    return 2.0 * 6371000 * asin(sqrt(u * u + cos(lat1r) * cos(lat2r) * v * v));
}

double gpsCourseTo(double lat1, double long1, double lat2, double long2)
{
    // returns course in degrees (North=0, West=270) from position 1 to position 2,
    // both specified as signed decimal-degrees latitude and longitude.
    double dlon = radians(long2 - long1);
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


int count_peers(bool active, config_t *cfg)
{
    int j = 0;
    for (int i = 0; i < cfg->lora_nodes; i++) {
        if (active == 1) {
            if ((peers[i].id > 0) && peers[i].lost == 0) {
                j++;
            }
        }
        else {
            if (peers[i].id > 0) {
                j++;
            }
        }
    }
    return j;
}

void reset_peers() {
    sys.now_sec = millis();
    for (int i = 0; i < cfg.lora_nodes; i++) {
        peers[i].id = 0;
        peers[i].host = 0;
        peers[i].state = 0;
        peers[i].lost = 0;
        peers[i].broadcast = 0;
        peers[i].lq_updated = sys.now_sec;
        peers[i].lq_tick = 0;
        peers[i].lq = 0;
        peers[i].updated = 0;
        peers[i].rssi = 0;
        peers[i].distance = 0;
        peers[i].direction = 0;
        peers[i].relalt = 0;
        strcpy(peers[i].name, "");
    }
}


void pick_id() {
    curr.id = 0;
    for (int i = 0; i < cfg.lora_nodes; i++) {
        if ((peers[i].id == 0) && (curr.id == 0)) {
            curr.id = i + 1;
        }
    }
}

void resync_tx_slot(int16_t delay) {
    bool startnow = 0;
    for (int i = 0; (i < cfg.lora_nodes) && (startnow == 0); i++) {
        if (peers[i].id > 0) {
            sys.lora_next_tx = peers[i].updated + (curr.id - peers[i].id) * cfg.lora_slot_spacing + sys.lora_cycle + delay;
            startnow = 1;
        }
    }
}

uint8_t crc8_dvb_s2(uint8_t crc, unsigned char a)
{
    crc ^= a;
    for (int ii = 0; ii < 8; ++ii) {
        if (crc & 0x80) {
            crc = (crc << 1) ^ 0xD5;
        } else {
            crc = crc << 1;
        }
    }
    return crc;
}