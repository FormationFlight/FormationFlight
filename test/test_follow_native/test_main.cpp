// Single Unity entry point for the test_follow_native suite. PlatformIO
// compiles every .cpp in this directory into one binary, so setUp()/
// tearDown()/main() must be defined exactly once -- the other test_*.cpp
// files here define only plain (non-static) test functions, declared
// extern below and registered with RUN_TEST().

#include <unity.h>

void setUp() {}
void tearDown() {}

// test_harness_smoke.cpp
extern void test_follow_manager_constructs_with_fakes();

// test_slot_geometry.cpp
extern void test_ahead_at_course_zero_is_due_north();
extern void test_behind_at_course_zero_is_due_south();
extern void test_right_at_course_zero_is_due_east();
extern void test_left_at_course_zero_is_due_west();
extern void test_behind_at_course_90_is_due_west();
extern void test_behind_at_course_270_is_due_east();
extern void test_ahead_bearing_wraps_near_360();
extern void test_round_trip_ahead();
extern void test_round_trip_combined_offset_nontrivial_course();

// test_geometry_sane.cpp
extern void test_geometry_sane_at_exactly_minSepM_passes();
extern void test_geometry_sane_just_under_minSepM_fails();
extern void test_non_stacked_slot_ignores_minVSepM();
extern void test_stacked_slot_under_minVSepM_fails();
extern void test_stacked_slot_at_exactly_minVSepM_passes();
extern void test_applyConfig_rejects_geometrically_insane_static_offset();

// test_rc_safety_net.cpp
extern void test_layer1_rejects_candidate_failing_geometry_alone();
extern void test_layer2_rejects_sign_flip_when_other_axes_below_minSepM();
extern void test_layer2_allows_sign_flip_when_other_axes_above_minSepM();
extern void test_zero_on_candidate_side_never_counts_as_crossed();
extern void test_zero_on_reference_side_never_counts_as_crossed();

// test_peer_lock.cpp
extern void test_fresh_manager_stays_acquiring_with_no_peers();
extern void test_fresh_manager_locks_within_one_cycle_once_peer_exists();
extern void test_target_peer_zero_locks_first_active_in_iteration_order();
extern void test_target_peer_pinned_ignores_other_live_peers();
extern void test_locked_peer_going_stale_enters_locked_holding_and_keeps_id();
extern void test_locked_holding_peer_returning_with_same_name_relocks();
extern void test_locked_holding_id_reused_by_different_aircraft_does_not_relock();
extern void test_gate_inactive_mid_lock_forces_idle_and_clears_lock();
extern void test_applyConfig_target_peer_change_forces_reacquire_mid_lock();

// test_altitude_floor.cpp
extern void test_altitude_above_floor_is_not_clamped();
extern void test_altitude_below_floor_is_clamped_but_still_emitted();
extern void test_floor_clamp_not_attributable_to_rc_reports_floor_clamped_condition();
extern void test_floor_clamp_attributable_to_rc_reports_rc_invalid_gap_condition();
extern void test_target_within_max_dist_is_emitted_normally();
extern void test_target_beyond_max_dist_suppresses_waypoint_but_keeps_lock();

// test_heading_and_course.cpp
extern void test_heading_off_never_sends_set_head_even_with_heading_hold_active();
extern void test_heading_course_returns_course_deg_wrapped();
extern void test_heading_fixed_ignores_course_deg();
extern void test_heading_course_relative_adds_offset_and_wraps();
extern void test_heading_point_leader_bears_toward_peer_position();
extern void test_heading_zero_collision_remaps_to_one();
extern void test_nonzero_heading_skips_send_set_head_when_heading_hold_inactive();
extern void test_course_above_threshold_uses_live_ground_course();
extern void test_course_dropping_below_threshold_holds_last_valid_course();
extern void test_course_below_threshold_from_first_cycle_falls_back_to_reported_value();

// test_rc_axis_and_prearm.cpp
extern void test_axis_offset_no_channel_assigned_returns_configured_unchanged();
extern void test_axis_offset_msp_read_failure_falls_back_to_configured();
extern void test_axis_offset_center_maps_to_zero();
extern void test_axis_offset_full_deflection_maps_to_plus_and_minus_gap();
extern void test_axis_offset_out_of_range_us_clamps_to_nearest_endpoint();
extern void test_prearm_candidate_computed_every_cycle_while_disarmed_with_axis_assigned();
extern void test_prearm_forced_false_and_absent_while_armed_regardless_of_assignment();
extern void test_prearm_failed_flag_is_not_sticky_across_arm_transition();
extern void test_prearm_center_with_nonzero_default_fails_check();
extern void test_prearm_matching_static_default_passes_check();

