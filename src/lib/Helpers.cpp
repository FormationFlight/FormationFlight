#include "Helpers.h"
#include <math.h>
#include <cmath>
#include <Arduino.h>
#include "main.h"
#include "Peers/PeerManager.h"


void pick_id()
{
    curr.id = 0;
    for (int i = 0; i < cfg.lora_nodes; i++)
    {
        peer_t *peer = PeerManager::getSingleton()->getPeer(i);
        if ((peer->id == 0) && (curr.id == 0) && (!peer->lost))
        {
            curr.id = i + 1;
        }
    }
}

void resync_tx_slot(int16_t delay)
{
    bool startnow = 0;
    for (int i = 0; (i < cfg.lora_nodes) && (startnow == 0); i++)
    {
        peer_t *peer = PeerManager::getSingleton()->getPeer(i);

        if (peer->id > 0)
        {
            sys.next_tx = peer->updated + (curr.id - peer->id) * cfg.slot_spacing + sys.lora_cycle + delay;
            startnow = 1;
        }
    }
}

uint8_t crc8_dvb_s2(uint8_t crc, unsigned char a)
{
    crc ^= a;
    for (int ii = 0; ii < 8; ++ii)
    {
        if (crc & 0x80)
        {
            crc = (crc << 1) ^ 0xD5;
        }
        else
        {
            crc = crc << 1;
        }
    }
    return crc;
}

String generate_id()
{
    uint32_t chipID;
#ifdef PLATFORM_ESP8266
    chipID = ESP.getChipId() << 8;
#elif defined(PLATFORM_ESP32)
    uint32_t low = ESP.getEfuseMac() & 0xFFFFFFFF;
    uint32_t high = (ESP.getEfuseMac() >> 32) % 0xFFFFFFFF;
    chipID = (high << 8 | low >> 24) << 8;
#endif
    String chipIDString = String(__builtin_bswap32(chipID), HEX);
    chipIDString.toUpperCase();
    return chipIDString;
}