#include <unity.h>

#include <cstring>

#include "hw_layout.h"

using namespace ff;

void setUp() {}
void tearDown() {}

namespace {

bool parse(const char* json, HardwareLayout& out, const char** err = nullptr) {
    return parseLayoutJson(json, std::strlen(json), out, err);
}

HardwareLayout parsed(const char* json) {
    HardwareLayout hw;
    TEST_ASSERT_TRUE(parse(json, hw));
    return hw;
}

}  // namespace

// The layouts below are verbatim from ExpressLRS/targets.

// RX/Generic 900.json (HappyModel ES900RX and two dozen others).
const char* kGeneric900 =
    R"({"serial_rx":3,"serial_tx":1,"radio_dio0":4,"radio_dio1":5,"radio_miso":12,)"
    R"("radio_mosi":13,"radio_nss":15,"radio_rst":2,"radio_sck":14,"power_min":0,)"
    R"("power_high":2,"power_max":2,"power_default":2,"power_control":0,)"
    R"("power_values":[120,124,127],"led":16,"button":0})";

// RX/Generic 2400.json (HappyModel EP1/EP2).
const char* kGeneric2400 =
    R"({"serial_rx":3,"serial_tx":1,"radio_busy":5,"radio_dio1":4,"radio_miso":12,)"
    R"("radio_mosi":13,"radio_nss":15,"radio_rst":2,"radio_sck":14,"power_min":0,)"
    R"("power_high":0,"power_max":0,"power_default":0,"power_control":0,)"
    R"("power_values":[13],"power_lna_gain":0,"led":16,"button":0})";

// RX/Generic 2400 PA.json (BetaFPV Nano): PA/LNA switched by the ESP.
const char* kGeneric2400Pa =
    R"({"serial_rx":3,"serial_tx":1,"radio_busy":5,"radio_dio1":4,"radio_miso":12,)"
    R"("radio_mosi":13,"radio_nss":15,"radio_rst":2,"radio_sck":14,"power_rxen":9,)"
    R"("power_txen":10,"power_lna_gain":12,"power_min":0,"power_high":3,"power_max":3,)"
    R"("power_default":3,"power_control":0,"power_values":[-10,-6,-3,1],"button":0,"led":16})";

// RX/Generic C3 LR1121 True Diversity.json, trimmed to the radio keys.
const char* kC3Lr1121Diversity =
    R"({"serial_rx":20,"serial_tx":21,"radio_busy":3,"radio_dio1":1,"radio_miso":5,)"
    R"("radio_mosi":4,"radio_nss":0,"radio_rst":2,"radio_sck":6,"radio_busy_2":8,)"
    R"("radio_dio1_2":9,"radio_nss_2":7,"power_min":0,"power_high":3,"power_max":3,)"
    R"("power_default":0,"power_control":0,"power_values":[12,16,19,22],)"
    R"("power_values_dual":[-10,-6,-3,1],"power_lna_gain":12,"radio_dcdc":true})";

void test_generic_900_pins() {
    HardwareLayout hw = parsed(kGeneric900);
    TEST_ASSERT_EQUAL(3, hw.serial_rx);
    TEST_ASSERT_EQUAL(1, hw.serial_tx);
    TEST_ASSERT_EQUAL(14, hw.radio_sck);
    TEST_ASSERT_EQUAL(12, hw.radio_miso);
    TEST_ASSERT_EQUAL(13, hw.radio_mosi);
    TEST_ASSERT_EQUAL(15, hw.radio[0].nss);
    TEST_ASSERT_EQUAL(2, hw.radio[0].rst);
    TEST_ASSERT_EQUAL(4, hw.radio[0].dio0);
    TEST_ASSERT_EQUAL(16, hw.led);
    TEST_ASSERT_EQUAL(0, hw.button);
    TEST_ASSERT_EQUAL(kPinNone, hw.radio[0].busy);
    TEST_ASSERT_EQUAL(1, hw.radioCount(LoraChip::SX127x));
    TEST_ASSERT_EQUAL(0, hw.radioCount(LoraChip::SX128x));
    TEST_ASSERT_FALSE(hw.radio_dcdc);
}

