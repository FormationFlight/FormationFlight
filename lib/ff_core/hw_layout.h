#pragma once
//
// The board's pin map, read at boot rather than compiled in.
//
// ExpressLRS builds one "unified" firmware per chip and radio and describes each
// board with a JSON layout from github.com/ExpressLRS/targets, appended to the
// image (see scripts/elrs_targets.py). FormationFlight uses the same layouts and
// the same trailer, so one Unified_ESP8285_2400 build runs on every SX1280
// ESP8285 receiver ExpressLRS knows about.
//
// This is the platform-free half: the struct, the JSON parser, and the mapping
// from a requested power to the board's power table. Finding the JSON in flash
// is in src/hal/HardwareConfig.
//
#include <cstddef>
#include <cstdint>

namespace ff {

enum class LoraChip : uint8_t { SX127x, SX128x, LR1121 };

// Where the layout came from, for the log and the status view. "None" on a
// unified build is the one that matters: the image was flashed without a
// target, so there is no radio and possibly no MSP UART either.
enum class LayoutSource : uint8_t { None, Compiled, Trailer, File };

const char* layoutSourceName(LayoutSource s);

constexpr int kPinNone = -1;

// ExpressLRS power levels, by index: 10, 25, 50, 100, 250, 500, 1000, 2000 mW.
constexpr int kPowerLevels = 8;
int8_t powerLevelDbm(int level);
uint16_t powerLevelMw(int level);

struct RadioPins {
    int nss = kPinNone;
    int rst = kPinNone;
    int dio0 = kPinNone;  // SX127x interrupt
    int dio1 = kPinNone;  // SX128x / LR1121 interrupt
    int busy = kPinNone;  // SX128x / LR1121
    // External front end, driven by the ESP. rxen high while listening, txen
    // high while transmitting. An LR1121 switches its own front end from its
    // DIO5-8 (radio_rfsw_ctrl) and normally has neither.
    int rxen = kPinNone;
    int txen = kPinNone;
};

struct HardwareLayout {
    LayoutSource source = LayoutSource::None;

    // Sizes match the ELRS trailer fields, plus the terminator.
    char product_name[129] = {0};
    char device_name[17] = {0};

    int serial_rx = kPinNone;
    int serial_tx = kPinNone;

    int radio_sck = kPinNone;
    int radio_miso = kPinNone;
    int radio_mosi = kPinNone;
    // radio[1] is the second radio of a true-diversity receiver. Its nss is
    // kPinNone on everything else.
    RadioPins radio[2];

    // SX127x: the board is wired to RFO_HF, not PA_BOOST. LR1121: use the
    // low-power sub-GHz PA rather than the high-power one. Same ELRS key.
    bool radio_rfo_hf = false;
    // Regulator. ELRS leaves it on the LDO unless the layout says the board has
    // the DC-DC inductor fitted; a compiled legacy target keeps the driver's
    // own default, which is DC-DC.
    bool radio_dcdc = true;
    // LR1121 TCXO supply, as the ELRS voltage code (0 = 1.6 V .. 7 = 3.3 V), or
    // -1 for a plain crystal.
    int radio_tcxo = -1;
    // LR1121 SetDioAsRfSwitch, verbatim: enable, standby, rx, tx, tx_hp,
    // tx_hf, unused, wifi. Zero entries means ELRS' default table.
    uint8_t radio_rfsw_ctrl[8] = {0};
    uint8_t radio_rfsw_ctrl_count = 0;

    // Held high whenever the radio is in use (ELRS: power_enable).
    int power_enable = kPinNone;
    // An RF switch selecting one of two antennas on a single radio.
    int ant_ctrl = kPinNone;
    // dB of LNA ahead of the radio, which the RSSI it reports includes.
    int8_t power_lna_gain = 0;

