#include <unity.h>

#include <cstring>

#include "hw_layout.h"
#include "lora_band.h"

using namespace ff;

void setUp() {}
void tearDown() {}

// What the build-time static_asserts used to guarantee, now that the band is a
// runtime choice: every preset sits inside its allocation, and the 2.4 GHz one
// meets the 500 kHz minimum for a fixed channel.
void test_every_preset_is_inside_its_band() {
    for (uint16_t band : kLoraBands) {
        LoraBand b;
        TEST_ASSERT_TRUE(loraBandPreset(band, b));
        TEST_ASSERT_EQUAL_UINT16(band, b.band);
        const char* err = nullptr;
        TEST_ASSERT_TRUE_MESSAGE(loraBandValid(b, &err), err);
    }
}

void test_unknown_band_has_no_preset() {
    LoraBand b;
    TEST_ASSERT_FALSE(loraBandPreset(0, b));
    TEST_ASSERT_FALSE(loraBandPreset(900, b));
    TEST_ASSERT_FALSE(b.valid());
}

void test_band_rules_travel_with_the_band() {
    LoraBand b;
    loraBandPreset(868, b);
    TEST_ASSERT_EQUAL_UINT8(10, b.duty_cycle_pct);
    TEST_ASSERT_EQUAL_INT8(27, b.max_dbm);
    loraBandPreset(433, b);
    TEST_ASSERT_EQUAL_INT8(10, b.max_dbm);
    loraBandPreset(915, b);
    TEST_ASSERT_EQUAL_UINT8(0, b.duty_cycle_pct);
}

void test_2400_is_lr_4_8_as_it_always_was_on_air() {
    LoraBand b;
    loraBandPreset(2400, b);
    TEST_ASSERT_EQUAL_UINT8(8, b.coding_rate);
    TEST_ASSERT_TRUE(b.long_interleaving);
    TEST_ASSERT_EQUAL_UINT16(12, b.preamble_symbols);
}

void test_a_channel_outside_its_band_is_refused() {
    LoraBand b;
    loraBandPreset(2400, b);
    b.frequency_hz = 2400000000u;  // the old default: half the channel below the edge
    const char* err = nullptr;
    TEST_ASSERT_FALSE(loraBandValid(b, &err));
    loraBandPreset(2400, b);
    b.bandwidth_khz = 406.25f;
    TEST_ASSERT_FALSE(loraBandValid(b, &err));
}

void test_chip_bands() {
    TEST_ASSERT_TRUE(chipSupportsBand(LoraChip::SX127x, 433));
    TEST_ASSERT_TRUE(chipSupportsBand(LoraChip::SX127x, 915));
    TEST_ASSERT_FALSE(chipSupportsBand(LoraChip::SX127x, 2400));
    TEST_ASSERT_TRUE(chipSupportsBand(LoraChip::SX128x, 2400));
    TEST_ASSERT_FALSE(chipSupportsBand(LoraChip::SX128x, 868));
    for (uint16_t band : kLoraBands) {
        TEST_ASSERT_TRUE(chipSupportsBand(LoraChip::LR1121, band));
    }
}

void test_elrs_hardware_is_never_offered_433() {
    // Matched for 868/915 and 2.4 GHz; 433 would tune and go nowhere useful.
    TEST_ASSERT_FALSE(boardSupportsBand(LoraChip::SX127x, true, 433));
    TEST_ASSERT_FALSE(boardSupportsBand(LoraChip::LR1121, true, 433));
    TEST_ASSERT_TRUE(boardSupportsBand(LoraChip::SX127x, true, 868));
    TEST_ASSERT_TRUE(boardSupportsBand(LoraChip::LR1121, true, 2400));
    // A hand-written 433 target is a 433 board.
    TEST_ASSERT_TRUE(boardSupportsBand(LoraChip::SX127x, false, 433));
}

void test_setting_wins_then_flashed_default() {
    TEST_ASSERT_EQUAL_UINT16(868, resolveLoraBand(LoraChip::SX127x, true, 868, 915));
    TEST_ASSERT_EQUAL_UINT16(915, resolveLoraBand(LoraChip::SX127x, true, 0, 915));
}

void test_multi_band_board_with_no_choice_stays_off() {
    TEST_ASSERT_EQUAL_UINT16(0, resolveLoraBand(LoraChip::SX127x, true, 0, 0));
    TEST_ASSERT_EQUAL_UINT16(0, resolveLoraBand(LoraChip::LR1121, true, 0, 0));
}

