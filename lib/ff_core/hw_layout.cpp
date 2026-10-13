#include "hw_layout.h"

#include <ArduinoJson.h>

#include <cstdio>

namespace ff {

const char* layoutSourceName(LayoutSource s) {
    switch (s) {
        case LayoutSource::Compiled:
            return "compiled";
        case LayoutSource::Trailer:
            return "firmware";
        case LayoutSource::File:
            return "hardware.json";
        case LayoutSource::None:
        default:
            return "none";
    }
}

namespace {
constexpr int8_t kLevelDbm[kPowerLevels] = {10, 14, 17, 20, 24, 27, 30, 33};
constexpr uint16_t kLevelMw[kPowerLevels] = {10, 25, 50, 100, 250, 500, 1000, 2000};

int clampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
}  // namespace

int8_t powerLevelDbm(int level) { return kLevelDbm[clampInt(level, 0, kPowerLevels - 1)]; }
uint16_t powerLevelMw(int level) { return kLevelMw[clampInt(level, 0, kPowerLevels - 1)]; }

bool HardwareLayout::hasRadio(LoraChip chip, int index) const {
    const RadioPins& r = radio[index];
    // The second radio usually shares the first one's reset line, and ELRS
    // layouts then leave radio_rst_2 out.
    if (r.nss == kPinNone || (index == 0 && r.rst == kPinNone)) {
        return false;
    }
    if (chip == LoraChip::SX127x) {
        return r.dio0 != kPinNone;
    }
    return r.dio1 != kPinNone && r.busy != kPinNone;
}

int HardwareLayout::radioCount(LoraChip chip) const {
    if (!hasRadio(chip, 0)) {
        return 0;
    }
    return hasRadio(chip, 1) ? 2 : 1;
}

int8_t elrsPowerToDbm(LoraChip chip, int value, bool rfo_hf) {
    if (chip != LoraChip::SX127x) {
        return static_cast<int8_t>(value);
    }
    const int output_power = value & 0x0F;
    return static_cast<int8_t>(rfo_hf ? output_power : 2 + output_power);
}

namespace {

// What each part accepts, for a target with no table to keep it in range.
void chipRange(LoraChip chip, bool high_band, bool rfo_hf, int& lo, int& hi) {
    switch (chip) {
        case LoraChip::SX127x:
            lo = rfo_hf ? 0 : 2;
            hi = rfo_hf ? 15 : 17;
            return;
        case LoraChip::SX128x:
            lo = -18;
            hi = 13;
            return;
        case LoraChip::LR1121:
            if (high_band) {
                lo = -18;
                hi = 13;
            } else if (rfo_hf) {
                lo = -17;
                hi = 14;
            } else {
                lo = -9;
                hi = 22;
            }
            return;
    }
}

}  // namespace

PowerChoice choosePower(const HardwareLayout& hw, LoraChip chip, bool high_band,
                        int requested_dbm, int ceiling_dbm) {
    PowerChoice c;

    const bool dual = chip == LoraChip::LR1121 && high_band && hw.power_values_dual_count > 0;
    const int16_t* values = dual ? hw.power_values_dual : hw.power_values;
    const int count = dual ? hw.power_values_dual_count : hw.power_values_count;

    if (!hw.has_power_table || count == 0) {
        int lo, hi;
        chipRange(chip, high_band, hw.radio_rfo_hf, lo, hi);
        int dbm = requested_dbm != 0 ? requested_dbm
                                     : (hw.has_power ? hw.power_dbm : kDefaultLoraDbm);
        if (dbm > ceiling_dbm) {
            dbm = ceiling_dbm;
        }
        c.chip_dbm = static_cast<int8_t>(clampInt(dbm, lo, hi));
        c.output_dbm = c.chip_dbm;
        return c;
    }

    // The levels this table actually covers. A table shorter than
    // power_max - power_min + 1 is a broken layout; trust the table.
    const int min = hw.power_min;
    const int max = clampInt(hw.power_max, min, min + count - 1);

    const int target = requested_dbm != 0 ? requested_dbm : kDefaultLoraDbm;
    int level = min;
    for (int l = min; l <= max; ++l) {
        if (powerLevelDbm(l) <= target) {
            level = l;
        }
    }
    // The band ceiling wins over both the default and the request. If even
    // power_min is above it there is nothing lower to choose, and the level
    // reported says so plainly rather than pretending.
    while (level > min && powerLevelDbm(level) > ceiling_dbm) {
        --level;
    }

    c.level = static_cast<int8_t>(level);
    c.output_dbm = powerLevelDbm(level);
    c.chip_dbm = elrsPowerToDbm(chip, values[level - min], hw.radio_rfo_hf);
    return c;
}

namespace {

void readPin(JsonDocument& doc, const char* key, int& out) {
    JsonVariant v = doc[key];
    if (v.is<int>()) {
        out = v.as<int>();
    }
}

uint8_t readTable(JsonDocument& doc, const char* key, int16_t* out, uint8_t capacity) {
    JsonArray a = doc[key].as<JsonArray>();
    uint8_t n = 0;
    for (JsonVariant v : a) {
        if (n == capacity) {
            break;
        }
        out[n++] = static_cast<int16_t>(v.as<int>());
    }
    return n;
}

}  // namespace

bool parseLayoutJson(const char* json, size_t len, HardwareLayout& out, const char** err) {
    // Only the keys the firmware uses. A full ELRS layout can be 2 KB of LED
    // indexes and PWM tables, and parsing all of it on an ESP8285 at boot would
    // cost a heap allocation several times the size of what is kept.
    //
    // Both documents live on the heap only for the parse: on an ESP8285 the
    // stack is 4 KB. Sized for a 64-bit host too, where every slot is larger.
    DynamicJsonDocument filter(2048);
    for (const char* key :
         {"serial_rx", "serial_tx", "radio_sck", "radio_miso", "radio_mosi", "radio_nss",
          "radio_rst", "radio_dio0", "radio_dio1", "radio_busy", "radio_nss_2", "radio_rst_2",
          "radio_dio0_2", "radio_dio1_2", "radio_busy_2", "radio_rfo_hf", "radio_dcdc",
          "radio_tcxo", "radio_rfsw_ctrl", "power_enable", "power_rxen", "power_txen",
          "power_rxen_2", "power_txen_2", "power_lna_gain", "ant_ctrl", "led", "button",
          "power_min", "power_max", "power_default", "power_values", "power_values_dual"}) {
        filter[key] = true;
    }

    DynamicJsonDocument doc(3072);
    DeserializationError e =
        deserializeJson(doc, json, len, DeserializationOption::Filter(filter));
    if (e) {
        if (err != nullptr) {
            *err = e.c_str();
        }
        return false;
    }
    if (filter.overflowed() || doc.overflowed()) {
        // A silently truncated layout is worse than none: it would come up
        // with a radio missing its second chip, or a PA with no table.
        if (err != nullptr) {
            *err = "layout too large to parse";
        }
        return false;
    }
    if (!doc.is<JsonObject>()) {
        if (err != nullptr) {
            *err = "layout is not a JSON object";
        }
        return false;
    }

    readPin(doc, "serial_rx", out.serial_rx);
    readPin(doc, "serial_tx", out.serial_tx);
    readPin(doc, "radio_sck", out.radio_sck);
    readPin(doc, "radio_miso", out.radio_miso);
    readPin(doc, "radio_mosi", out.radio_mosi);

    readPin(doc, "radio_nss", out.radio[0].nss);
    readPin(doc, "radio_rst", out.radio[0].rst);
    readPin(doc, "radio_dio0", out.radio[0].dio0);
    readPin(doc, "radio_dio1", out.radio[0].dio1);
    readPin(doc, "radio_busy", out.radio[0].busy);
    readPin(doc, "power_rxen", out.radio[0].rxen);
    readPin(doc, "power_txen", out.radio[0].txen);

    readPin(doc, "radio_nss_2", out.radio[1].nss);
    readPin(doc, "radio_rst_2", out.radio[1].rst);
    readPin(doc, "radio_dio0_2", out.radio[1].dio0);
    readPin(doc, "radio_dio1_2", out.radio[1].dio1);
    readPin(doc, "radio_busy_2", out.radio[1].busy);
    readPin(doc, "power_rxen_2", out.radio[1].rxen);
    readPin(doc, "power_txen_2", out.radio[1].txen);

    readPin(doc, "power_enable", out.power_enable);
    readPin(doc, "ant_ctrl", out.ant_ctrl);
    readPin(doc, "radio_tcxo", out.radio_tcxo);
    readPin(doc, "led", out.led);
    readPin(doc, "button", out.button);

    if (doc["radio_rfo_hf"].is<bool>()) {
        out.radio_rfo_hf = doc["radio_rfo_hf"].as<bool>();
    }
    // Absent means LDO in ELRS, so an ELRS layout always decides this one,
    // unlike every other key. A board without the inductor that is switched to
    // DC-DC does not work at all.
    out.radio_dcdc = doc["radio_dcdc"] | false;
    if (doc["power_lna_gain"].is<int>()) {
        out.power_lna_gain = static_cast<int8_t>(doc["power_lna_gain"].as<int>());
    }

    JsonArray rfsw = doc["radio_rfsw_ctrl"].as<JsonArray>();
    if (!rfsw.isNull()) {
        out.radio_rfsw_ctrl_count = 0;
        for (JsonVariant v : rfsw) {
            if (out.radio_rfsw_ctrl_count == sizeof(out.radio_rfsw_ctrl)) {
                break;
            }
            out.radio_rfsw_ctrl[out.radio_rfsw_ctrl_count++] = static_cast<uint8_t>(v.as<int>());
        }
    }

    const uint8_t n = readTable(doc, "power_values", out.power_values, kPowerLevels);
    if (n > 0) {
        out.power_values_count = n;
        out.power_values_dual_count =
            readTable(doc, "power_values_dual", out.power_values_dual, kPowerLevels);
        out.power_min = static_cast<uint8_t>(clampInt(doc["power_min"] | 0, 0, kPowerLevels - 1));
        out.power_max = static_cast<uint8_t>(
            clampInt(doc["power_max"] | static_cast<int>(out.power_min), out.power_min,
                     kPowerLevels - 1));
        out.power_default = static_cast<uint8_t>(
            clampInt(doc["power_default"] | static_cast<int>(out.power_min), out.power_min,
                     out.power_max));
        out.has_power_table = true;
    }
    return true;
}

void parseOptionsJson(const char* json, size_t len, HardwareLayout& out) {
    StaticJsonDocument<128> filter;
    filter["lora-band"] = true;
    filter["name"] = true;
    filter["group-passphrase"] = true;
    DynamicJsonDocument doc(512);
    if (deserializeJson(doc, json, len, DeserializationOption::Filter(filter))) {
        return;
    }
    if (doc["lora-band"].is<int>()) {
        out.default_band = static_cast<uint16_t>(doc["lora-band"].as<int>());
    }
    if (doc["name"].is<const char*>()) {
        snprintf(out.flashed_name, sizeof(out.flashed_name), "%s", doc["name"].as<const char*>());
    }
    if (doc["group-passphrase"].is<const char*>()) {
        snprintf(out.flashed_passphrase, sizeof(out.flashed_passphrase), "%s",
                 doc["group-passphrase"].as<const char*>());
    }
}

}  // namespace ff
