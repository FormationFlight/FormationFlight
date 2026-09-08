#pragma once

#include "../../../src/lib/Follow/FollowDeps.h"
#include "../../../src/lib/GNSS/GNSSManager.h"

// Test double for IFollowGnss. Only the "self position" lookup needs
// faking -- the great-circle
// math itself (distanceMeters()/courseDegrees(), file-local to
// GNSSManager.cpp) is already pure and trustworthy, so this delegates to a
// real GNSSManager instance with its own spoofing hook (spoofLocationEnabled/
// spoofedLocation) pointed at the test-settable self position, rather than
// re-implementing the great-circle formulas here.
class FakeGnss : public IFollowGnss {
public:
    void setSelf(GNSSLocation loc)
    {
        real.spoofedLocation = loc;
        real.spoofLocationEnabled = true;
    }

    double horizontalDistanceTo(GNSSLocation b) override { return real.horizontalDistanceTo(b); }
    int16_t courseTo(GNSSLocation b) override { return real.courseTo(b); }

private:
    GNSSManager real;
};