void test_default_is_10mw_not_the_elrs_default_level() {
    // ELRS defaults this board to 50 mW (power_default 2); FormationFlight to
    // 10 mW, level 0: 120 = 0x78, MaxPower 7, OutputPower 8 -> 2 + 8.
    HardwareLayout hw = parsed(kGeneric900);
    PowerChoice p = choosePower(hw, LoraChip::SX127x, false, 0, 30);
    TEST_ASSERT_EQUAL_INT8(0, p.level);
    TEST_ASSERT_EQUAL_INT8(10, p.chip_dbm);
    TEST_ASSERT_EQUAL_INT8(10, p.output_dbm);
}

void test_50mw_is_17dbm_from_127() {
    // 127 = 0x7F: MaxPower 7, OutputPower 15 -> 2 + 15.
    HardwareLayout hw = parsed(kGeneric900);
    PowerChoice p = choosePower(hw, LoraChip::SX127x, false, 17, 30);
    TEST_ASSERT_EQUAL_INT8(2, p.level);
    TEST_ASSERT_EQUAL_INT8(17, p.chip_dbm);
}

void test_request_rounds_down_to_a_level() {
    HardwareLayout hw = parsed(kGeneric900);
    // 15 dBm sits between 25 mW (14) and 50 mW (17): 25 mW, 124 = 0x7C.
    PowerChoice p = choosePower(hw, LoraChip::SX127x, false, 15, 30);
    TEST_ASSERT_EQUAL_INT8(1, p.level);
    TEST_ASSERT_EQUAL_INT8(14, p.chip_dbm);
    TEST_ASSERT_EQUAL_INT8(14, p.output_dbm);
}

void test_request_below_the_table_gets_power_min() {
    HardwareLayout hw = parsed(kGeneric900);
    PowerChoice p = choosePower(hw, LoraChip::SX127x, false, 1, 30);
    TEST_ASSERT_EQUAL_INT8(0, p.level);
    TEST_ASSERT_EQUAL_INT8(10, p.output_dbm);
}

void test_request_above_the_table_gets_power_max() {
    HardwareLayout hw = parsed(kGeneric900);
    PowerChoice p = choosePower(hw, LoraChip::SX127x, false, 30, 30);
    TEST_ASSERT_EQUAL_INT8(2, p.level);
}

void test_band_ceiling_caps_the_request() {
    // The 433 band's 10 dBm ceiling beats a request for 50 mW.
    HardwareLayout hw = parsed(kGeneric900);
    PowerChoice p = choosePower(hw, LoraChip::SX127x, false, 17, 10);
    TEST_ASSERT_EQUAL_INT8(0, p.level);
    TEST_ASSERT_EQUAL_INT8(10, p.output_dbm);
    TEST_ASSERT_EQUAL_INT8(10, p.chip_dbm);  // 120 = 0x78
}

void test_pa_board_reports_output_not_chip_power() {
    HardwareLayout hw = parsed(kGeneric2400Pa);
    TEST_ASSERT_EQUAL(9, hw.radio[0].rxen);
    TEST_ASSERT_EQUAL(10, hw.radio[0].txen);
    TEST_ASSERT_EQUAL_INT8(12, hw.power_lna_gain);
    PowerChoice p = choosePower(hw, LoraChip::SX128x, true, 20, 30);
    TEST_ASSERT_EQUAL_INT8(3, p.level);
    TEST_ASSERT_EQUAL_INT8(1, p.chip_dbm);    // into the PA
    TEST_ASSERT_EQUAL_INT8(20, p.output_dbm); // 100 mW out of it
}

