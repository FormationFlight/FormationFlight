//
// ESP32 Radar
// Github : https://github.com/OlivierC-FR/ESP32-INAV-Radar
// RCgroups : https://www.rcgroups.com/forums/showthread.php?3304673-iNav-Radar-ESP32-LoRa-modems
//
// -------------------------------------------------------------------------------------------

#include <targets.h>
#include <Arduino.h>
#ifdef PLATFORM_ESP32
#include <esp_system.h>
#endif
#include <lib/MSP.h>
#ifdef HAS_LORA
#include <lib/LoRa.h>
#endif
#include <lib/Display.h>
#include <EEPROM.h>
#include <main.h>
#include <lib/Helpers.h>
#include <lib/ConfigHandler.h>
// Configuration systems (Bluetooth or WiFi)
#ifdef BT_CONFIG
#include <lib/BTConfig.h>
#elif defined(WIFI_CONFIG)
#include <lib/WiFiConfig.h>
#endif
#include <lib/espnow.h>

#if !defined(WIFI_CONFIG) && defined(DEBUG)
#define DBGLN(x) Serial.printf("%d: ", millis()); Serial.println(x);
#else
#define DBGLN(x) debugPrintln(x);
#endif

// -------- VARS

config_t cfg;
system_t sys;
stats_t stats;
MSP msp;
msp_radar_pos_t radarPos;
curr_t curr;
peer_t peers[LORA_NODES_MAX];
air_type0_t air_0;

// -------- LoRa

void packet_prep()
{
    air_0.id = curr.id;
    air_0.lat = curr.gps.lat / 100;
    air_0.lon = curr.gps.lon / 100;
    air_0.alt = curr.gps.alt; // m
    air_0.extra_type = sys.lora_tick % 5;

    switch (air_0.extra_type)
    {
    case 0:
        air_0.extra_value = curr.gps.groundCourse / 10;
        break;

    case 1:
        air_0.extra_value = curr.gps.groundSpeed / 20;
        break;

    case 2:
        air_0.extra_value = curr.name[0];
        break;

    case 3:
        air_0.extra_value = curr.name[1];
        break;

    case 4:
        air_0.extra_value = curr.name[2];
        break;

    default:
        break;
    }
    // calculate crc
    uint8_t buf[sizeof(air_type0_t)];
    memcpy_P(buf, &air_0, sizeof(air_type0_t));
    uint8_t calculatedCrc = 0;
    for (uint8_t i = 0; i < sizeof(air_type0_t) - sizeof(air_0.crc); i++)
    {
        calculatedCrc = crc8_dvb_s2(calculatedCrc, buf[i]);
    }
    air_0.crc = calculatedCrc;
}

void lora_send()
{
#ifdef HAS_LORA
    while (!LoRa.beginPacket())
    {
    } // --------------------------- Implicit len
    LoRa.write((uint8_t *)&air_0, sizeof(air_0));
    LoRa.endPacket(false);
#endif
}

