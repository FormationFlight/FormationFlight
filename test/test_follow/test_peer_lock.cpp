// Peer lock state machine (resolveLock(), driven through service() since
// resolveLock() itself is a private FollowController method). Uses the
// harness's real PeerTable (+ FakeFc for the follow gate, FakeSelf for the
// own-fix gate on waypoint emission).
//
// Peers are UID-keyed in v2: there are no slot ids, and a UID is a stable
// identity, so v1's "slot id reused by a different aircraft" guard (and its
// test) has no counterpart here.

#include <unity.h>

#include <cstring>

#include "test_helpers.h"

void test_fresh_controller_stays_acquiring_with_no_peers() {
    FollowHarness h;  // empty peer table
    h.fc.gcsNav = true;

    h.tick();
    FollowStatus s = h.status();
    TEST_ASSERT_EQUAL(FOLLOW_LOCK_ACQUIRING, s.state);
    TEST_ASSERT_EQUAL_STRING("ACQUIRING", followLockStateName(s.state));
}

void test_fresh_controller_locks_within_one_cycle_once_peer_exists() {
    FollowHarness h;
    h.setPeer(/*uid=*/1, 1.0, 1.0, 0.0, 0.0);
    h.fc.gcsNav = true;

    h.tick();  // IDLE -> ACQUIRING -> LOCKED, all in this one cycle
    FollowStatus s = h.status();
    TEST_ASSERT_EQUAL(FOLLOW_LOCK_LOCKED, s.state);
    TEST_ASSERT_EQUAL_UINT32(1u, s.lockedUid);
}

void test_target_uid_zero_locks_first_active_in_table_order() {
    FollowHarness h;
    // The table hands out slots in insertion order, so the first-inserted
    // live peer is the "first active" one FIRST_ACTIVE picks.
    h.setPeer(/*uid=*/0x33, 1.0, 1.0, 0.0, 0.0);
    h.setPeer(/*uid=*/0x55, 2.0, 2.0, 0.0, 0.0);
    h.fc.gcsNav = true;  // targetUid defaults to 0 (FIRST_ACTIVE)

    h.tick();
    FollowStatus s = h.status();
    TEST_ASSERT_EQUAL(FOLLOW_LOCK_LOCKED, s.state);
    TEST_ASSERT_EQUAL_UINT32(0x33u, s.lockedUid);
}

void test_target_uid_pinned_ignores_other_live_peers() {
    FollowHarness h;
    h.setPeer(/*uid=*/1, 1.0, 1.0, 0.0, 0.0);
    h.setPeer(/*uid=*/5, 2.0, 2.0, 0.0, 0.0);
    h.fc.gcsNav = true;

    FollowConfig cfg = configOf(h);
    cfg.targetUid = 5;
    const char* err = nullptr;
    TEST_ASSERT_TRUE(h.apply(cfg, &err));

    h.tick();
    FollowStatus s = h.status();
    TEST_ASSERT_EQUAL(FOLLOW_LOCK_LOCKED, s.state);
    TEST_ASSERT_EQUAL_UINT32(5u, s.lockedUid);
}

void test_locked_peer_going_stale_enters_locked_holding_and_keeps_uid() {
    FollowHarness h;
    h.setPeer(/*uid=*/1, 1.0, 1.0, 0.0, 0.0);
    h.fc.gcsNav = true;

    h.tick();
    TEST_ASSERT_EQUAL(FOLLOW_LOCK_LOCKED, h.status().state);

    h.markStale(1);
    h.tick();
    FollowStatus s = h.status();
    TEST_ASSERT_EQUAL(FOLLOW_LOCK_LOCKED_HOLDING, s.state);
    TEST_ASSERT_EQUAL_UINT32(1u, s.lockedUid);  // retained, not cleared
}

void test_locked_holding_peer_returning_relocks_same_uid() {
    FollowHarness h;
    h.setPeer(/*uid=*/1, 1.0, 1.0, 0.0, 0.0);
    h.namePeer(1, "ABC");
    h.fc.gcsNav = true;

    h.tick();
    h.markStale(1);
    h.tick();
    TEST_ASSERT_EQUAL(FOLLOW_LOCK_LOCKED_HOLDING, h.status().state);

    // Telemetry returns: refresh the same peer (same UID) so it's fresh
    // again.
    h.setPeer(/*uid=*/1, 1.0, 1.0, 0.0, 0.0);
    h.tick();
    FollowStatus s = h.status();
    TEST_ASSERT_EQUAL(FOLLOW_LOCK_LOCKED, s.state);
    TEST_ASSERT_EQUAL_UINT32(1u, s.lockedUid);
    TEST_ASSERT_EQUAL_STRING("ABC", s.lockedName);
}

