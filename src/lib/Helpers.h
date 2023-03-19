#ifndef HELPERS_H
#define HELPERS_H

#include <Arduino.h>
#include "main.h"

#define M_PI 3.14159265358979323846
double deg2rad(double deg);
double rad2deg(double rad);
double gpsDistanceBetween(double lat1d, double lon1d, double lat2d, double lon2d);
double gpsCourseTo(double lat1, double long1, double lat2, double long2);
int count_peers(bool active, config_t *cfg);
void reset_peers();
void pick_id();
void resync_tx_slot(int16_t delay);
uint8_t crc8_dvb_s2(uint8_t crc, unsigned char a);
#endif