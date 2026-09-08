// The two-layer RC safety net, axisSignLocked()/candidateOffsetOk(). Pure
// predicate tests only -- resolveOffset()'s freeze behavior (which needs a
// locked peer via the state machine) is instead exercised indirectly in
// test_gvar_reporting.cpp, as a side effect of a condition-code-priority
// test that drives an RC-rejected candidate through loop().

#include <unity.h>

#include "FollowManager.h"
#include "FollowInternal.h"

// ---- Layer 1: geometry sanity alone is enough to reject, regardless of
// Layer 2's sign-lock outcome. ----

void test_layer1_rejects_candidate_failing_geometry_alone()
{
    // Candidate's own magnitude (2.0) is under minSepM (3.0) -- Layer 1
    // fails outright. Reference doesn't even share a sign with candidate's
    // longitudinal axis, so if Layer 1 weren't checked first this could
    // look like a Layer-2-only case; candidateOffsetOk() must still reject it.
    FollowOffset candidate{2.0, 0.0, 0.0};
    FollowOffset reference{-15.0, 0.0, 10.0};
    TEST_ASSERT_FALSE(candidateOffsetOk(candidate, reference, /*minSepM=*/3.0, /*minVSepM=*/1.0));
}

// ---- Layer 2: sign flip on an axis, other axes' combined magnitude
// decides whether it's safe. ----

void test_layer2_rejects_sign_flip_when_other_axes_below_minSepM()
{
    // candidate.longitudinal_m (+2) flips sign vs reference's (-15).
    // candidate's other axes (lat=0, vert=0) magnitude = 0, reference's
    // other axes (lat=0, vert=10) magnitude = 10 -- coMag = min(0, 10) = 0,
    // well under minSepM. This is the "would pass directly through the
    // leader" case the safety net exists for.
    FollowOffset candidate{2.0, 0.0, 0.0};
    FollowOffset reference{-15.0, 0.0, 10.0};
    TEST_ASSERT_FALSE(candidateOffsetOk(candidate, reference, /*minSepM=*/1.5, /*minVSepM=*/1.0));
}

void test_layer2_allows_sign_flip_when_other_axes_above_minSepM()
{
    // Same longitudinal sign flip as above, but this time both candidate's
    // and reference's other-axes magnitude clear minSepM -- there's a safe
    // path around the leader, not through it, so this must be allowed.
    FollowOffset candidate{2.0, 5.0, 0.0};
    FollowOffset reference{-15.0, 5.0, 10.0};
    TEST_ASSERT_TRUE(candidateOffsetOk(candidate, reference, /*minSepM=*/1.5, /*minVSepM=*/1.0));
}

// ---- A zero on either side is the boundary itself, never a "side" --
// axisSignLocked() must never treat it as a crossing. ----

void test_zero_on_candidate_side_never_counts_as_crossed()
{
    // candidateAxis == 0, referenceAxis very negative, other-axes magnitude
    // effectively zero -- if 0 counted as "positive" or "negative" this
    // would look like a crossing with coMag well under minSepM and get
    // rejected. It must instead pass straight through (not crossed).
    TEST_ASSERT_FALSE(axisSignLocked(/*candidateAxis=*/0.0, /*referenceAxis=*/-15.0,
                                      /*candidateOther1=*/0.0, /*candidateOther2=*/0.0,
                                      /*referenceOther1=*/0.0, /*referenceOther2=*/0.0,
                                      /*minSepM=*/3.0));
}

void test_zero_on_reference_side_never_counts_as_crossed()
{
    TEST_ASSERT_FALSE(axisSignLocked(/*candidateAxis=*/-15.0, /*referenceAxis=*/0.0,
                                      /*candidateOther1=*/0.0, /*candidateOther2=*/0.0,
                                      /*referenceOther1=*/0.0, /*referenceOther2=*/0.0,
                                      /*minSepM=*/3.0));
}
