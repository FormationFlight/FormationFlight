#include <Arduino.h>
#include "RadioManager.h"
#include "main.h"
#include "../Helpers.h"
#ifdef HAS_LORA
#include <RadioLib.h>
#endif
#include "../CryptoManager.h"
#include <ArduinoJson.h>
#include "../Peers/PeerManager.h"

RadioManager::RadioManager()
{
}

RadioManager* radioManager = nullptr;

RadioManager* RadioManager::getSingleton()
{
    if (radioManager == nullptr)
    {
        radioManager = new RadioManager();
    }
    return radioManager;
}

air_type0_t RadioManager::prepare_packet()
{
    air_type0_t air_0;
    air_0.packet_type = PACKET_TYPE_RADAR_POSITION;
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


void RadioManager::receive(const uint8_t *rawPacket, size_t packetSize, float rssi)
{


    // Check packet size
    if (packetSize != sizeof(air_type0_t))
    {
        return;
    }
    air_type0_t air_0;
    memcpy_P(&air_0, rawPacket, packetSize);
    if (air_0.packet_type != PACKET_TYPE_RADAR_POSITION)
    {
        // We can implement additional packet types here
        return;
    }

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

    peer_t *peer = PeerManager::getSingleton()->getPeer(id - 1);

    // Update previous GPS location for extrapolation
    peer->gps_pre.lat = peer->gps.lat;
    peer->gps_pre.lon = peer->gps.lon;
    peer->gps_pre.alt = peer->gps.alt;
    peer->gps_pre.groundCourse = peer->gps.groundCourse;
    peer->gps_pre.groundSpeed = peer->gps.groundSpeed;
    peer->gps_pre_updated = peer->updated;

    sys.air_last_received_id = air_0.id;
    peer->id = air_0.id;
    peer->lq_tick++;
    peer->state = 0;
    peer->lost = 0;
    peer->updated = millis();
    if (rssi != 0) {
        peer->rssi = int(rssi);
    }

    peer->gps.lat = air_0.lat * 100;
    peer->gps.lon = air_0.lon * 100;
    peer->gps.alt = air_0.alt; // m

    switch (air_0.extra_type)
    {
    case 0:
        peer->gps.groundCourse = air_0.extra_value * 10;
        break;

    case 1:
        peer->gps.groundSpeed = air_0.extra_value * 20;
        break;

    case 2:
        peer->name[0] = air_0.extra_value;
        break;

    case 3:
        peer->name[1] = air_0.extra_value;
        break;

    case 4:
        peer->name[2] = air_0.extra_value;
        peer->name[3] = 0;
        break;

    default:
        break;
    }

    if (peer->gps.lat != 0 && peer->gps.lon != 0)
    {
        // Restore the last known coordinates
        peer->gps_rec.lat = peer->gps.lat;
        peer->gps_rec.lon = peer->gps.lon;
        peer->gps_rec.alt = peer->gps.alt;
        peer->gps_rec.groundCourse = peer->gps.groundCourse;
        peer->gps_rec.groundSpeed = peer->gps.groundSpeed;
    }

    sys.num_peers = PeerManager::getSingleton()->count();

    if ((sys.air_last_received_id == curr.id) && (sys.phase > MODE_OTA_SYNC) && !sys.lora_no_tx)
    {
        // Slot conflict
        uint32_t cs1 = peer->name[0] + peer->name[1] * 26 + peer->name[2] * 26 * 26;
        uint32_t cs2 = curr.name[0] + curr.name[1] * 26 + curr.name[2] * 26 * 26 + 1;
        if (cs1 < cs2)
        { // Pick another slot
            sprintf(sys.message, "%s", "ID CONFLICT");
            pick_id();
            resync_tx_slot(cfg.lora_timing_delay);
        }
    }
    peer->packetsReceived++;
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

void RadioManager::statusJson(JsonDocument *doc)
{
    JsonArray radiosArray = doc->createNestedArray("radios");
    for (uint8_t i = 0; i < MAX_RADIOS; i++)
    {
        if (radios[i] != nullptr) {
            Radio *radio = radios[i];
            JsonObject o = radiosArray.createNestedObject();
            o["status"] = radio->getStatusString();
            o["enabled"] = radio->getEnabled();
        }
    }
}

void RadioManager::setRadioStatus(uint8_t index, bool status)
{
    if (radios[index] == nullptr) {
        return;
    }
    radios[index]->setEnabled(status);
}