void lora_receive(int packetSize)
{
    #ifdef HAS_LORA
    if (packetSize == 0)
        return;
    if (packetSize != sizeof(air_type0_t))
    {
        return;
    }

    sys.lora_last_rx = millis();
    sys.lora_last_rx -= (stats.last_tx_duration > 0) ? stats.last_tx_duration : 0; // RX time is the same as TX time
    sys.last_rssi = LoRa.packetRssi();
    sys.ppsc++;

    LoRa.readBytes((uint8_t *)&air_0, packetSize);

    uint8_t calculatedCrc = 0;
    // Check CRC
    for (uint8_t i = 0; i < sizeof(air_0) - sizeof(air_0.crc); i++)
    {
        calculatedCrc = crc8_dvb_s2(calculatedCrc, ((uint8_t *)&air_0)[i]); // loop over summable data
    }
    if (calculatedCrc != air_0.crc)
    {
        return;
    }

    uint8_t id = air_0.id - 1;

    peers[id].gps_pre.lat = peers[id].gps.lat;
    peers[id].gps_pre.lon = peers[id].gps.lon;
    peers[id].gps_pre.alt = peers[id].gps.alt;
    peers[id].gps_pre.groundCourse = peers[id].gps.groundCourse;
    peers[id].gps_pre.groundSpeed = peers[id].gps.groundSpeed;
    peers[id].gps_pre_updated = peers[id].updated;

    sys.air_last_received_id = air_0.id;
    peers[id].id = sys.air_last_received_id;
    peers[id].lq_tick++;
    peers[id].state = 0;
    peers[id].lost = 0;
    peers[id].updated = sys.lora_last_rx;
    peers[id].rssi = sys.last_rssi;

    peers[id].gps.lat = air_0.lat * 100;
    peers[id].gps.lon = air_0.lon * 100;
    peers[id].gps.alt = air_0.alt; // m

    switch (air_0.extra_type)
    {
    case 0:
        peers[id].gps.groundCourse = air_0.extra_value * 10;
        break;

    case 1:
        peers[id].gps.groundSpeed = air_0.extra_value * 20;
        break;

    case 2:
        peers[id].name[0] = air_0.extra_value;
        break;

    case 3:
        peers[id].name[1] = air_0.extra_value;
        break;

    case 4:
        peers[id].name[2] = air_0.extra_value;
        peers[id].name[3] = 0;
        break;

    default:
        break;
    }

    if (peers[id].gps.lat != 0 && peers[id].gps.lon != 0)
    { // Save the last known coordinates
        peers[id].gps_rec.lat = peers[id].gps.lat;
        peers[id].gps_rec.lon = peers[id].gps.lon;
        peers[id].gps_rec.alt = peers[id].gps.alt;
        peers[id].gps_rec.groundCourse = peers[id].gps.groundCourse;
        peers[id].gps_rec.groundSpeed = peers[id].gps.groundSpeed;
    }

    sys.num_peers = count_peers(0, &cfg);

    if (sys.io_bt_enabled && sys.debug)
    {
        debugPrintln((String) "[" + char(id + 65) + "] " + peers[id].name + " N" + peers[id].gps.lat + " E" + peers[id].gps.lon + " " + peers[id].gps.alt + "m " + String(peers[id].gps.groundSpeed / 100) + "m/s " + String(peers[id].gps.groundCourse / 10) + "° " + String(peers[id].rssi) + "db");
        // debugPrintln("SNR: " + String(sys.last_snr)+"dB / Freq.err: " + String(sys.last_freqerror)+"Hz");
        // debugPrintln("Packet size: " + String(packetSize));
    }

    if ((sys.air_last_received_id == curr.id) && (sys.phase > MODE_LORA_SYNC) && !sys.lora_no_tx)
    { // Slot conflict
        uint32_t cs1 = peers[id].name[0] + peers[id].name[1] * 26 + peers[id].name[2] * 26 * 26;
        uint32_t cs2 = curr.name[0] + curr.name[1] * 26 + curr.name[2] * 26 * 26 + 1;
        if (cs1 < cs2)
        { // Pick another slot
            sprintf(sys.message, "%s", "ID CONFLICT");
            pick_id();
            resync_tx_slot(cfg.lora_timing_delay);
        }
    }
    #endif
}

void lora_init()
{
#ifdef HAS_LORA
#ifdef PLATFORM_ESP32
    SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
#elif defined(PLATFORM_ESP8266)
    SPI.begin();
    SPI.setBitOrder(MSBFIRST);
    SPI.setDataMode(SPI_MODE0);
    SPI.setFrequency(10000000);
#endif
    LoRa.setPins(LORA_CS, LORA_RST, LORA_IRQ);

    if (!LoRa.begin(cfg.lora_frequency))
    {
        DBGLN("failed to init LoRa");
        while (1)
            ;
    }

    LoRa.sleep();
    LoRa.setSignalBandwidth(cfg.lora_bandwidth);
    LoRa.setCodingRate4(cfg.lora_coding_rate);
    LoRa.setSpreadingFactor(cfg.lora_spreading_factor);
    // #if LORA_POWER < 18
    // LoRa.setTxPower(cfg.lora_power, 0);
    // #else
    LoRa.setTxPower(cfg.lora_power, 1);
    // #endif
    LoRa.setOCP(250);
    LoRa.idle();
    LoRa.onReceive(lora_receive);
    LoRa.enableCrc();
#endif
}

// -------- MSP and FC

void msp_get_state()
{
    uint32_t modes;
    msp.getActiveModes(&modes);
    curr.state = bitRead(modes, 0);
}

void msp_get_name()
{
    msp.request(MSP_NAME, &curr.name, sizeof(curr.name));
    curr.name[7] = '\0';
}

void msp_get_gps()
{
    msp.request(MSP_RAW_GPS, &curr.gps, sizeof(curr.gps));
}

