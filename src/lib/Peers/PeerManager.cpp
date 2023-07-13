#include "PeerManager.h"
#include "../ConfigStrings.h"
#include "../GNSS/GNSSManager.h"
#include "main.h"

PeerManager *peerManager = nullptr;

PeerManager *PeerManager::getSingleton()
{
    if (peerManager == nullptr)
    {
        peerManager = new PeerManager();
    }
    return peerManager;
}

peer_t *PeerManager::getPeer(uint8_t index)
{
    if (index >= NODES_MAX)
    {
        return nullptr;
    }
    return &peers[index];
}

void PeerManager::reset()
{
    for (int i = 0; i < NODES_MAX; i++)
    {
        peers[i].id = 0;
        peers[i].host = 0;
        peers[i].state = 0;
        peers[i].lost = 0;
        peers[i].broadcast = 0;
        peers[i].lq_updated = millis();
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
    // Calculate peer distances
    static unsigned long lastUpdate = 0;
    GNSSManager *gnssManager = GNSSManager::getSingleton();
    GNSSLocation loc = gnssManager->getLocation();

    if (millis() - lastUpdate > 100)
    {
        for (int i = 0; i < NODES_MAX; i++)
        {
            peer_t *peer = &peers[i];

            // Set LQ & Lost on peers
            if (millis() > (peer->lq_updated + sys.lora_cycle * NODES_MAX))
            {
                uint16_t diff = peer->updated - peer->lq_updated;
                peer->lq = constrain(peer->lq_tick * 4.2 * sys.lora_cycle / diff, 0, 4);
                peer->lq_updated = millis();
                peer->lq_tick = 0;
            }
            if (peer->id > 0 && ((millis() - peer->updated) > LORA_PEER_TIMEOUT))
            {
                peer->lost = 2;
            }
            
            // Set the distance, direction, and relative altitude of valid peers
            if (loc.fixType != GNSS_FIX_TYPE_NONE && peer->id > 0 && !peer->lost)
            {
                peer_t *peer = &peers[i];

                GNSSLocation peerLocation{.lat = peer->gps.lat / 1000000.0, .lon = peer->gps.lon / 1000000.0, .alt = (double)peer->gps.alt};
                peer->distance = gnssManager->horizontalDistanceTo(peerLocation);
                peer->direction = gnssManager->courseTo(peerLocation);
                peer->relalt = peerLocation.alt - loc.alt;
            }
        }
        lastUpdate = millis();
    }
}

uint8_t PeerManager::count(bool active)
{
    int n = 0;
    for (int i = 0; i < NODES_MAX; i++)
    {
        // If active, don't count lost peers
        if (peers[i].id > 0)
        {
            if (active && peers[i].lost > 0)
            {
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
    (*doc)["myID"] = peer_slotname[curr.id];
    (*doc)["count"] = count();
    (*doc)["countActive"] = count_active();
    JsonArray peerArray = doc->createNestedArray("peers");
    for (uint8_t i = 0; i < NODES_MAX; i++)
    {
        if (getPeer(i)->id != 0)
        {
            peer_t *peer = getPeer(i);
            JsonObject o = peerArray.createNestedObject();
            o["id"] = peer_slotname[peer->id];
            // TODO: get curr id from SystemManager
            // o["self"] = peer->id == curr.id;
            o["name"] = peer->name;
            o["updated"] = peer->updated;
            o["age"] = millis() - peer->updated;
            o["lost"] = peer->lost;
            o["lat"] = peer->gps.lat / 1000000.0;
            o["lon"] = peer->gps.lon / 1000000.0;
            o["latRaw"] = peer->gps.lat;
            o["lonRaw"] = peer->gps.lon;
            o["alt"] = peer->gps.alt;
            o["groundSpeed"] = peer->gps.groundSpeed;
            o["groundCourse"] = peer->gps.groundCourse;
            o["distance"] = peer->distance;
            o["courseTo"] = peer->direction;
            o["relativeAltitude"] = peer->relalt;
            o["packetsReceived"] = peer->packetsReceived;
            o["lq"] = peer->lq;
            if (peer->rssi != 0) {
                o["rssi"] = peer->rssi;
            }
        }
    }
}
