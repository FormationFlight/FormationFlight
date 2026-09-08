// offsetGeometrySane()'s minimum-separation safety bounds. Pure predicate
// tests plus one applyConfig() end-to-end case; no fakes are exercised
// (applyConfig() never touches msp/gnss/peers), even though it constructs
// a FollowManager.

#include <unity.h>
#include <cstring>

#include "FollowManager.h"
#include "FollowInternal.h"
#include "FakeMsp.h"
#include "FakeGnss.h"
#include "FakePeers.h"

// ---- 3D magnitude boundary (minSepM) ----

void test_geometry_sane_at_exactly_minSepM_passes()
{
    // horizontalMag = 3.0 >= FOLLOW_STACKED_HORIZONTAL_EPSILON_M (0.5), so
    // the stacked-vertical rule below doesn't apply here regardless of minVSepM.
    FollowOffset offset{3.0, 0.0, 0.0};
    TEST_ASSERT_TRUE(offsetGeometrySane(offset, /*minSepM=*/3.0, /*minVSepM=*/10.0, nullptr));
}

void test_geometry_sane_just_under_minSepM_fails()
{
    FollowOffset offset{2.99, 0.0, 0.0};
    String err;
    TEST_ASSERT_FALSE(offsetGeometrySane(offset, /*minSepM=*/3.0, /*minVSepM=*/10.0, &err));
    TEST_ASSERT_TRUE(strstr(err.c_str(), "minSepM") != nullptr);
}

// ---- Stacked-slot vertical-separation rule ----

void test_non_stacked_slot_ignores_minVSepM()
{
    // horizontalMag = 3.0 (non-stacked), vertical = 0.1 well under minVSepM
    // -- the stacked rule shouldn't apply, so this must still pass.
    FollowOffset offset{3.0, 0.0, 0.1};
    TEST_ASSERT_TRUE(offsetGeometrySane(offset, /*minSepM=*/1.0, /*minVSepM=*/10.0, nullptr));
}

void test_stacked_slot_under_minVSepM_fails()
{
    // horizontalMag = 0.1 < FOLLOW_STACKED_HORIZONTAL_EPSILON_M (0.5) --
    // stacked. mag3d = ~2.0 >= minSepM(1.0), so only the stacked check can
    // fail this one.
    FollowOffset offset{0.0, 0.1, 2.0};
    String err;
    TEST_ASSERT_FALSE(offsetGeometrySane(offset, /*minSepM=*/1.0, /*minVSepM=*/3.0, &err));
    TEST_ASSERT_TRUE(strstr(err.c_str(), "minVSepM") != nullptr);
}

void test_stacked_slot_at_exactly_minVSepM_passes()
{
    FollowOffset offset{0.0, 0.1, 3.0};
    TEST_ASSERT_TRUE(offsetGeometrySane(offset, /*minSepM=*/1.0, /*minVSepM=*/3.0, nullptr));
}

// ---- applyConfig() end-to-end: the same rule, exercised through the
// caller wiring, not just the pure helper. ----

void test_applyConfig_rejects_geometrically_insane_static_offset()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers;
    FollowManager fm(&msp, &gnss, &peers);

    FollowRuntimeConfig before = fm.getConfig();

    FollowRuntimeConfig bad = before;
    bad.ofsLongM = 0.1;
    bad.ofsLatM = 0.0;
    bad.ofsVertM = 0.0;
    bad.minSepM = 3.0; // 0.1 magnitude is well under this

    String err;
    TEST_ASSERT_FALSE(fm.applyConfig(bad, &err));
    TEST_ASSERT_TRUE(strstr(err.c_str(), "minSepM") != nullptr);

    // A rejected applyConfig() must leave the live config completely
    // untouched.
    TEST_ASSERT_EQUAL_DOUBLE(before.ofsLongM, fm.getConfig().ofsLongM);
}