void test_power_values_are_indexed_from_power_min() {
    // RX/Generic 900 PA.json: the table starts at level 3.
    HardwareLayout hw = parsed(
        R"({"radio_nss":15,"radio_rst":2,"radio_dio0":4,"power_min":3,"power_max":5,)"
        R"("power_default":5,"power_values":[114,119,124]})");
    // Nothing on this board goes below 100 mW, so that is the default.
    PowerChoice p = choosePower(hw, LoraChip::SX127x, false, 0, 30);
    TEST_ASSERT_EQUAL_INT8(3, p.level);
    TEST_ASSERT_EQUAL_INT8(2 + 2, p.chip_dbm);   // 114 = 0x72
    TEST_ASSERT_EQUAL_INT8(20, p.output_dbm);    // 100 mW
    p = choosePower(hw, LoraChip::SX127x, false, 27, 30);
    TEST_ASSERT_EQUAL_INT8(5, p.level);
    TEST_ASSERT_EQUAL_INT8(2 + 12, p.chip_dbm);  // 124 = 0x7C
    TEST_ASSERT_EQUAL_INT8(27, p.output_dbm);    // 500 mW
    p = choosePower(hw, LoraChip::SX127x, false, 24, 30);
    TEST_ASSERT_EQUAL_INT8(4, p.level);
    TEST_ASSERT_EQUAL_INT8(2 + 7, p.chip_dbm);   // 119 = 0x77
}

void test_lr1121_diversity_layout() {
    HardwareLayout hw = parsed(kC3Lr1121Diversity);
    TEST_ASSERT_EQUAL(2, hw.radioCount(LoraChip::LR1121));
    TEST_ASSERT_EQUAL(7, hw.radio[1].nss);
    TEST_ASSERT_EQUAL(9, hw.radio[1].dio1);
    TEST_ASSERT_EQUAL(8, hw.radio[1].busy);
    TEST_ASSERT_EQUAL(kPinNone, hw.radio[1].rst);  // shares radio 1's
    TEST_ASSERT_TRUE(hw.radio_dcdc);
    TEST_ASSERT_EQUAL(0, hw.radio_rfsw_ctrl_count);
}

void test_lr1121_uses_the_dual_table_at_2400() {
    HardwareLayout hw = parsed(kC3Lr1121Diversity);
    PowerChoice sub = choosePower(hw, LoraChip::LR1121, false, 20, 30);
    TEST_ASSERT_EQUAL_INT8(22, sub.chip_dbm);
    PowerChoice hf = choosePower(hw, LoraChip::LR1121, true, 20, 30);
    TEST_ASSERT_EQUAL_INT8(1, hf.chip_dbm);
    TEST_ASSERT_EQUAL_INT8(20, hf.output_dbm);
}

void test_rfsw_ctrl_is_kept_verbatim() {
    // RX/Radiomaster ER Series.json
    HardwareLayout hw = parsed(R"({"radio_rfsw_ctrl":[15,4,4,0,0,14,0,13]})");
    TEST_ASSERT_EQUAL(8, hw.radio_rfsw_ctrl_count);
    TEST_ASSERT_EQUAL(15, hw.radio_rfsw_ctrl[0]);
    TEST_ASSERT_EQUAL(14, hw.radio_rfsw_ctrl[5]);
    TEST_ASSERT_EQUAL(13, hw.radio_rfsw_ctrl[7]);
}

void test_no_table_uses_compiled_dbm_and_the_request() {
    HardwareLayout hw;
    hw.has_power = true;
    hw.power_dbm = 10;
    PowerChoice p = choosePower(hw, LoraChip::SX127x, false, 0, 30);
    TEST_ASSERT_EQUAL_INT8(-1, p.level);
    TEST_ASSERT_EQUAL_INT8(10, p.chip_dbm);
    p = choosePower(hw, LoraChip::SX127x, false, 14, 30);
    TEST_ASSERT_EQUAL_INT8(14, p.chip_dbm);
    // Clamped to what the part takes, then to the band.
    p = choosePower(hw, LoraChip::SX127x, false, 30, 30);
    TEST_ASSERT_EQUAL_INT8(17, p.chip_dbm);
    p = choosePower(hw, LoraChip::SX128x, true, 20, 30);
    TEST_ASSERT_EQUAL_INT8(13, p.chip_dbm);
    p = choosePower(hw, LoraChip::SX127x, false, 14, 10);
    TEST_ASSERT_EQUAL_INT8(10, p.chip_dbm);
}

