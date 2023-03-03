#include <Arduino.h>
#include "ConfigHandler.h"
#include <EEPROM.h>
#include "ConfigStrings.h"

void config_save() {
    for(size_t i = 0; i < sizeof(cfg); i++) {
        char data = ((char *)&cfg)[i];
        EEPROM.write(i, data);
    }
    EEPROM.commit();
}

void config_init(bool forcedefault) {

    size_t size = sizeof(cfg);
    EEPROM.begin(size * 2);

    for(size_t i = 0; i < size; i++)  {
        char data = EEPROM.read(i);
        ((char *)&cfg)[i] = data;
    }

    if (cfg.version != VERSION_CONFIG || FORCE_DEFAULT_CONFIG || forcedefault)
        {
        cfg.version = VERSION_CONFIG;
        strcpy(cfg.target_name, CFG_TARGET_NAME);
        cfg.lora_power = LORA_POWER;
        cfg.lora_band = LORA_BAND;
        cfg.lora_frequency = LORA_FREQUENCY;
        cfg.lora_mode = LORA_MODE;
        cfg.force_gs = LORA_FORCE_GS;
        cfg.lora_bandwidth = LORA_M0_BANDWIDTH;
        cfg.lora_coding_rate = LORA_M0_CODING_RATE;
        cfg.lora_spreading_factor = LORA_M0_SPREADING_FACTOR;
        cfg.lora_nodes = LORA_M0_NODES;
        cfg.lora_slot_spacing = LORA_M0_SLOT_SPACING;
        cfg.lora_timing_delay = LORA_M0_TIMING_DELAY;
        cfg.msp_after_tx_delay = LORA_M0_MSP_AFTER_TX_DELAY;

        cfg.display_enable = 1;
        config_save();
    }
}

