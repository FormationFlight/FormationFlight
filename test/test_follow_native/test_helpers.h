#pragma once

#include "FollowManager.h"
#include "FakeMsp.h"
#include "FakeGnss.h"
#include "FakePeers.h"
#include "Arduino.h" // native_millis_set()/native_millis_advance()

// Shared driving helpers for tests that need loop()-cycle-level behavior --
// resolveLock(), resolveOffset(), etc. are private FollowManager methods,
// so this is the only way to exercise them from outside.

// Advances native time past the default emitHz's period (250ms at 4Hz) so
// loop()'s own nextRunTime gate doesn't swallow the next cycle.
inline void followTick(FollowManager &fm)
{
    native_millis_advance(300);
    fm.loop();
}

// Sets up FakePeers slot 0 as peer id=1 at (lat,lon), moving at
// groundSpeedMs along groundCourseDeg, and points FakeGnss's self position
// at the same (lat,lon) -- horizontalDistanceTo()/courseTo() otherwise
// operate against GNSSManager's zero-valued default location, which every
// full-loop() test needs to avoid.
inline void setupLockedPeer(FakePeers &peers, FakeGnss &gnss, double lat, double lon,
                             double groundSpeedMs = 10.0, double groundCourseDeg = 0.0)
{
    peers.setPeer(0, /*id=*/1, lat, lon, groundSpeedMs, groundCourseDeg);
    GNSSLocation self{};
    self.lat = lat;
    self.lon = lon;
    gnss.setSelf(self);
}
