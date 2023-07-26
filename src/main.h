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
#define VERSION_CONFIG 400
#define START_DELAY 1000

// These are injected at build time by build_flags.py, these are here to make syntax highlighting happy
#ifndef VERSION
#define VERSION "vX.X.X"
#endif
#ifndef BUILDTIME
#define BUILDTIME "2000-01-01"
#endif
#ifndef CLOUD_BUILD
#define CLOUD_BUILD false
#endif
#ifndef GITHASH
#define GITHASH "githash"
#endif

// Timing parameters

#define LORA_M3_BANDWIDTH 250000 // Hz
#define LORA_M3_CODING_RATE 4
#define LORA_M3_SPREADING_FACTOR 6
#define LORA_M3_NODES 6
#define LORA_M3_SLOT_SPACING 1000/LORA_M3_NODES/10 // ms
#define LORA_M3_TIMING_DELAY -10 // ms, roughly the length of an OTA transmission
#define LORA_M3_MSP_AFTER_TX_DELAY 1 // ms

// --- All modes common

#define LORA_CYCLE_SCAN 5000 // 5s
#define LORA_PEER_TIMEOUT 6000 // 6s
#define LORA_DRIFT_THRESHOLD 2 // Min for action
#define LORA_DRIFT_CORRECTION 1 // Max to correct

// --------- IO AND DISPLAY

// Interval in ms between display updates
#define DISPLAY_CYCLE 250
// Standard MSP serial speed
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

struct curr_t {
    // Our timeslot ID
    uint8_t id;
    // MSP FC state
    uint8_t state;
    // What MSP FC type we're connected to
    MSPHost host;
    // Our own name
    char name[16];
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

    bool force_gs = false;

    uint8_t lora_nodes;
    uint16_t slot_spacing;
    int16_t lora_timing_delay;
    int16_t msp_after_tx_delay;

    bool display_enable;
};

struct system_t {
    MODE phase;
    // Time in ms for all peers to transmit one interval
    uint16_t lora_cycle;
    // Incremented every packet sent
    uint8_t ota_nonce = 0;
    // Current millis() at beginning of each loop
    uint32_t now = 0;
    // Last OTA packet received ID field
    uint8_t air_last_received_id = 0;
    // Set enabled when there are too many peers, disables transmission
    bool disable_tx = false;
    // millis() when last TX began
    uint32_t last_tx_start = 0;
    // millis() when last TX ended
    uint32_t last_tx_end = 0;
    // millis() when next TX is scheduled for
    uint32_t next_tx = 0;
    // Offset from peer in ms
    int32_t drift = 0;
    // Amount by which we'll offset our next transmission
    int drift_correction = 0;
    // If display is enabled
    bool display_enable = true;
    // Current page on OLED
    uint8_t display_page = 0;
    // millis() when the display contents were last updated
    uint32_t display_updated = 0;
    // millis() when the button was last released
    uint32_t io_button_released = 0;
    // Whether button is pressed
    bool io_button_pressed = false;

    // Timestamp when the last scan cycle began
    uint32_t cycle_scan_begin;

    // Message to display to the user
    char message[20];
};

extern config_t cfg;
extern system_t sys;
extern curr_t curr;
#endif