void msp_set_fc()
{
    char j[5];
    curr.host = HOST_NONE;
    msp.request(MSP_FC_VARIANT, &j, sizeof(j));

    if (strncmp(j, "INAV", 4) == 0)
    {
        curr.host = HOST_INAV;
        msp.request(MSP_FC_VERSION, &curr.fcversion, sizeof(curr.fcversion));
    }
    else if (strncmp(j, "GCS", 3) == 0)
    {
        curr.host = HOST_GCS;
        msp.request(MSP_FC_VERSION, &curr.fcversion, sizeof(curr.fcversion));
    }
    else if (strncmp(j, "ARDU", 4) == 0)
    {
        curr.host = HOST_ARDU;
        msp.request(MSP_FC_VERSION, &curr.fcversion, sizeof(curr.fcversion));
    }
    else if (strncmp(j, "BTFL", 4) == 0)
    {
        curr.host = HOST_BETA;
        msp.request(MSP_FC_VERSION, &curr.fcversion, sizeof(curr.fcversion));
    }
}

void msp_get_fcanalog()
{
    msp.request(MSP_ANALOG, &curr.fcanalog, sizeof(curr.fcanalog));
}

void msp_send_radar(uint8_t i)
{
    radarPos.id = i;
    radarPos.state = (peers[i].lost == 2) ? 2 : peers[i].state;
    radarPos.lat = peers[i].gps_comp.lat;              // x 10E7
    radarPos.lon = peers[i].gps_comp.lon;              // x 10E7
    radarPos.alt = peers[i].gps_comp.alt * 100;        // cm
    radarPos.heading = peers[i].gps.groundCourse / 10; // From ° x 10 to °
    radarPos.speed = peers[i].gps.groundSpeed;         // cm/s
    radarPos.lq = peers[i].lq;
    msp.command2(MSP2_COMMON_SET_RADAR_POS, &radarPos, sizeof(radarPos), 0);
}

void msp_send_peer(uint8_t peer_id)
{
    if (peers[peer_id].id > 0)
    {
        msp_send_radar(peer_id);
    }
}

// -------- INTERRUPTS

volatile int interruptCounter = 0;
int numberOfInterrupts = 0;

#ifdef PLATFORM_ESP32
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
#endif

void IRAM_ATTR handleInterrupt()
{
#ifdef PLATFORM_ESP32
    portENTER_CRITICAL_ISR(&mux);
#endif

    if (sys.io_button_pressed == 0)
    {
        sys.io_button_pressed = 1;

        if (sys.phase > MODE_LORA_SYNC)
        {
            sys.display_page++;
        }
        else if (sys.phase == MODE_HOST_SCAN || sys.phase == MODE_LORA_SCAN)
        {
            sys.io_bt_enabled = 1; // Enable the Bluetooth if button pressed during host scan or lora scan
        }
        else if (sys.phase == MODE_START)
        {
            sys.forcereset = 1;
        }
        sys.io_button_released = millis();
    }
#ifdef PLATFORM_ESP32
    portEXIT_CRITICAL_ISR(&mux);
#endif
}

// ----------------------------- setup

void setup()
{

    sys.phase = MODE_START;
    sys.forcereset = 0;
    sys.io_bt_enabled = CFG_AUTOSTART_BT;
    sys.debug = 0;

    config_init();

    sys.lora_cycle = cfg.lora_nodes * cfg.lora_slot_spacing;
    sys.cycle_stats = sys.lora_cycle * 2;

    pinMode(IO_LED_PIN, OUTPUT);
    sys.io_led_blink = 0;
#ifdef PIN_BUTTON
    pinMode(PIN_BUTTON, INPUT);
    attachInterrupt(digitalPinToInterrupt(PIN_BUTTON), handleInterrupt, RISING);
#endif
    sys.io_button_pressed = 0;
    if (cfg.display_enable)
    {
        display_draw_intro();
    }

    delay(START_DELAY);

#ifdef PLATFORM_ESP32
    msp.begin(Serial1);
    Serial1.begin(SERIAL_SPEED, SERIAL_8N1, SERIAL_PIN_RX, SERIAL_PIN_TX);
    Serial.begin(115200);
#elif defined(PLATFORM_ESP8266)
    msp.begin(Serial);
    Serial.begin(SERIAL_SPEED, SERIAL_8N1);
#endif

    reset_peers();
#ifdef HAS_LORA
    DBGLN("Begin init LoRa");
    lora_init();
#endif
    DBGLN("Begin init espnow");
    espnow_setup();
    DBGLN("Begin display");

    if (cfg.display_enable)
    {
        display_draw_startup();
    }

    sys.cycle_scan_begin = millis();
    sys.now = millis();
    curr.host = HOST_NONE;
}

