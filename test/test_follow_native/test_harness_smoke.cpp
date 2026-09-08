// Smoke test that proves the native env + seams + fakes actually compile,
// link, and run together, independent of any of this suite's other coverage.

#include <unity.h>

#include "FollowManager.h"
#include "FakeMsp.h"
#include "FakeGnss.h"
#include "FakePeers.h"

void test_follow_manager_constructs_with_fakes()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers;
    FollowManager fm(&msp, &gnss, &peers);

    DynamicJsonDocument doc(1024);
    fm.configJson(&doc);
    TEST_ASSERT_TRUE(doc.containsKey("ofsLongM"));
}