void test_locked_holding_never_fails_over_to_another_peer() {
    FollowHarness h;
    h.setupLockedPeer(/*uid=*/0xA, 1.0, 1.0);
    h.fc.gcsNav = true;

    h.tick();
    TEST_ASSERT_EQUAL(FOLLOW_LOCK_LOCKED, h.status().state);
    TEST_ASSERT_EQUAL_UINT32(0xAu, h.status().lockedUid);

    h.markStale(0xA);
    h.tick();
    TEST_ASSERT_EQUAL(FOLLOW_LOCK_LOCKED_HOLDING, h.status().state);

    // A fresh, perfectly followable peer B appears while we're holding on A.
    // LOCKED_HOLDING only ever re-checks A's UID -- it must never scan for or
    // adopt B (no automatic failover), and must emit nothing this cycle.
    h.setPeer(/*uid=*/0xB, 1.0, 1.0, 10.0, 0.0);
    h.fc.sentWaypoints.clear();
    h.tick();

    FollowStatus s = h.status();
    TEST_ASSERT_EQUAL(FOLLOW_LOCK_LOCKED_HOLDING, s.state);
    TEST_ASSERT_EQUAL_UINT32(0xAu, s.lockedUid);
    TEST_ASSERT_EQUAL_size_t(0, h.fc.sentWaypoints.size());
}

void test_gate_inactive_mid_lock_forces_idle_and_clears_lock() {
    FollowHarness h;
    h.setPeer(/*uid=*/1, 1.0, 1.0, 0.0, 0.0);
    h.fc.gcsNav = true;

    h.tick();
    TEST_ASSERT_EQUAL(FOLLOW_LOCK_LOCKED, h.status().state);

    h.fc.gcsNav = false;
    h.tick();
    FollowStatus s = h.status();
    TEST_ASSERT_EQUAL(FOLLOW_LOCK_IDLE, s.state);
    TEST_ASSERT_EQUAL_UINT32(0u, s.lockedUid);
    TEST_ASSERT_EQUAL_STRING("", s.lockedName);
}

void test_applyConfig_target_uid_change_forces_reacquire_mid_lock() {
    FollowHarness h;
    h.setPeer(/*uid=*/1, 1.0, 1.0, 0.0, 0.0);
    h.setPeer(/*uid=*/5, 2.0, 2.0, 0.0, 0.0);
    h.fc.gcsNav = true;

    h.tick();
    TEST_ASSERT_EQUAL(FOLLOW_LOCK_LOCKED, h.status().state);

    FollowConfig cfg = configOf(h);
    cfg.targetUid = 5;  // was 0 (FIRST_ACTIVE), now pinned elsewhere
    const char* err = nullptr;
    TEST_ASSERT_TRUE(h.apply(cfg, &err));

    // forceReacquire() fires synchronously inside applyConfig() -- no
    // additional service() cycle needed to observe the reset.
    FollowStatus s = h.status();
    TEST_ASSERT_EQUAL(FOLLOW_LOCK_ACQUIRING, s.state);
    TEST_ASSERT_EQUAL_UINT32(0u, s.lockedUid);
}

void test_announce_only_peer_is_not_followable() {
    FollowHarness h;
    // An announce creates the peer (name, capabilities) but carries no
    // position, so last_position_ms stays at 0 -- older than any timeout.
    h.namePeer(/*uid=*/1, "ABC");
    h.fc.gcsNav = true;

    h.tick();
    FollowStatus s = h.status();
    TEST_ASSERT_EQUAL(FOLLOW_LOCK_ACQUIRING, s.state);
    TEST_ASSERT_EQUAL_UINT32(0u, s.lockedUid);
    TEST_ASSERT_EQUAL_size_t(0, h.fc.sentWaypoints.size());
}

void test_no_own_fix_suppresses_waypoint_but_keeps_lock() {
    FollowHarness h;
    h.setPeer(/*uid=*/1, 1.0, 1.0, 0.0, 0.0);
    h.fc.gcsNav = true;
    // h.self is deliberately never set(): our own fix is invalid.

    h.tick();
    FollowStatus s = h.status();
    TEST_ASSERT_EQUAL(FOLLOW_LOCK_LOCKED, s.state);  // lock is kept...
    TEST_ASSERT_EQUAL_UINT32(1u, s.lockedUid);
    TEST_ASSERT_EQUAL_size_t(0, h.fc.sentWaypoints.size());  // ...but nothing goes out
    TEST_ASSERT_FALSE(s.haveLastTarget);

    // Own fix arrives: the very next cycle emits.
    h.self.set(1.0, 1.0);
    h.tick();
    TEST_ASSERT_EQUAL(FOLLOW_LOCK_LOCKED, h.status().state);
    TEST_ASSERT_EQUAL_size_t(1, h.fc.sentWaypoints.size());
    TEST_ASSERT_TRUE(h.status().haveLastTarget);
}

void test_telemetry_needs_follow_gate_and_rc_assignment() {
    FollowHarness h;

    // Gate off, no RC axis or autothrottle channel assigned: an idle Follow
    // asks for no polled telemetry at all.
    h.fc.gcsNav = false;
    h.tick();
    TEST_ASSERT_FALSE(h.fc.needAltitude);
    TEST_ASSERT_FALSE(h.fc.needRc);

    // Gate on: altitude is needed (for the commanded home-relative alt), RC
    // still isn't.
    h.fc.gcsNav = true;
    h.tick();
    TEST_ASSERT_TRUE(h.fc.needAltitude);
    TEST_ASSERT_FALSE(h.fc.needRc);

    // Assigning an RC axis channel turns the RC need on at the next cycle.
    FollowConfig cfg = configOf(h);
    cfg.rcLongChannel = 5;
    const char* err = nullptr;
    TEST_ASSERT_TRUE(h.apply(cfg, &err));
    h.tick();
    TEST_ASSERT_TRUE(h.fc.needAltitude);
    TEST_ASSERT_TRUE(h.fc.needRc);
}