// ----------------------------------------------------------------------------- MAIN LOOP

void loop()
{
    sys.now = millis();

    // ---------------------- IO BUTTON

    if ((sys.now > sys.io_button_released + 150) && (sys.io_button_pressed == 1))
    {
        sys.io_button_pressed = 0;
    }

    // ---------------------- HOST SCAN

    if (sys.phase == MODE_START)
    {

        if (sys.forcereset)
        {
            display_draw_clearconfig();
            config_clear();
            delay(3000);
            ESP.restart();
        }
        sys.phase = MODE_HOST_SCAN;
    }

    // ---------------------- HOST SCAN

    if (sys.phase == MODE_HOST_SCAN)
    {

        if ((sys.now > (sys.cycle_scan_begin + HOST_MSP_TIMEOUT)) || (curr.host != HOST_NONE))
        { // End of the host scan
            if (curr.host != HOST_NONE)
            {
                msp_get_name();
            }

            if (curr.name[0] == '\0')
            {
                uint32_t chipID;
#ifdef PLATFORM_ESP8266
                chipID = ESP.getChipId();
#elif defined(PLATFORM_ESP32)
                uint64_t macAddress = ESP.getEfuseMac();
                uint64_t macAddressTrunc = macAddress << 40;
                chipID = macAddressTrunc >> 40;
#endif
                String chipIDString = String(chipID, HEX);
                chipIDString.toUpperCase();
                for (int i = 0; i < 3; i++)
                {
                    curr.name[i] = chipIDString.charAt(i + 3); //(char)random(65, 90);
                }
                curr.name[3] = 0;
            }

            curr.gps.fixType = 0;
            curr.gps.lat = 0;
            curr.gps.lon = 0;
            curr.gps.alt = 0;
            curr.id = 0;
#ifdef HAS_LORA
            LoRa.sleep();
            LoRa.receive();
#endif
            if (cfg.display_enable)
            {
                display_draw_scan(&sys);
            }

            sys.cycle_scan_begin = millis();
            sys.phase = MODE_LORA_SCAN;
        }
        else
        { // Still scanning
            if (sys.now > sys.display_updated + DISPLAY_CYCLE / 2)
            {
                delay(100);
                msp_set_fc();
                if (cfg.force_gs)
                {
                    curr.host = HOST_GCS;
                }
                if (cfg.display_enable)
                {
                    display_draw_progressbar(100 * (millis() - sys.cycle_scan_begin) / HOST_MSP_TIMEOUT);
                }
                sys.display_updated = millis();
            }
        }
    }

    // ---------------------- LORA INIT

    if (sys.phase == MODE_LORA_SCAN)
    {

        if (sys.now > (sys.cycle_scan_begin + LORA_CYCLE_SCAN))
        { // End of the scan, set the ID then sync

            if (sys.io_bt_enabled)
            {
                initConfigInterface();
            }
#if !defined(PIN_BUTTON)
            // Enable WiFi / Bluetooth if there's no button to do so
            initConfigInterface();
#endif

            sys.num_peers = count_peers(0, &cfg);
            if (sys.num_peers >= cfg.lora_nodes || curr.host == HOST_GCS)
            { // Too many nodes already, or connected to a ground station : go silent mode
                sys.lora_no_tx = 1;
            }
            else
            {
                sys.lora_no_tx = 0;
                pick_id();
            }
            sys.display_page = 0;
            sys.phase = MODE_LORA_SYNC;
        }
        else
        { // Still scanning
#ifdef HAS_OLED
            if (sys.now > sys.display_updated + DISPLAY_CYCLE / 2 && cfg.display_enable)
            {
                for (int i = 0; i < cfg.lora_nodes; i++)
                {
                    if (peers[i].id > 0)
                    {
                        display_draw_peername(peers[i].id);
                    }
                }
                display_draw_progressbar(100 * (millis() - sys.cycle_scan_begin) / LORA_CYCLE_SCAN);
                sys.display_updated = millis();
            }
#endif
            delay(20);
        }
    }

    // ---------------------- LORA SYNC

    if (sys.phase == MODE_LORA_SYNC)
    {

        if (sys.num_peers == 0 || sys.lora_no_tx)
        { // Alone or Silent mode, no need to sync
            sys.lora_next_tx = millis() + sys.lora_cycle;
        }
        else
        { // Not alone, sync by slot
            resync_tx_slot(cfg.lora_timing_delay);
        }

        sys.display_updated = sys.lora_next_tx + sys.lora_cycle - 30;
        sys.stats_updated = sys.lora_next_tx + sys.lora_cycle - 15;
        sys.pps = 0;
        sys.ppsc = 0;
        sys.num_peers = 0;
        stats.packets_total = 0;
        stats.packets_received = 0;
        stats.percent_received = 0;
        digitalWrite(IO_LED_PIN, LOW);
        sys.phase = MODE_LORA_RX;
    }

    // ---------------------- LORA RX

    if ((sys.phase == MODE_LORA_RX) && (sys.now > sys.lora_next_tx))
    {

        while (sys.now > sys.lora_next_tx)
        { // In  case we skipped some beats
            sys.lora_next_tx += sys.lora_cycle;
        }

        if (sys.lora_no_tx)
        {
            sprintf(sys.message, "%s", "SILENT MODE (NO TX)");
        }
        else
        {
            sys.phase = MODE_LORA_TX;
        }
        sys.lora_tick++;
    }

    // ---------------------- LORA TX

    if (sys.phase == MODE_LORA_TX)
    {
        if (curr.host == HOST_NONE)
        {
            curr.gps.lat = 0;
            curr.gps.lon = 0;
            curr.gps.alt = 0;
            curr.gps.groundCourse = 0;
            curr.gps.groundSpeed = 0;
        }
        else if (curr.host == HOST_INAV || curr.host == HOST_ARDU || curr.host == HOST_BETA)
        {
            msp_get_gps(); // GPS > FC > ESP
        }

        sys.lora_last_tx = millis();

        packet_prep();
#ifdef HAS_LORA

        // lora_send();
#endif
        DBGLN("sending ota");
        espnow_send(&air_0);
        stats.last_tx_duration = millis() - sys.lora_last_tx;

        // Drift correction

        if (curr.id > 1 && sys.num_peers_active > 0)
        {
            int prev = curr.id - 2;
            if (peers[prev].id > 0)
            {
                sys.lora_drift = sys.lora_last_tx - peers[prev].updated - cfg.lora_slot_spacing;

                if ((abs(sys.lora_drift) > LORA_DRIFT_THRESHOLD) && (abs(sys.lora_drift) < cfg.lora_slot_spacing))
                {
                    sys.drift_correction = constrain(sys.lora_drift, -LORA_DRIFT_CORRECTION, LORA_DRIFT_CORRECTION);
                    sys.lora_next_tx -= sys.drift_correction;
                    sprintf(sys.message, "%s %3d", "TIMING ADJUST", -sys.drift_correction);
                }
            }
        }

        sys.lora_slot = 0;
        sys.msp_next_cycle = sys.lora_last_tx + cfg.msp_after_tx_delay;

        // Back to RX
#ifdef HAS_LORA
        LoRa.sleep();
        LoRa.receive();
#endif
        sys.phase = MODE_LORA_RX;
    }

    // ---------------------- DISPLAY

    if ((sys.now > sys.display_updated + DISPLAY_CYCLE) && sys.display_enable && (sys.phase > MODE_LORA_SYNC) && cfg.display_enable)
    {
        stats.timer_begin = millis();

        if (sys.num_peers == 0 && sys.display_page == 1)
        { // No need for timings graphs when alone
            sys.display_page++;
        }

        if (sys.display_page >= (3 + cfg.lora_nodes))
        {
            sys.display_page = 0;
        }

        display_draw_status(&sys);
        sys.message[0] = 0;
        stats.last_oled_duration = millis() - stats.timer_begin;
        sys.display_updated = sys.now;
    }

    // ---------------------- SERIAL / MSP

    if (sys.now > sys.msp_next_cycle && sys.phase > MODE_LORA_SYNC && sys.lora_slot < cfg.lora_nodes)
    {
        stats.timer_begin = millis();

        if (sys.lora_slot == 0 && (curr.host == HOST_INAV || curr.host == HOST_ARDU || curr.host == HOST_BETA))
        {

            if (sys.lora_tick % 6 == 0)
            {
                msp_get_state();
            }

            if ((sys.lora_tick + 1) % 6 == 0)
            {
                msp_get_fcanalog();
            }
        }

        // msp_send_peer(sys.lora_slot);

        // ----------------Send MSP to FC and predict new position for all nodes minus current
        if (sys.lora_slot == 0) {
            DBGLN("sending msp");
            for (int i = 0; i < cfg.lora_nodes; i++)
            {
                if (peers[i].id > 0 && i + 1 != curr.id)
                {
                    peers[i].gps_comp.lat = peers[i].gps.lat;
                    peers[i].gps_comp.lon = peers[i].gps.lon;
                    peers[i].gps_comp.alt = peers[i].gps.alt;

                    /*if (peers[i].gps.groundSpeed > 200 && peers[i].gps.lat != 0 && peers[i].gps_pre.lat != 0)
                    { // If speed >2m/s : Compensate the position delay
                        sys.now_sec = millis();
                        int32_t comp_var_lat = peers[i].gps.lat - peers[i].gps_pre.lat;
                        int32_t comp_var_lon = peers[i].gps.lon - peers[i].gps_pre.lon;
                        int32_t comp_var_alt = peers[i].gps.alt - peers[i].gps_pre.alt;
                        int32_t comp_var_dur = 1 + peers[i].updated - peers[i].gps_pre_updated;
                        int32_t comp_dur_fw = (sys.now_sec - peers[i].updated);
                        float comp_ratio = comp_dur_fw / comp_var_dur;

                        peers[i].gps_comp.lat += comp_var_lat * comp_ratio;
                        peers[i].gps_comp.lon += comp_var_lon * comp_ratio;
                        peers[i].gps_comp.alt += comp_var_alt * comp_ratio;
                    }*/
                    msp_send_radar(i);
                    // delay(7);
                }
            }
        }
        stats.last_msp_duration[sys.lora_slot] = millis() - stats.timer_begin;
        sys.msp_next_cycle += cfg.lora_slot_spacing;
        sys.lora_slot++;
    }

    // ---------------------- SERIAL CONFIG CHANNEL
    handleConfig();

    // ---------------------- STATISTICS & IO

    if ((sys.now > (sys.cycle_stats + sys.stats_updated)) && (sys.phase > MODE_LORA_SYNC))
    {
        sys.pps = sys.ppsc;
        sys.ppsc = 0;
        // sys.last_snr = LoRa.packetSnr();
        // sys.last_freqerror = LoRa.packetFrequencyError();

        // Timed-out peers + LQ

        for (int i = 0; i < cfg.lora_nodes; i++)
        {
            if (sys.now > (peers[i].lq_updated + sys.lora_cycle * 4))
            {
                uint16_t diff = peers[i].updated - peers[i].lq_updated;
                peers[i].lq = constrain(peers[i].lq_tick * 4.2 * sys.lora_cycle / diff, 0, 4);
                peers[i].lq_updated = sys.now;
                peers[i].lq_tick = 0;
            }
            if (peers[i].id > 0 && ((sys.now - peers[i].updated) > LORA_PEER_TIMEOUT))
            { // Lost for a short time
                peers[i].lost = 2;
                /*if ((sys.now - peers[i].updated) > LORA_PEER_TIMEOUT_LOST)
                { // Lost for a long time
                    peers[i].lost = 2;
                }*/
            }
        }

        sys.num_peers_active = count_peers(1, &cfg);
        stats.packets_total += sys.num_peers_active * sys.cycle_stats / sys.lora_cycle;
        stats.packets_received += sys.pps;
        stats.percent_received = (stats.packets_received > 0) ? constrain(100 * stats.packets_received / stats.packets_total, 0, 100) : 0;

// Screen management
#ifdef HAS_OLED
        if (!curr.state && !sys.display_enable)
        { // Aircraft is disarmed = Turning on the OLED
            display_off();
            sys.display_enable = 1;
        }
        else if (curr.state && sys.display_enable)
        { // Aircraft is armed = Turning off the OLED
            display_on;
            sys.display_enable = 0;
        }
#endif
        sys.stats_updated = sys.now;
    }

    // ---------------------- LED blinker

    if (sys.lora_tick % 6 == 0)
    {
        if (sys.num_peers_active > 0)
        {
            sys.io_led_changestate = millis() + IO_LEDBLINK_DURATION;
            sys.io_led_count = 0;
            sys.io_led_blink = 1;
        }
    }

    if (sys.io_led_blink && millis() > sys.io_led_changestate)
    {

        sys.io_led_count++;
        sys.io_led_changestate += IO_LEDBLINK_DURATION;

        if (sys.io_led_count % 2 == 0)
        {
            digitalWrite(IO_LED_PIN, LOW);
        }
        else
        {
            digitalWrite(IO_LED_PIN, HIGH);
        }

        if (sys.io_led_count >= sys.num_peers_active * 2)
        {
            sys.io_led_blink = 0;
        }
    }
}