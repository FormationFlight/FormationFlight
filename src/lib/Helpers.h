#ifndef HELPERS_H
#define HELPERS_H

#include <Arduino.h>
#include "main.h"

#define M_PI 3.14159265358979323846
void pick_id();
void resync_tx_slot(int16_t delay);
uint8_t crc8_dvb_s2(uint8_t crc, unsigned char a);
String generate_id();
#endif