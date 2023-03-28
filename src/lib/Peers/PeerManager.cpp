#include "PeerManager.h"
#include "../ConfigStrings.h"

PeerManager *peerManager = nullptr;

PeerManager* PeerManager::getSingleton()
{
    if (peerManager == nullptr)
    {
        peerManager = new PeerManager();
    }
    return peerManager;
}

peer_t* PeerManager::getPeer(uint8_t index)
{
    if (index >= LORA_NODES_MAX) {
        return nullptr;
    }
    return &peers[index];
}

void PeerManager::reset()
{
    sys.now_sec = millis();
    for (int i = 0; i < LORA_NODES_MAX; i++)
    {
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
        peers[i].packetsReceived = 0;
        strcpy(peers[i].name, "");
    }
}

void PeerManager::loop()
{
    // TODO: Calculate peer distance and other nice metrics
}

uint8_t PeerManager::count(bool active)
{
    int n = 0;
    for (int i = 0; i < LORA_NODES_MAX; i++)
    {
        // If active, don't count lost peers
        if (peers[i].id > 0)
        {
            if (active && peers[i].lost > 0) {
                continue;
            }
            n++;
        }
    }
    return n;
}

uint8_t PeerManager::count_active()
{
    return count(true);
}

void PeerManager::statusJson(JsonDocument *doc)
{
    JsonArray peerArray = doc->createNestedArray("peers");
    for (uint8_t i = 0; i < LORA_NODES_MAX; i++)
    {
        if (getPeer(i)->id != 0) {
            peer_t *peer = getPeer(i);
            JsonObject o = peerArray.createNestedObject();
            o["id"] = peer_slotname[peer->id];
            o["self"] = peer->id == curr.id;
            o["name"] = peer->name;
            o["updated"] = peer->updated;
            o["lost"] = peer->lost;
            o["lat"] = peer->gps.lat;
            o["lon"] = peer->gps.lon;
            o["alt"] = peer->gps.alt;
            o["groundSpeed"] = peer->gps.groundSpeed;
            o["groundCourse"] = peer->gps.groundCourse;
            o["packetsReceived"] = peer->packetsReceived;
        }
    }
}
