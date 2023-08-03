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

peer_t *PeerManager::getSpoofedPeer(uint8_t index)
{
    // Return true data for self
    if ((index + 1) == curr.id) {
        return &peers[index];
    }

    if (index >= NODES_MAX)
    {
        return nullptr;
    }

    GNSSLocation spoofOrigin = GNSSManager::getSingleton()->getLocation();
    if (spoofOrigin.fixType == GNSS_FIX_TYPE_NONE)
    {
        // Pick an arbitrary point to spoof peers at if we don't know where we are
        // 45.171546, 5.722387 is Grenoble, France where OlivierC-FR comes from as an homage to his project iNav Radar
        spoofOrigin.lat = 45.171546;
        spoofOrigin.lon = 5.722387;
    }

    uint8_t id = index + 1;
    spoofedPeers[index].id = id;

    // Generate peers in 100m offsets away in a circle around the user
    GNSSLocation peerLocation = GNSSManager::generatePointAround(spoofOrigin, index, cfg.lora_nodes, 100 * (index + 1));
    spoofedPeers[index].gps.lat = (int32_t)(peerLocation.lat * 1000000),
    spoofedPeers[index].gps.lon = (int32_t)(peerLocation.lon * 1000000),
    spoofedPeers[index].gps.alt = 100,
    spoofedPeers[index].gps.groundSpeed = 0,
    spoofedPeers[index].gps.groundCourse = 0,
    spoofedPeers[index].distance = GNSSManager::getSingleton()->horizontalDistanceTo(peerLocation);

    spoofedPeers[index].gps_pre.lat = (int32_t)(peerLocation.lat * 1000000),
    spoofedPeers[index].gps_pre.lon = (int32_t)(peerLocation.lon * 1000000),
    spoofedPeers[index].gps_pre.alt = 100,
    spoofedPeers[index].gps_pre.groundSpeed = 0,
    spoofedPeers[index].gps_pre.groundCourse = 0,

    spoofedPeers[index].state = 1;
    spoofedPeers[index].lost = 0;
    spoofedPeers[index].updated = millis();
    spoofedPeers[index].lq = 4;
    spoofedPeers[index].name[0] = 'F';
    spoofedPeers[index].name[1] = 'A';
    spoofedPeers[index].name[2] = 'K' + index;
    spoofedPeers[index].name[3] = '\0';
    spoofedPeers[index].rssi = -50 + id;

    return &spoofedPeers[index];
}

peer_t *PeerManager::getPeerMutable(uint8_t index)
{
    if (index >= NODES_MAX)
    {
        return nullptr;
    }

    return &peers[index];
}

const peer_t *PeerManager::getPeer(uint8_t index)
{
    if(this->spoofingPeers)
    {
        return this->getSpoofedPeer(index);
    }

    return this->getPeerMutable(index);
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

        spoofedPeers[i].id = 0;
        spoofedPeers[i].host = 0;
        spoofedPeers[i].state = 0;
        spoofedPeers[i].lost = 0;
        spoofedPeers[i].broadcast = 0;
        spoofedPeers[i].lq_updated = millis();
        spoofedPeers[i].lq_tick = 0;
        spoofedPeers[i].lq = 0;
        spoofedPeers[i].updated = 0;
        spoofedPeers[i].rssi = 0;
        spoofedPeers[i].distance = 0;
        spoofedPeers[i].direction = 0;
        spoofedPeers[i].relalt = 0;
        spoofedPeers[i].packetsReceived = 0;

        strcpy(peers[i].name, "");
        strcpy(spoofedPeers[i].name, "");
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
        const peer_t *peer = getPeer(i);
        // If active, don't count lost peers
        if (peer != NULL && peer->id != 0)
        {
            if (active && peer->lost > 0)
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
    (*doc)["maxPeers"] = NODES_MAX;
    if (this->spoofingPeers) {
        (*doc)["spoofing"] = this->spoofingPeers;
    }
    JsonArray peerArray = doc->createNestedArray("peers");
    for (uint8_t i = 0; i < NODES_MAX; i++)
    {
        const peer_t *peer = getPeer(i);
        if (peer != NULL && peer->id != 0)
        {
            JsonObject o = peerArray.createNestedObject();
            o["rawId"] = peer->id;
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

// Enables or disables spoofing fake peers
void PeerManager::enableSpoofing(bool enabled)
{
    this->spoofingPeers = enabled;
}