void test_single_band_board_needs_no_choice() {
    TEST_ASSERT_EQUAL_UINT16(2400, resolveLoraBand(LoraChip::SX128x, true, 0, 0));
}

void test_a_band_the_board_cannot_run_counts_as_none() {
    // 433 asked of ELRS hardware falls back to the flashed default...
    TEST_ASSERT_EQUAL_UINT16(915, resolveLoraBand(LoraChip::SX127x, true, 433, 915));
    // ...or to nothing at all, never to 433.
    TEST_ASSERT_EQUAL_UINT16(0, resolveLoraBand(LoraChip::LR1121, true, 433, 0));
    // 2400 on an SX127x is not a thing.
    TEST_ASSERT_EQUAL_UINT16(868, resolveLoraBand(LoraChip::SX127x, false, 2400, 868));
    // A legacy 433 board keeps its 433.
    TEST_ASSERT_EQUAL_UINT16(433, resolveLoraBand(LoraChip::SX127x, false, 0, 433));
}

void test_stricter_duty_cycle() {
    TEST_ASSERT_EQUAL_UINT8(0, stricterDutyCycle(0, 0));
    TEST_ASSERT_EQUAL_UINT8(10, stricterDutyCycle(0, 10));
    TEST_ASSERT_EQUAL_UINT8(10, stricterDutyCycle(10, 0));
    TEST_ASSERT_EQUAL_UINT8(1, stricterDutyCycle(1, 10));
    TEST_ASSERT_EQUAL_UINT8(10, stricterDutyCycle(50, 10));
}

void test_options_block_carries_the_band() {
    HardwareLayout hw;
    const char* opts = R"({"uid":[1,2,3,4,5,6],"lora-band":868,"wifi-ssid":"x"})";
    parseOptionsJson(opts, std::strlen(opts), hw);
    TEST_ASSERT_EQUAL_UINT16(868, hw.default_band);

    HardwareLayout untouched;
    untouched.default_band = 915;
    parseOptionsJson("{}", 2, untouched);
    TEST_ASSERT_EQUAL_UINT16(915, untouched.default_band);
    parseOptionsJson("not json", 8, untouched);
    TEST_ASSERT_EQUAL_UINT16(915, untouched.default_band);
}

void test_options_block_carries_first_boot_settings() {
    HardwareLayout hw;
    const char* opts =
        R"({"lora-band":915,"name":"LEAD","group-passphrase":"squadron seven"})";
    parseOptionsJson(opts, std::strlen(opts), hw);
    TEST_ASSERT_EQUAL_STRING("LEAD", hw.flashed_name);
    TEST_ASSERT_EQUAL_STRING("squadron seven", hw.flashed_passphrase);

    // Truncated to what Settings can hold, never overrun.
    HardwareLayout longer;
    const char* big = R"({"name":"a-name-far-longer-than-fifteen"})";
    parseOptionsJson(big, std::strlen(big), longer);
    TEST_ASSERT_EQUAL_size_t(15, std::strlen(longer.flashed_name));
}

void test_elrs_hardware_follows_the_layout_source() {
    HardwareLayout hw;
    TEST_ASSERT_FALSE(hw.elrsHardware());
    hw.source = LayoutSource::Compiled;
    TEST_ASSERT_FALSE(hw.elrsHardware());
    hw.source = LayoutSource::Trailer;
    TEST_ASSERT_TRUE(hw.elrsHardware());
    hw.source = LayoutSource::File;
    TEST_ASSERT_TRUE(hw.elrsHardware());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_every_preset_is_inside_its_band);
    RUN_TEST(test_unknown_band_has_no_preset);
    RUN_TEST(test_band_rules_travel_with_the_band);
    RUN_TEST(test_2400_is_lr_4_8_as_it_always_was_on_air);
    RUN_TEST(test_a_channel_outside_its_band_is_refused);
    RUN_TEST(test_chip_bands);
    RUN_TEST(test_elrs_hardware_is_never_offered_433);
    RUN_TEST(test_setting_wins_then_flashed_default);
    RUN_TEST(test_multi_band_board_with_no_choice_stays_off);
    RUN_TEST(test_single_band_board_needs_no_choice);
    RUN_TEST(test_a_band_the_board_cannot_run_counts_as_none);
    RUN_TEST(test_stricter_duty_cycle);
    RUN_TEST(test_options_block_carries_the_band);
    RUN_TEST(test_options_block_carries_first_boot_settings);
    RUN_TEST(test_elrs_hardware_follows_the_layout_source);
    return UNITY_END();
}
