#include <Arduino.h>

#ifdef PLATFORM_ESP8266
#include <espnow.h>
#endif
#include "main.h"
#include "Helpers.h"

uint8_t broadcastAddress[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

void espnow_receive(uint8_t * mac, uint8_t *incomingData, uint8_t packetSize) {
    if (packetSize == 0) return;
    air_type0_t air_0;
    if (packetSize == sizeof(air_type0_t)) {
        memcpy_P(&air_0, incomingData, packetSize);
    } else {
        return;
    }

    uint8_t id = air_0.id - 1;

    peers[id].gps_pre.lat = peers[id].gps.lat;
    peers[id].gps_pre.lon = peers[id].gps.lon;
    peers[id].gps_pre.alt = peers[id].gps.alt;
    peers[id].gps_pre.groundCourse = peers[id].gps.groundCourse;
    peers[id].gps_pre.groundSpeed = peers[id].gps.groundSpeed;
    peers[id].gps_pre_updated = peers[id].updated;

    sys.air_last_received_id = air_0.id;
    peers[id].id = sys.air_last_received_id;
    peers[id].lq_tick++;
    peers[id].state = 0;
    peers[id].lost = 0;
    peers[id].updated = sys.lora_last_rx;
    peers[id].rssi = sys.last_rssi;

    peers[id].gps.lat = air_0.lat * 100;
    peers[id].gps.lon = air_0.lon * 100;
    peers[id].gps.alt = air_0.alt; // m
    
    switch (air_0.extra_type)
        {
        case 0 : peers[id].gps.groundCourse = air_0.extra_value * 10;
        break;

        case 1 : peers[id].gps.groundSpeed = air_0.extra_value * 20;
        break;

        case 2 : peers[id].name[0] = air_0.extra_value;
        break;

        case 3 : peers[id].name[1] = air_0.extra_value;
        break;

        case 4 : peers[id].name[2] = air_0.extra_value;
                 peers[id].name[3] = 0;
        break;

        default:
        break;
        }

    if (peers[id].gps.lat != 0 && peers[id].gps.lon != 0) {  // Save the last known coordinates
        peers[id].gps_rec.lat = peers[id].gps.lat;
        peers[id].gps_rec.lon = peers[id].gps.lon;
        peers[id].gps_rec.alt = peers[id].gps.alt;
        peers[id].gps_rec.groundCourse = peers[id].gps.groundCourse;
        peers[id].gps_rec.groundSpeed = peers[id].gps.groundSpeed;
    }

    sys.num_peers = count_peers(0, &cfg);

    if ((sys.air_last_received_id == curr.id) && (sys.phase > MODE_LORA_SYNC) && !sys.lora_no_tx) { // Slot conflict
        uint32_t cs1 = peers[id].name[0] + peers[id].name[1] * 26 + peers[id].name[2] * 26 * 26 ;
        uint32_t cs2 = curr.name[0] + curr.name[1] * 26 + curr.name[2] * 26 * 26 + 1;
        if (cs1 < cs2) { // Pick another slot
            sprintf(sys.message, "%s", "ID CONFLICT");
            pick_id();
            resync_tx_slot(cfg.lora_timing_delay);
        }
    }
}

void espnow_setup() {
    esp_now_init();
    esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
    esp_now_add_peer(broadcastAddress, ESP_NOW_ROLE_COMBO, 1, NULL, 0);
    esp_now_register_recv_cb(espnow_receive);
}

void espnow_send(air_type0_t *air_0)
{
    uint8_t buf[sizeof(air_type0_t)];
    memcpy_P(buf, air_0, sizeof(air_type0_t));
    esp_now_send(broadcastAddress, buf, sizeof(air_type0_t));
}
