// Peer lock state machine (resolveLock(), driven through loop() since
// resolveLock() itself is a private FollowManager method). Needs FakePeers
// (+ FakeMsp for the follow gate).

#include <unity.h>
#include <cstring>

#include "test_helpers.h"

static const char *stateOf(FollowManager &fm)
{
    static DynamicJsonDocument doc(1024);
    doc.clear();
    fm.statusJson(&doc);
    static char buf[32];
    strncpy(buf, doc["state"].as<const char *>(), sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    return buf;
}

static void tick(FollowManager &fm) { followTick(fm); }

void test_fresh_manager_stays_acquiring_with_no_peers()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers; // all slots id==0
    FollowManager fm(&msp, &gnss, &peers);
    msp.gcsNavActive = true;

    tick(fm);
    TEST_ASSERT_EQUAL_STRING("ACQUIRING", stateOf(fm));
}

void test_fresh_manager_locks_within_one_cycle_once_peer_exists()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers;
    peers.setPeer(0, /*id=*/1, 1.0, 1.0, 0.0, 0.0);
    FollowManager fm(&msp, &gnss, &peers);
    msp.gcsNavActive = true;

    tick(fm); // IDLE -> ACQUIRING -> LOCKED, all in this one cycle
    TEST_ASSERT_EQUAL_STRING("LOCKED", stateOf(fm));

    DynamicJsonDocument doc(1024);
    fm.statusJson(&doc);
    TEST_ASSERT_EQUAL(1, doc["lockedId"].as<int>());
}

void test_target_peer_zero_locks_first_active_in_iteration_order()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers;
    // Slot 0 unset (id==0, skipped); slot 2 is the first live peer.
    peers.setPeer(2, /*id=*/3, 1.0, 1.0, 0.0, 0.0);
    peers.setPeer(4, /*id=*/5, 2.0, 2.0, 0.0, 0.0);
    FollowManager fm(&msp, &gnss, &peers);
    msp.gcsNavActive = true; // targetPeer defaults to 0 (FIRST_ACTIVE)

    tick(fm);
    DynamicJsonDocument doc(1024);
    fm.statusJson(&doc);
    TEST_ASSERT_EQUAL(3, doc["lockedId"].as<int>());
}

void test_target_peer_pinned_ignores_other_live_peers()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers;
    peers.setPeer(0, /*id=*/1, 1.0, 1.0, 0.0, 0.0);
    peers.setPeer(4, /*id=*/5, 2.0, 2.0, 0.0, 0.0);
    FollowManager fm(&msp, &gnss, &peers);
    msp.gcsNavActive = true;

    FollowRuntimeConfig cfg = fm.getConfig();
    cfg.targetPeer = 5;
    String err;
    TEST_ASSERT_TRUE(fm.applyConfig(cfg, &err));

    tick(fm);
    DynamicJsonDocument doc(1024);
    fm.statusJson(&doc);
    TEST_ASSERT_EQUAL(5, doc["lockedId"].as<int>());
}

void test_locked_peer_going_stale_enters_locked_holding_and_keeps_id()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers;
    peers.setPeer(0, /*id=*/1, 1.0, 1.0, 0.0, 0.0);
    FollowManager fm(&msp, &gnss, &peers);
    msp.gcsNavActive = true;

    tick(fm);
    TEST_ASSERT_EQUAL_STRING("LOCKED", stateOf(fm));

    peers.markStale(0);
    tick(fm);
    TEST_ASSERT_EQUAL_STRING("LOCKED_HOLDING", stateOf(fm));

    DynamicJsonDocument doc(1024);
    fm.statusJson(&doc);
    TEST_ASSERT_EQUAL(1, doc["lockedId"].as<int>()); // retained, not cleared
}