void test_rfo_hf_drops_the_boost_offset() {
    TEST_ASSERT_EQUAL_INT8(17, elrsPowerToDbm(LoraChip::SX127x, 127, false));
    TEST_ASSERT_EQUAL_INT8(15, elrsPowerToDbm(LoraChip::SX127x, 127, true));
    TEST_ASSERT_EQUAL_INT8(-10, elrsPowerToDbm(LoraChip::SX128x, -10, false));
    TEST_ASSERT_EQUAL_INT8(22, elrsPowerToDbm(LoraChip::LR1121, 22, false));
}

void test_short_table_is_trusted_over_power_max() {
    HardwareLayout hw =
        parsed(R"({"power_min":0,"power_max":5,"power_default":5,"power_values":[120,124]})");
    PowerChoice p = choosePower(hw, LoraChip::SX127x, false, 30, 30);
    TEST_ASSERT_EQUAL_INT8(1, p.level);
    TEST_ASSERT_EQUAL_INT8(14, p.chip_dbm);
}

void test_absent_keys_keep_compiled_defaults() {
    HardwareLayout hw;
    hw.serial_rx = 32;
    hw.power_dbm = 10;
    hw.has_power = true;
    TEST_ASSERT_TRUE(parse(R"({"led":2})", hw));
    TEST_ASSERT_EQUAL(2, hw.led);
    TEST_ASSERT_EQUAL(32, hw.serial_rx);
    TEST_ASSERT_EQUAL_INT8(10, hw.power_dbm);
    TEST_ASSERT_FALSE(hw.has_power_table);
}

void test_unused_keys_are_ignored() {
    // Real layouts carry LED tables, PWM outputs and comments the firmware has
    // no use for; none of them may stop the parse.
    HardwareLayout hw = parsed(
        R"({"//":"LED","led_rgb":2,"ledidx_rgb_status":[0,1],"pwm_outputs":[0,1,2],)"
        R"("radio_nss":27,"vbat_scale":902})");
    TEST_ASSERT_EQUAL(27, hw.radio[0].nss);
    TEST_ASSERT_EQUAL(kPinNone, hw.led);
}

void test_malformed_json_leaves_layout_untouched() {
    HardwareLayout hw;
    hw.radio[0].nss = 18;
    const char* err = nullptr;
    TEST_ASSERT_FALSE(parse(R"({"radio_nss":15,)", hw, &err));
    TEST_ASSERT_NOT_NULL(err);
    TEST_ASSERT_EQUAL(18, hw.radio[0].nss);
}

void test_non_object_is_rejected() {
    HardwareLayout hw;
    TEST_ASSERT_FALSE(parse("[1,2,3]", hw));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_generic_900_pins);
    RUN_TEST(test_default_is_10mw_not_the_elrs_default_level);
    RUN_TEST(test_50mw_is_17dbm_from_127);
    RUN_TEST(test_request_rounds_down_to_a_level);
    RUN_TEST(test_request_below_the_table_gets_power_min);
    RUN_TEST(test_request_above_the_table_gets_power_max);
    RUN_TEST(test_band_ceiling_caps_the_request);
    RUN_TEST(test_pa_board_reports_output_not_chip_power);
    RUN_TEST(test_power_values_are_indexed_from_power_min);
    RUN_TEST(test_lr1121_diversity_layout);
    RUN_TEST(test_lr1121_uses_the_dual_table_at_2400);
    RUN_TEST(test_rfsw_ctrl_is_kept_verbatim);
    RUN_TEST(test_no_table_uses_compiled_dbm_and_the_request);
    RUN_TEST(test_rfo_hf_drops_the_boost_offset);
    RUN_TEST(test_short_table_is_trusted_over_power_max);
    RUN_TEST(test_absent_keys_keep_compiled_defaults);
    RUN_TEST(test_unused_keys_are_ignored);
    RUN_TEST(test_malformed_json_leaves_layout_untouched);
    RUN_TEST(test_non_object_is_rejected);
    return UNITY_END();
}
