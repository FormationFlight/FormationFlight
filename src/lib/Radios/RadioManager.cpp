#include <Arduino.h>
#include "RadioManager.h"
#include "main.h"
#include "../Helpers.h"
#ifdef HAS_LORA
#include <RadioLib.h>
#endif

RadioManager::RadioManager()
{
}

RadioManager* radioManager;

RadioManager* RadioManager::getSingleton()
{
    if (radioManager == NULL)
    {
        radioManager = new RadioManager();
    }
    return radioManager;
}

air_type0_t RadioManager::prepare_packet()
{
    air_type0_t air_0;
    air_0.id = curr.id;
    air_0.lat = curr.gps.lat / 100;
    air_0.lon = curr.gps.lon / 100;
    air_0.alt = curr.gps.alt; // m
    air_0.extra_type = sys.ota_nonce % 5;

    switch (air_0.extra_type)
    {
    case 0:
        air_0.extra_value = curr.gps.groundCourse / 10;
        break;

    case 1:
        air_0.extra_value = curr.gps.groundSpeed / 20;
        break;

    case 2:
        air_0.extra_value = curr.name[0];
        break;

    case 3:
        air_0.extra_value = curr.name[1];
        break;

    case 4:
        air_0.extra_value = curr.name[2];
        break;

    default:
        break;
    }
    // calculate crc
    uint8_t buf[sizeof(air_type0_t)];
    memcpy_P(buf, &air_0, sizeof(air_type0_t));
    uint8_t calculatedCrc = 0;
    for (uint8_t i = 0; i < sizeof(air_type0_t) - sizeof(air_0.crc); i++)
    {
        calculatedCrc = crc8_dvb_s2(calculatedCrc, buf[i]);
    }
    air_0.crc = calculatedCrc;
    return air_0;
}


void RadioManager::receive(const uint8_t *rawPacket, size_t packetSize)
{
    // Check packet size
    if (packetSize != sizeof(air_type0_t))
    {
        return;
    }
    air_type0_t air_0;
    memcpy_P(&air_0, rawPacket, packetSize);
    uint8_t calculatedCrc = 0;
    // Check CRC
    for (uint8_t i = 0; i < sizeof(air_type0_t) - sizeof(air_0.crc); i++)
    {
        calculatedCrc = crc8_dvb_s2(calculatedCrc, rawPacket[i]);
    }
    if (calculatedCrc != air_0.crc)
    {
        return;
    }

    uint8_t id = air_0.id - 1;
    if (id >= LORA_NODES_MAX)
    {
        return;
    }

    // Update previous GPS location for extrapolation
    peers[id].gps_pre.lat = peers[id].gps.lat;
    peers[id].gps_pre.lon = peers[id].gps.lon;
    peers[id].gps_pre.alt = peers[id].gps.alt;
    peers[id].gps_pre.groundCourse = peers[id].gps.groundCourse;
    peers[id].gps_pre.groundSpeed = peers[id].gps.groundSpeed;
    peers[id].gps_pre_updated = peers[id].updated;

    sys.air_last_received_id = air_0.id;
    peers[id].id = air_0.id;
    peers[id].lq_tick++;
    peers[id].state = 0;
    peers[id].lost = 0;
    peers[id].updated = millis();
    peers[id].rssi = 0;

    peers[id].gps.lat = air_0.lat * 100;
    peers[id].gps.lon = air_0.lon * 100;
    peers[id].gps.alt = air_0.alt; // m

    switch (air_0.extra_type)
    {
    case 0:
        peers[id].gps.groundCourse = air_0.extra_value * 10;
        break;

    case 1:
        peers[id].gps.groundSpeed = air_0.extra_value * 20;
        break;

    case 2:
        peers[id].name[0] = air_0.extra_value;
        break;

    case 3:
        peers[id].name[1] = air_0.extra_value;
        break;

    case 4:
        peers[id].name[2] = air_0.extra_value;
        peers[id].name[3] = 0;
        break;

    default:
        break;
    }

    if (peers[id].gps.lat != 0 && peers[id].gps.lon != 0)
    {
        // Restore the last known coordinates
        peers[id].gps_rec.lat = peers[id].gps.lat;
        peers[id].gps_rec.lon = peers[id].gps.lon;
        peers[id].gps_rec.alt = peers[id].gps.alt;
        peers[id].gps_rec.groundCourse = peers[id].gps.groundCourse;
        peers[id].gps_rec.groundSpeed = peers[id].gps.groundSpeed;
    }

    sys.num_peers = count_peers(0, &cfg);

    if ((sys.air_last_received_id == curr.id) && (sys.phase > MODE_OTA_SYNC) && !sys.lora_no_tx)
    {
        // Slot conflict
        uint32_t cs1 = peers[id].name[0] + peers[id].name[1] * 26 + peers[id].name[2] * 26 * 26;
        uint32_t cs2 = curr.name[0] + curr.name[1] * 26 + curr.name[2] * 26 * 26 + 1;
        if (cs1 < cs2)
        { // Pick another slot
            sprintf(sys.message, "%s", "ID CONFLICT");
            pick_id();
            resync_tx_slot(cfg.lora_timing_delay);
        }
    }
}

void RadioManager::transmit(air_type0_t *packet)
{
    for (uint8_t i = 0; i < MAX_RADIOS; i++) {
        if (radios[i] != nullptr) {
            radios[i]->transmit(packet);
        }
    }
}

void RadioManager::addRadio(Radio *radio)
{
    for (uint8_t i = 0; i < MAX_RADIOS; i++) {
        if (radios[i] == nullptr) {
            radios[i] = radio;
            radios[i]->begin();
            return;
        }
    }
}

void RadioManager::loop()
{
    for (uint8_t i = 0; i < MAX_RADIOS; i++) {
        if (radios[i] == nullptr) {
            continue;
        }
        radios[i]->loop();
    }
}