void handleConfigMessage(Stream& input_source, String message)
{
    message.trim();
    if (message=="reboot") {
        input_source.println("Rebooting");
        delay(1000);
        ESP.restart();
    }
    else if (message=="reset") {
        input_source.println("Resetting to default values");
        config_init(1);
        input_source.println("Rebooting");
        delay(1000);
        ESP.restart();
    }
    else if (message=="cmd") {
        input_source.println("------------");
        input_source.println("info : Configuration infos");
        input_source.println("band433 : Radio module is 433MHz");
        input_source.println("band868 : Radio module is 868MHz");
        input_source.println("band915 : Radio module is 915MHz");
        input_source.println("mode0 : Lora mode " + (String)loramode_name[0]);
        input_source.println("mode1 : Lora mode " + (String)loramode_name[1]);
        input_source.println("mode2 : Lora mode " + (String)loramode_name[2]);
        input_source.println("debug : Toggle debug mode");
        input_source.println("gs : Toggle ground station mode");
        input_source.println("list : List active nodes");
        input_source.println("reset : Reset all values to default");
        input_source.println("reboot : Restarts the ESP");
        input_source.println("------------");
    }
    else if (message=="band433") {
        input_source.println("Setting band : 433MHz");
        input_source.println("Active after reboot");
        cfg.lora_band = 433;
        cfg.lora_frequency = LORA_FREQUENCY_433;
        config_save();
    }
    else if (message=="band868") {
        input_source.println("Setting band : 868MHz");
        input_source.println("Active after reboot");
        cfg.lora_band = 868;
        cfg.lora_frequency = LORA_FREQUENCY_868;
        config_save();
    }
    else if (message=="band915") {
        input_source.println("Setting band : 915MHz");
        input_source.println("Active after reboot");
        cfg.lora_band = 915;
        cfg.lora_frequency = LORA_FREQUENCY_915;
        config_save();
    }
    else if (message=="mode0") {
        input_source.println("Setting radio mode : " + (String)loramode_name[0]);
        cfg.lora_mode = 0;
        cfg.lora_bandwidth = LORA_M0_BANDWIDTH;
        cfg.lora_coding_rate = LORA_M0_CODING_RATE;
        cfg.lora_spreading_factor = LORA_M0_SPREADING_FACTOR;
        cfg.lora_nodes = LORA_M0_NODES;
        cfg.lora_slot_spacing = LORA_M0_SLOT_SPACING;
        cfg.lora_timing_delay = LORA_M0_TIMING_DELAY;
        cfg.msp_after_tx_delay = LORA_M0_MSP_AFTER_TX_DELAY;
        input_source.println((String)cfg.lora_nodes + " nodes x " + (String)cfg.lora_slot_spacing + "ms = " + (String)(cfg.lora_nodes * cfg.lora_slot_spacing) + "ms cycle");
        input_source.println("Active after reboot");
        config_save();
    }
    else if (message=="mode1") {
        input_source.println("Setting radio mode : " + (String)loramode_name[1]);
        cfg.lora_mode = 1;
        cfg.lora_bandwidth = LORA_M1_BANDWIDTH;
        cfg.lora_coding_rate = LORA_M1_CODING_RATE;
        cfg.lora_spreading_factor = LORA_M1_SPREADING_FACTOR;
        cfg.lora_nodes = LORA_M1_NODES;
        cfg.lora_slot_spacing = LORA_M1_SLOT_SPACING;
        cfg.lora_timing_delay = LORA_M1_TIMING_DELAY;
        cfg.msp_after_tx_delay = LORA_M1_MSP_AFTER_TX_DELAY;
        input_source.println((String)cfg.lora_nodes + " nodes x " + (String)cfg.lora_slot_spacing + "ms = " + (String)(cfg.lora_nodes * cfg.lora_slot_spacing) + "ms cycle");
        input_source.println("Active after reboot");
        config_save();
    }
    else if (message=="mode2") {
        input_source.println("Setting radio mode : " + (String)loramode_name[2]);
        cfg.lora_mode = 2;
        cfg.lora_bandwidth = LORA_M2_BANDWIDTH;
        cfg.lora_coding_rate = LORA_M2_CODING_RATE;
        cfg.lora_spreading_factor = LORA_M2_SPREADING_FACTOR;
        cfg.lora_nodes = LORA_M2_NODES;
        cfg.lora_slot_spacing = LORA_M2_SLOT_SPACING;
        cfg.lora_timing_delay = LORA_M2_TIMING_DELAY;
        cfg.msp_after_tx_delay = LORA_M2_MSP_AFTER_TX_DELAY;
        input_source.println((String)cfg.lora_nodes + " nodes x " + (String)cfg.lora_slot_spacing + "ms = " + (String)(cfg.lora_nodes * cfg.lora_slot_spacing) + "ms cycle");
        input_source.println("Active after reboot");
        config_save();
    }
    else if (message=="powerlow") {
        cfg.lora_power = 4;
        input_source.println("Set power to 4dBm");
        config_save();
    }
    else if (message=="powermid") {
        cfg.lora_power = 10;
        input_source.println("Set power to 10dBm");
        config_save();
    }
    else if (message=="powerhigh") {
        cfg.lora_power = 20;
        input_source.println("Set power to 20dBm");
        config_save();
    }
    else if (message=="debug") {
        if (sys.debug) { sys.debug = 0; }
        else { sys.debug = 1;}
    }
    else if (message=="gs") {
        if (cfg.force_gs) { cfg.force_gs = 0; }
        else { cfg.force_gs = 1; }
        input_source.println("Ground station mode: " + (String)onoff[cfg.force_gs]);
        input_source.println("Active after reboot");
        config_save();
    }
    else if (message=="6nodes") {
        input_source.println("Setting 6 nodes max");
        input_source.println("Active after reboot");
        cfg.lora_nodes = 6;
        config_save();
    }
    else if (message=="info") {
        input_source.println("ESP32 Radar version: " + String(cfg.version));
        input_source.println("Target: " + String(cfg.target_name) + " " + String(CFG_TARGET_FULLNAME));
        input_source.println("Band: " + String(cfg.lora_band)+ "MHz / Mode: " + String(loramode_name[cfg.lora_mode]));
        input_source.println("Freq: " + String((float)cfg.lora_frequency / 1000000, 3)+ "MHz / Power: " + String(cfg.lora_power));
        input_source.println("BW: " + String((float)cfg.lora_bandwidth / 1000, 0)+ "KHz / CR: " + String(cfg.lora_coding_rate) + " / SF: " +String(cfg.lora_spreading_factor));
        input_source.println((String)cfg.lora_nodes + " nodes x " + (String)cfg.lora_slot_spacing + "ms = " + (String)(cfg.lora_nodes * cfg.lora_slot_spacing) + "ms cycle");
        input_source.println("Ground station mode: " + (String)onoff[cfg.force_gs]);
    }
    else if (message=="list") {
        for (int i = 0; i < cfg.lora_nodes; i++) {
            if (peers[i].id > 0) {
                input_source.println((String)"[" + char(i+65) + "] " + peers[i].name + " N" + String((float)peers[i].gps_rec.lat / 10000000, 5) + " E" + String((float)peers[i].gps_rec.lon / 10000000, 5) + " " + peers[i].gps_rec.alt + "m " + String(peers[i].gps.groundSpeed / 100) + "m/s " + String(peers[i].gps.groundCourse / 10) + "° " + String((int)((sys.lora_last_tx - peers[i].updated) / 1000)) + "s " + String(peers[i].rssi) + "db");
            }
            if (i + 1 == curr.id) {
                input_source.println((String)"[" + char(i+65) + "] " + String(host_name[curr.host]) + " (host) N" + String((float)curr.gps.lat / 10000000, 5) + " E" + String((float)curr.gps.lon / 10000000, 5) + " " + String(curr.gps.alt) + "m Eff:" + String(stats.percent_received) + "%" );
            }
        }
    }
    else {
        input_source.printf("Unknown command %s\n", message.c_str());
    }
}