void test_locked_holding_peer_returning_with_same_name_relocks()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers;
    peers.setPeer(0, /*id=*/1, 1.0, 1.0, 0.0, 0.0, /*relalt=*/0, "ABC");
    FollowManager fm(&msp, &gnss, &peers);
    msp.gcsNavActive = true;

    tick(fm);
    peers.markStale(0);
    tick(fm);
    TEST_ASSERT_EQUAL_STRING("LOCKED_HOLDING", stateOf(fm));

    // Telemetry returns: refresh the same peer (same id, same name) so it's
    // fresh again.
    peers.setPeer(0, /*id=*/1, 1.0, 1.0, 0.0, 0.0, /*relalt=*/0, "ABC");
    tick(fm);
    TEST_ASSERT_EQUAL_STRING("LOCKED", stateOf(fm));

    DynamicJsonDocument doc(1024);
    fm.statusJson(&doc);
    TEST_ASSERT_EQUAL(1, doc["lockedId"].as<int>());
}

void test_locked_holding_id_reused_by_different_aircraft_does_not_relock()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers;
    peers.setPeer(0, /*id=*/1, 1.0, 1.0, 0.0, 0.0, /*relalt=*/0, "ABC");
    FollowManager fm(&msp, &gnss, &peers);
    msp.gcsNavActive = true;

    tick(fm);
    peers.markStale(0);
    tick(fm);
    TEST_ASSERT_EQUAL_STRING("LOCKED_HOLDING", stateOf(fm));

    // Same LoRa slot id, but a different aircraft's name -- must NOT
    // silently adopt it under the old id.
    peers.setPeer(0, /*id=*/1, 1.0, 1.0, 0.0, 0.0, /*relalt=*/0, "XYZ");
    tick(fm);

    DynamicJsonDocument doc(1024);
    fm.statusJson(&doc);
    TEST_ASSERT_EQUAL(0, doc["lockedId"].as<int>()); // cleared, not adopted
    // Only a gate cycle can recover -- state stays stuck in LOCKED_HOLDING
    // (lockedId==0 makes getPeerById(0) always nullptr) even though a live
    // peer exists under that same slot id.
    TEST_ASSERT_EQUAL_STRING("LOCKED_HOLDING", stateOf(fm));
}

void test_gate_inactive_mid_lock_forces_idle_and_clears_lock()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers;
    peers.setPeer(0, /*id=*/1, 1.0, 1.0, 0.0, 0.0);
    FollowManager fm(&msp, &gnss, &peers);
    msp.gcsNavActive = true;

    tick(fm);
    TEST_ASSERT_EQUAL_STRING("LOCKED", stateOf(fm));

    msp.gcsNavActive = false;
    tick(fm);
    TEST_ASSERT_EQUAL_STRING("IDLE", stateOf(fm));

    DynamicJsonDocument doc(1024);
    fm.statusJson(&doc);
    TEST_ASSERT_EQUAL(0, doc["lockedId"].as<int>());
    TEST_ASSERT_EQUAL_STRING("", doc["lockedName"].as<const char *>());
}

void test_applyConfig_target_peer_change_forces_reacquire_mid_lock()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers;
    peers.setPeer(0, /*id=*/1, 1.0, 1.0, 0.0, 0.0);
    peers.setPeer(4, /*id=*/5, 2.0, 2.0, 0.0, 0.0);
    FollowManager fm(&msp, &gnss, &peers);
    msp.gcsNavActive = true;

    tick(fm);
    TEST_ASSERT_EQUAL_STRING("LOCKED", stateOf(fm));

    FollowRuntimeConfig cfg = fm.getConfig();
    cfg.targetPeer = 5; // was 0 (FIRST_ACTIVE), now pinned elsewhere
    String err;
    TEST_ASSERT_TRUE(fm.applyConfig(cfg, &err));

    // forceReacquire() fires synchronously inside applyConfig() -- no
    // additional loop() cycle needed to observe the reset.
    DynamicJsonDocument doc(1024);
    fm.statusJson(&doc);
    TEST_ASSERT_EQUAL_STRING("ACQUIRING", doc["state"].as<const char *>());
    TEST_ASSERT_EQUAL(0, doc["lockedId"].as<int>());
}
