#pragma once

#include "../../../src/lib/Follow/FollowDeps.h"
#include <array>
#include <cmath>

// Test double for IFollowPeers. Mirrors the real PeerManager's id<->index
// convention exactly (PeerManager.cpp's getPeerById()/getPeer()): table is
// indexed 0..NODES_MAX-1, getPeerById(id) looks up table[id - 1], and an
// unset slot's id is 0 -- so the "First Active" scan (skip id==0, first
// non-stale wins) behaves identically against real and fake.
class FakePeers : public IFollowPeers {
public:
    std::array<peer_t, NODES_MAX> table{};

    // Sets peer `index`'s id/position/course/speed and marks it fresh
    // (updated = current native_millis_*() time) -- lat/lon are plain
    // degrees, groundSpeedMs is m/s, groundCourseDeg is degrees; converted
    // here to peer_t::gps's internal units (x1e6 lat/lon -- see
    // FollowManager.h's slotToLatLon() comment for why that's not x1e7 --
    // cm/s speed, degrees x10 course).
    void setPeer(uint8_t index, uint8_t id, double lat, double lon,
                 double groundSpeedMs, double groundCourseDeg, int16_t relalt = 0,
                 const char *name = "PR")
    {
        peer_t &p = table[index];
        p = peer_t{};
        p.id = id;
        p.lost = 0;
        p.updated = millis();
        p.relalt = relalt;
        strncpy(p.name, name, sizeof(p.name) - 1);
        p.gps.fixType = 2;
        p.gps.numSat = 10;
        p.gps.lat = (int32_t)lround(lat * 1e6);
        p.gps.lon = (int32_t)lround(lon * 1e6);
        p.gps.groundSpeed = (int16_t)lround(groundSpeedMs * 100.0);
        p.gps.groundCourse = (int16_t)lround(groundCourseDeg * 10.0);
    }

    // Simulates peer_is_stale() tripping without waiting out peerTimeoutMs.
    void markStale(uint8_t index) { table[index].lost = 1; }

    const peer_t *getPeer(uint8_t index) override
    {
        if (index >= NODES_MAX)
        {
            return nullptr;
        }
        return &table[index];
    }

    const peer_t *getPeerById(uint8_t id) override
    {
        if (id == 0 || id > NODES_MAX)
        {
            return nullptr;
        }
        return getPeer(id - 1);
    }
};
