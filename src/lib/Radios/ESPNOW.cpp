
#include "ESPNOW.h"
#include "RadioManager.h"
#include "../CryptoManager.h"
#include "../Helpers.h"
#include "../WiFi/WiFiManager.h"
#if defined(PLATFORM_ESP8266)
void espnow_receive(uint8_t *mac, uint8_t *incomingData, uint8_t packetSize)
#elif defined(PLATFORM_ESP32)
void espnow_receive(const uint8_t *mac_addr, const uint8_t *incomingData, int packetSize)
#endif
{
    if (!ESPNOW::getSingleton()->getEnabled()) {
        return;
    }
    uint8_t buf[packetSize];
    memcpy(buf, incomingData, packetSize);
    CryptoManager::getSingleton()->decrypt(buf, packetSize);
    ESPNOW::getSingleton()->handleReceiveCounters(RadioManager::getSingleton()->receive(buf, packetSize, 0));
}

ESPNOW *espnowInstance = nullptr;

ESPNOW::ESPNOW()
{
}

ESPNOW* ESPNOW::getSingleton()
{
    if (espnowInstance == nullptr)
    {
        espnowInstance = new ESPNOW();
    }
    return espnowInstance;
}

void ESPNOW::transmit(air_type0_t *air_0)
{
    if (!getEnabled()) {
        return;
    }
    uint8_t buf[sizeof(air_type0_t)];
    memcpy_P(buf, air_0, sizeof(air_type0_t));
    CryptoManager::getSingleton()->encrypt(buf, sizeof(air_type0_t));
    esp_now_send(broadcastAddress, buf, sizeof(air_type0_t));
    packetsTransmitted++;
    lastTransmitTime = millis();
}

int ESPNOW::begin()
{
    // Ensure WiFi was started
    WiFiManager::getSingleton();
    // Initialize ESPNOW
    esp_now_init();
#ifdef PLATFORM_ESP8266
    esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
    esp_now_add_peer(broadcastAddress, ESP_NOW_ROLE_COMBO, 1, NULL, 0);
#elif defined(PLATFORM_ESP32)
    memset((void*)&peerInfo, 0, sizeof(esp_now_peer_info_t));
    peerInfo.ifidx = WIFI_IF_AP;
    memcpy(peerInfo.peer_addr, broadcastAddress, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;
    esp_now_add_peer(&peerInfo);
#endif
    esp_now_register_recv_cb(espnow_receive);
    return 0;
}

void ESPNOW::loop()
{
}

String ESPNOW::getStatusString()
{
    char buf[64];
    sprintf(buf, "ESPNOW [%uTX/%uRX] [%uCRC/%uSIZE/%uVAL]", packetsTransmitted, packetsReceived, packetsBadCrc, packetsBadSize, packetsBadValidation);
    return String(buf);
}