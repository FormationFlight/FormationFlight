// Smoke test that proves the native env + seams + fakes actually compile,
// link, and run together, independent of any of this suite's other coverage.
//
// Test functions are plain (non-static) so test_main.cpp (this suite's one
// Unity entry point) can RUN_TEST() them by extern declaration.

#include <unity.h>

#include "test_helpers.h"

void test_follow_controller_constructs_with_fakes() {
    FollowHarness h;

    // The controller seeds its live config from the compile-time defaults;
    // the chase-high default slot is 15 m behind.
    TEST_ASSERT_EQUAL_DOUBLE(-15.0, h.ctl.config().ofsLongM);
}
