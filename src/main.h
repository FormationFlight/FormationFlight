#ifndef MAIN_H
#define MAIN_H
//
// ESP32 Radar
// Github : https://github.com/OlivierC-FR/ESP32-INAV-Radar
// RCgroups : https://www.rcgroups.com/forums/showthread.php?3304673-iNav-Radar-ESP32-LoRa-modems
//
// -------------------------------------------------------------------------------------------

#ifndef DEBUG
#define DEBUG 0
#endif
#define DBGLN(x) do { if (DEBUG) { Serial.printf("%lu: ", millis()); Serial.println(x); } } while (0)
#define DBGF(...) do { if (DEBUG) { Serial.printf("%lu: ", millis()); Serial.printf(__VA_ARGS__); } } while (0)

#include "lib/MSP/MSP.h"
#include "lib/MSP/MSPManager.h"
// -------- GENERAL

#define PRODUCT_NAME "FormationFlight"
#define VERSION "4.0.0"
#define VERSION_CONFIG 400
#define FORCE_DEFAULT_CONFIG 1
#define CFG_AUTOSTART_BT 0
#define START_DELAY 1000
// #define CFG_TARGET_NAME < (platformio.ini)
// #define CFG_TARGET_FULLNAME < (platformio.ini)

// -------- LORA DEFAULTS
#define LORA_MODE 0
#define LORA_AUTOMODE 0
#define LORA_FORCE_GS 0
// #define LORA_BAND 433 < (platformio.ini)
// #define LORA_FREQUENCY 433375000 < (platformio.ini)
#define LORA_FREQUENCY_433 433375000 // Hz
#define LORA_FREQUENCY_868 868500000 // Hz
#define LORA_FREQUENCY_915 915000000 // Hz

// --- Mode 0 (Standard)

#define LORA_M0_BANDWIDTH 250000 // Hz
#define LORA_M0_CODING_RATE 5
#define LORA_M0_SPREADING_FACTOR 10
#define LORA_M0_NODES 4
#define LORA_M0_SLOT_SPACING 250 // ms
#define LORA_M0_TIMING_DELAY -150 // ms
#define LORA_M0_MSP_AFTER_TX_DELAY 150 // ms

// --- Mode 1 (Long range)

#define LORA_M1_BANDWIDTH 250000 // Hz
#define LORA_M1_CODING_RATE 5
#define LORA_M1_SPREADING_FACTOR 11
#define LORA_M1_NODES 4
#define LORA_M1_SLOT_SPACING 500 // ms
#define LORA_M1_TIMING_DELAY -300 // ms
#define LORA_M1_MSP_AFTER_TX_DELAY 300 // ms

// --- Mode 2 (Fast)

#define LORA_M2_BANDWIDTH 250000 // Hz
#define LORA_M2_CODING_RATE 5
#define LORA_M2_SPREADING_FACTOR 9
#define LORA_M2_NODES 3
#define LORA_M2_SLOT_SPACING 166 // ms
#define LORA_M2_TIMING_DELAY -75 // ms
#define LORA_M2_MSP_AFTER_TX_DELAY 75 // ms

// --- Mode 3 (Ultrafast)

#define LORA_M3_BANDWIDTH 250000 // Hz
#define LORA_M3_CODING_RATE 4
#define LORA_M3_SPREADING_FACTOR 6
#define LORA_M3_NODES 6
#define LORA_M3_SLOT_SPACING 1000/LORA_M3_NODES/10 // ms
#define LORA_M3_TIMING_DELAY -5 // ms
#define LORA_M3_MSP_AFTER_TX_DELAY 1 // ms

// --- All modes common

#define LORA_CYCLE_SCAN 5000 // 5s
#define LORA_PEER_TIMEOUT 6000 // 6s
#define LORA_DRIFT_THRESHOLD 4 // Min for action
#define LORA_DRIFT_CORRECTION 12 // Max to correct

// --------- IO AND DISPLAY

#define DISPLAY_CYCLE 250
#define IO_LEDBLINK_DURATION 300
#define SERIAL_SPEED 115200

// -------- PHASES

enum MODE {
    MODE_START = 0,
    MODE_HOST_SCAN = 1,
    MODE_OTA_SCAN = 2,
    MODE_OTA_SYNC = 3,
    MODE_OTA_RX = 4,
    MODE_OTA_TX = 5
};

// -------- HOST


struct curr_t {
    uint8_t id;
    uint8_t state;
    MSPHost host;
    char name[16];
    uint8_t tick;
    msp_raw_gps_t gps;
    msp_fc_version_t fcversion;
    msp_analog_t fcanalog;
};

/*struct air_type0_t {
    unsigned int id : 3;
    signed int lat : 25; // -9 000 000 to +9 000 000 (5 decimals)
    signed int lon : 26; // -18 000 000 to +18 000 000 (5 decimals)
    unsigned int alt : 13; // 0 to +8192m
    unsigned int extra_type : 3;
    signed int extra_value : 10;
    uint8_t crc;
};*/

enum PACKETTYPE {
    PACKET_TYPE_RADAR_POSITION = 0
};

// For encryption reasons, the packet must be 16 bytes or greater
struct __attribute__((packed)) air_type0_t {
    uint8_t packet_type;
    uint8_t id;
    int32_t lat;
    int32_t lon;
    uint16_t alt;
    uint8_t extra_type;
    int16_t extra_value;
    uint8_t crc;
};

struct config_t {

    uint16_t version;
    uint8_t profile_id;
    char target_name[8];

    uint8_t lora_power;
    uint16_t lora_band;
    uint32_t lora_frequency;

    uint8_t lora_mode;
    bool lora_automode;
    bool force_gs;

    uint32_t lora_bandwidth;
    uint8_t lora_coding_rate;
    uint8_t lora_spreading_factor;
    uint8_t lora_nodes;
    uint16_t slot_spacing;
    int16_t lora_timing_delay;
    int16_t msp_after_tx_delay;

    // IO & Display

    bool display_enable;
};

struct system_t {
    bool debug;
    bool forcereset;
    uint8_t phase;

    uint16_t lora_cycle;
    uint8_t ota_nonce = 0;

    uint32_t now = 0;
    uint32_t now_sec = 0;
    uint8_t air_last_received_id = 0;
    int last_rssi;
    float last_snr;
    long last_freqerror;

    uint8_t pps = 0;
    uint8_t ppsc = 0;
    uint8_t num_peers = 0;
    uint8_t num_peers_active = 0;

    bool lora_no_tx = 0;
    uint8_t ota_slot = 0;
    uint32_t last_tx = 0;
    uint32_t lora_last_rx = 0;
    uint32_t next_tx = 0;
    int32_t drift = 0;
    int drift_correction = 0;

    uint32_t msp_next_cycle = 0;

    uint8_t display_page = 0;
    bool display_enable = 1;
    uint32_t display_updated = 0;

    uint32_t io_button_released = 0;
    bool io_button_pressed = 0;

    uint16_t cycle_stats;
    uint32_t cycle_scan_begin;

    uint32_t menu_begin;
    uint16_t menu_timeout = 4000;
    uint8_t menu_line = 1;

    uint32_t io_led_changestate;
    uint8_t io_led_count;
    uint8_t io_led_blink;
    uint32_t stats_updated = 0;

    bool io_bt_enabled = 0;

    char message[20];
};

extern config_t cfg;
extern system_t sys;
extern curr_t curr;
#endif