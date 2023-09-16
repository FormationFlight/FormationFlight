#include <Arduino.h>
#include "RadioManager.h"
#include "main.h"
#include "../Helpers.h"
#include "../ConfigStrings.h"
#ifdef HAS_LORA
#include <RadioLib.h>
#endif
#include "../Cryptography/CryptoManager.h"
#include <ArduinoJson.h>
#include "../Peers/PeerManager.h"
#include "../GNSS/GNSSManager.h"

RadioManager::RadioManager()
{
}

RadioManager *radioManager = nullptr;

RadioManager *RadioManager::getSingleton()
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
    GNSSLocation loc = GNSSManager::getSingleton()->getLocation();
    air_0.lat = (int32_t)(loc.lat * 1000000.0);
    air_0.lon = (int32_t)(loc.lon * 1000000.0);
    air_0.alt = loc.alt;
    air_0.extra_type = sys.ota_nonce % 5;

    switch (air_0.extra_type)
    {
    case 0:
        air_0.extra_value = loc.groundCourse;  // location.groundCourse is in degrees so we don't need to convert before sending
        break;

    case 1:
        air_0.extra_value = loc.groundSpeed / 20;
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

ReceiveResult RadioManager::receive(const uint8_t *rawPacket, size_t packetSize, float rssi)
{
    // Check packet size
    if (packetSize != sizeof(air_type0_t))
    {
        return RECEIVE_RESULT_BAD_SIZE;
    }
    air_type0_t air_0;
    memcpy_P(&air_0, rawPacket, packetSize);
    if (air_0.packet_type != PACKET_TYPE_RADAR_POSITION)
    {
        // We can implement additional packet types here
        DBGF("Received bad packet type: %d\n", air_0.packet_type);
        return RECEIVE_RESULT_BAD_PACKET_TYPE;
    }

    uint8_t calculatedCrc = 0;
    // Check CRC
    for (uint8_t i = 0; i < sizeof(air_type0_t) - sizeof(air_0.crc); i++)
    {
        calculatedCrc = crc8_dvb_s2(calculatedCrc, rawPacket[i]);
    }
    if (calculatedCrc != air_0.crc)
    {
        return RECEIVE_RESULT_BAD_CRC;
    }
    if (air_0.id == 0 || air_0.id > NODES_MAX)
    {
        DBGF("Received bad ID: %d\n", air_0.id);
        return RECEIVE_RESULT_BAD_ID;
    }
    // TODO: use this radio's OTA time
    if (air_0.id == curr.id && millis() - sys.last_tx_end < 4)
    {
        DBGF("IGNORING packet with our own ID %s. Last TX was %u-%u\n", peer_slotname[air_0.id], sys.last_tx_start, sys.last_tx_end);
        return RECEIVE_RESULT_BAD_ID_DUPLICATE;
    }
    if (air_0.extra_type > 4)
    {
        return RECEIVE_RESULT_BAD_FIELD;
    }

    peer_t *peer = PeerManager::getSingleton()->getPeerMutable(air_0.id - 1);

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
    if (rssi != 0)
    {
        peer->rssi = int(rssi);
    }
    peer->gps.lat = air_0.lat;// * 100;
    peer->gps.lon = air_0.lon;// * 100;
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
    DBGF("Received packet from peer %s\n", peer_slotname[air_0.id]);

    if ((sys.air_last_received_id == curr.id) && (sys.phase > MODE_OTA_SYNC) && !sys.disable_tx)
    {
        // Pick another slot
        DBGF("[RadioManager] Received packet with our own ID %s, moving to %s. Last TX was %u-%u\n", peer_slotname[air_0.id], peer_slotname[curr.id], sys.last_tx_start, sys.last_tx_end);
        sprintf(sys.message, "%s", "ID CONFLICT");
        pick_id();
        resync_tx_slot(cfg.lora_timing_delay);
    }
    peer->packetsReceived++;
    return RECEIVE_RESULT_OK;
}

void RadioManager::transmit(air_type0_t *packet, uint8_t ota_nonce)
{
    for (uint8_t i = 0; i < MAX_RADIOS; i++)
    {
        if (radios[i] != nullptr)
        {
            radios[i]->transmit(packet, ota_nonce);
        }
    }
}

void RadioManager::addRadio(Radio *radio)
{
    for (uint8_t i = 0; i < MAX_RADIOS; i++)
    {
        if (radios[i] == nullptr)
        {
            radios[i] = radio;
            radios[i]->begin();
            return;
        }
    }
}

void RadioManager::loop()
{
    for (uint8_t i = 0; i < MAX_RADIOS; i++)
    {
        if (radios[i] == nullptr)
        {
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
        if (radios[i] != nullptr)
        {
            Radio *radio = radios[i];
            JsonObject o = radiosArray.createNestedObject();
            o["status"] = radio->getStatusString();
            o["counters"] = radio->getCounterString();
            o["enabled"] = radio->getEnabled();
        }
    }
}

void RadioManager::setRadioStatus(uint8_t index, bool status)
{
    if (radios[index] == nullptr)
    {
        return;
    }
    radios[index]->setEnabled(status);
}