// test_autothrottle.cpp
extern void test_engage_gate_false_when_not_locked();
extern void test_engage_gate_false_when_locked_but_not_airplane();
extern void test_engage_gate_false_when_arm_channel_outside_range();
extern void test_engage_gate_true_when_all_three_conditions_met();
extern void test_engage_drops_immediately_when_one_condition_flips_false();
extern void test_zero_accel_is_pure_feedforward();
extern void test_positive_error_adds_correction_negative_subtracts();
extern void test_target_speed_clamps_to_max_and_min();
extern void test_autothrottle_armed_when_channel_unassigned();
extern void test_autothrottle_armed_when_assigned_channel_read_fails();
extern void test_autothrottle_armed_only_within_threshold_range();

// test_gvar_reporting.cpp
extern void test_gvar_first_cycle_always_sends_even_when_value_is_zero();
extern void test_gvar_resends_immediately_when_value_changes();
extern void test_gvar_unchanged_value_not_resent_before_heartbeat();
extern void test_gvar_unchanged_value_resent_after_heartbeat_elapses();
extern void test_gvar_index_minus_one_never_sends();
extern void test_condition_code_priority_highest_value_wins_not_first_computed();

// test_config_and_eeprom.cpp
extern void test_applyConfig_validation_rules_table();
extern void test_rejected_applyConfig_leaves_live_config_untouched();
extern void test_eeprom_round_trip_preserves_fields_with_documented_rounding();
extern void test_eeprom_version_mismatch_falls_back_to_defaults();
extern void test_eeprom_save_rate_limited_second_call_fails_first_persists();
extern void test_configJson_emits_every_documented_field();
extern void test_statusJson_conditional_fields_present_and_absent_as_documented();

// test_cross_mirror_fixture.cpp
extern void test_applyConfig_matches_every_fixture_case();