    // The ELRS power table: power_values[i] is what to hand the radio for level
    // power_min + i. power_values_dual is the 2.4 GHz table on an LR1121.
    bool has_power_table = false;
    uint8_t power_min = 0;
    uint8_t power_max = 0;
    uint8_t power_default = 0;
    int16_t power_values[kPowerLevels] = {0};
    uint8_t power_values_count = 0;
    int16_t power_values_dual[kPowerLevels] = {0};
    uint8_t power_values_dual_count = 0;

    // No table (a compiled legacy target): the radio's own dBm, used as is.
    bool has_power = false;
    int8_t power_dbm = 0;

    int led = kPinNone;
    int button = kPinNone;

    // The LoRa band to boot on when radios.lora_band does not say: the
    // "lora-band" option in the trailer (elrs_targets.py --band), or
    // LORA_DEFAULT_BAND on a legacy target. 0 for none.
    uint16_t default_band = 0;

    // First-boot settings from the trailer's options ("name",
    // "group-passphrase"): what the web installer was asked for. Applied once,
    // to a node with no config.json yet; after that the web UI owns them.
    // Sizes match Settings (kMaxNameLen, kMaxPassphraseLen).
    char flashed_name[16] = {0};
    char flashed_passphrase[33] = {0};

    // A board described by an ExpressLRS layout, whose RF front end is
    // therefore matched for 868/915 or 2.4 GHz and nothing else.
    bool elrsHardware() const {
        return source == LayoutSource::Trailer || source == LayoutSource::File;
    }

    bool hasRadio(LoraChip chip, int index = 0) const;
    int radioCount(LoraChip chip) const;
};

// What to hand the radio for an ELRS power_values entry, in the dBm the RadioLib
// driver takes.
//
// ELRS stores what it writes to the radio, not a dBm figure. On the SX1280 and
// LR1121 that happens to be dBm. On the SX127x it is the whole RegPaConfig byte
// minus the PA select bit: MaxPower in 6:4 and OutputPower in 3:0. Every value
// in the targets repo has MaxPower = 7, which makes PA_BOOST 2 + OutputPower
// dBm and RFO_HF OutputPower dBm (Pmax = 10.8 + 0.6 * 7 = 15).
int8_t elrsPowerToDbm(LoraChip chip, int value, bool rfo_hf);

// FormationFlight's LoRa power when radios.lora_power_dbm is 0.
constexpr int kDefaultLoraDbm = 10;

struct PowerChoice {
    int8_t chip_dbm = 0;   // what the driver hands RadioLib
    int8_t output_dbm = 0; // what leaves the board, for display
    int8_t level = -1;     // ELRS power level, or -1 with no table
};

// The power to run at.
//
// requested_dbm is radios.lora_power_dbm: the most the person wants out of the
// antenna port, or 0 for FormationFlight's default of 10 mW (kDefaultLoraDbm).
// Deliberately not the ELRS board's own default level, which is 50 mW on an
// ES900 and 100 mW or more on a PA board: FormationFlight shares the air with
// the pilot's control link, and LoRa reaches kilometres at 10 mW. With a power
// table that is the highest level not above it (never below power_min);
// ceiling_dbm, the band's legal limit, caps both. Without one it is the
// radio's dBm, clamped to what the chip accepts.
//
// output_dbm is the level's nominal output, which is how ELRS labels it, and on
// a PA board is a long way from chip_dbm.
PowerChoice choosePower(const HardwareLayout& hw, LoraChip chip, bool high_band,
                        int requested_dbm, int ceiling_dbm);

// Parses an ELRS layout into `out`, overwriting only what the JSON mentions, so
// a compiled default survives for anything the layout leaves out. Returns false
// with `*err` set on malformed JSON; `out` is then untouched.
bool parseLayoutJson(const char* json, size_t len, HardwareLayout& out, const char** err);

// The trailer's options block: ELRS keeps its bind UID and WiFi credentials
// there; FormationFlight reads "lora-band", "name" and "group-passphrase".
// Anything unparseable is ignored -- the options are optional.
void parseOptionsJson(const char* json, size_t len, HardwareLayout& out);

}  // namespace ff