int main(int argc, char **argv)
{
    UNITY_BEGIN();

    RUN_TEST(test_follow_manager_constructs_with_fakes);

    RUN_TEST(test_ahead_at_course_zero_is_due_north);
    RUN_TEST(test_behind_at_course_zero_is_due_south);
    RUN_TEST(test_right_at_course_zero_is_due_east);
    RUN_TEST(test_left_at_course_zero_is_due_west);
    RUN_TEST(test_behind_at_course_90_is_due_west);
    RUN_TEST(test_behind_at_course_270_is_due_east);
    RUN_TEST(test_ahead_bearing_wraps_near_360);
    RUN_TEST(test_round_trip_ahead);
    RUN_TEST(test_round_trip_combined_offset_nontrivial_course);

    RUN_TEST(test_geometry_sane_at_exactly_minSepM_passes);
    RUN_TEST(test_geometry_sane_just_under_minSepM_fails);
    RUN_TEST(test_non_stacked_slot_ignores_minVSepM);
    RUN_TEST(test_stacked_slot_under_minVSepM_fails);
    RUN_TEST(test_stacked_slot_at_exactly_minVSepM_passes);
    RUN_TEST(test_applyConfig_rejects_geometrically_insane_static_offset);

    RUN_TEST(test_layer1_rejects_candidate_failing_geometry_alone);
    RUN_TEST(test_layer2_rejects_sign_flip_when_other_axes_below_minSepM);
    RUN_TEST(test_layer2_allows_sign_flip_when_other_axes_above_minSepM);
    RUN_TEST(test_zero_on_candidate_side_never_counts_as_crossed);
    RUN_TEST(test_zero_on_reference_side_never_counts_as_crossed);

    RUN_TEST(test_fresh_manager_stays_acquiring_with_no_peers);
    RUN_TEST(test_fresh_manager_locks_within_one_cycle_once_peer_exists);
    RUN_TEST(test_target_peer_zero_locks_first_active_in_iteration_order);
    RUN_TEST(test_target_peer_pinned_ignores_other_live_peers);
    RUN_TEST(test_locked_peer_going_stale_enters_locked_holding_and_keeps_id);
    RUN_TEST(test_locked_holding_peer_returning_with_same_name_relocks);
    RUN_TEST(test_locked_holding_id_reused_by_different_aircraft_does_not_relock);
    RUN_TEST(test_gate_inactive_mid_lock_forces_idle_and_clears_lock);
    RUN_TEST(test_applyConfig_target_peer_change_forces_reacquire_mid_lock);

    RUN_TEST(test_altitude_above_floor_is_not_clamped);
    RUN_TEST(test_altitude_below_floor_is_clamped_but_still_emitted);
    RUN_TEST(test_floor_clamp_not_attributable_to_rc_reports_floor_clamped_condition);
    RUN_TEST(test_floor_clamp_attributable_to_rc_reports_rc_invalid_gap_condition);
    RUN_TEST(test_target_within_max_dist_is_emitted_normally);
    RUN_TEST(test_target_beyond_max_dist_suppresses_waypoint_but_keeps_lock);

    RUN_TEST(test_heading_off_never_sends_set_head_even_with_heading_hold_active);
    RUN_TEST(test_heading_course_returns_course_deg_wrapped);
    RUN_TEST(test_heading_fixed_ignores_course_deg);
    RUN_TEST(test_heading_course_relative_adds_offset_and_wraps);
    RUN_TEST(test_heading_point_leader_bears_toward_peer_position);
    RUN_TEST(test_heading_zero_collision_remaps_to_one);
    RUN_TEST(test_nonzero_heading_skips_send_set_head_when_heading_hold_inactive);
    RUN_TEST(test_course_above_threshold_uses_live_ground_course);
    RUN_TEST(test_course_dropping_below_threshold_holds_last_valid_course);
    RUN_TEST(test_course_below_threshold_from_first_cycle_falls_back_to_reported_value);

    RUN_TEST(test_axis_offset_no_channel_assigned_returns_configured_unchanged);
    RUN_TEST(test_axis_offset_msp_read_failure_falls_back_to_configured);
    RUN_TEST(test_axis_offset_center_maps_to_zero);
    RUN_TEST(test_axis_offset_full_deflection_maps_to_plus_and_minus_gap);
    RUN_TEST(test_axis_offset_out_of_range_us_clamps_to_nearest_endpoint);
    RUN_TEST(test_prearm_candidate_computed_every_cycle_while_disarmed_with_axis_assigned);
    RUN_TEST(test_prearm_forced_false_and_absent_while_armed_regardless_of_assignment);
    RUN_TEST(test_prearm_failed_flag_is_not_sticky_across_arm_transition);
    RUN_TEST(test_prearm_center_with_nonzero_default_fails_check);
    RUN_TEST(test_prearm_matching_static_default_passes_check);

    RUN_TEST(test_engage_gate_false_when_not_locked);
    RUN_TEST(test_engage_gate_false_when_locked_but_not_airplane);
    RUN_TEST(test_engage_gate_false_when_arm_channel_outside_range);
    RUN_TEST(test_engage_gate_true_when_all_three_conditions_met);
    RUN_TEST(test_engage_drops_immediately_when_one_condition_flips_false);
    RUN_TEST(test_zero_accel_is_pure_feedforward);
    RUN_TEST(test_positive_error_adds_correction_negative_subtracts);
    RUN_TEST(test_target_speed_clamps_to_max_and_min);
    RUN_TEST(test_autothrottle_armed_when_channel_unassigned);
    RUN_TEST(test_autothrottle_armed_when_assigned_channel_read_fails);
    RUN_TEST(test_autothrottle_armed_only_within_threshold_range);

    RUN_TEST(test_gvar_first_cycle_always_sends_even_when_value_is_zero);
    RUN_TEST(test_gvar_resends_immediately_when_value_changes);
    RUN_TEST(test_gvar_unchanged_value_not_resent_before_heartbeat);
    RUN_TEST(test_gvar_unchanged_value_resent_after_heartbeat_elapses);
    RUN_TEST(test_gvar_index_minus_one_never_sends);
    RUN_TEST(test_condition_code_priority_highest_value_wins_not_first_computed);

    RUN_TEST(test_applyConfig_validation_rules_table);
    RUN_TEST(test_rejected_applyConfig_leaves_live_config_untouched);
    RUN_TEST(test_eeprom_round_trip_preserves_fields_with_documented_rounding);
    RUN_TEST(test_eeprom_version_mismatch_falls_back_to_defaults);
    RUN_TEST(test_eeprom_save_rate_limited_second_call_fails_first_persists);
    RUN_TEST(test_configJson_emits_every_documented_field);
    RUN_TEST(test_statusJson_conditional_fields_present_and_absent_as_documented);

    RUN_TEST(test_applyConfig_matches_every_fixture_case);

    return UNITY_END();
}
