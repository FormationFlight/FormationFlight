#include <Arduino.h>
#ifdef HAS_OLED
#include <SSD1306.h>
#endif
#include "Display.h"
#include "../ConfigStrings.h"
#include "pixel.h"
#include "../Helpers.h"
#include "../Peers/PeerManager.h"
#include "../MSP/MSPManager.h"
#include "../Cryptography/CryptoManager.h"
#include "../Statistics/StatsManager.h"

#ifdef HAS_OLED
SSD1306 display(OLED_ADDRESS, OLED_SDA, OLED_SCL);
#endif

void display_init()
{
#ifdef HAS_OLED
#ifdef OLED_RST
    pinMode(OLED_RST, OUTPUT);
    digitalWrite(OLED_RST, LOW);
    delay(50);
    digitalWrite(OLED_RST, HIGH);
#endif
    display.init();
    display.flipScreenVertically();
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_LEFT);
#endif
}

void display_draw_status(system_t *sys)
{
#ifdef HAS_OLED
    GNSSLocation loc = GNSSManager::getSingleton()->getLocation();
    display.clear();
    int j = 0;
    int line;

    if (sys->display_page == 0)
    {

        display.setFont(ArialMT_Plain_24);
        display.setTextAlignment(TEXT_ALIGN_RIGHT);
        display.drawString(26, 11, String(loc.numSat));
        display.drawString(13, 42, String(PeerManager::getSingleton()->count_active() + 1 - sys->disable_tx));
        display.drawString(125, 11, String(peer_slotname[curr.id]));
        display.setFont(ArialMT_Plain_10);
        display.drawString(126, 29, "_ _ _ _ _ _ _ _ _ _ _ _ _ _ _ ");
#ifdef LORA_POWER
        display.drawString(125, 44, "+" + String(LORA_POWER) + "dB");
#endif
        display.drawString(125, 54, String(CryptoManager::getSingleton()->getEnabled() ? "ENCR" : "OPEN"));
        display.setTextAlignment(TEXT_ALIGN_CENTER);
        display.drawString(64, 0, String(sys->message));
        display.setTextAlignment(TEXT_ALIGN_LEFT);
        display.drawString(55, 12, String(curr.name));
        display.drawString(27, 23, GNSSManager::getSingleton()->getCurrentProviderNameShort());
        // display.drawString(35, 44, String(sys->pps) + "p/s");
        display.drawString(55, 23, String(host_name[curr.host]));
        display.drawString(15, 44, "/ " + String(NODES_MAX));
        // display.drawString(15, 54, String(loramode_name[cfg.lora_mode]));

        if (loc.fixType == GNSS_FIX_TYPE_2D)
            display.drawString(27, 12, "2D");
        if (loc.fixType == GNSS_FIX_TYPE_3D)
            display.drawString(27, 12, "3D");
    }

    else if (sys->display_page == 1)
    {

        long pos[NODES_MAX];
        long diff;

        display.setFont(ArialMT_Plain_10);
        display.setTextAlignment(TEXT_ALIGN_LEFT);
        display.drawHorizontalLine(0, 11, 128);

        for (int i = 0; i < cfg.lora_nodes; i++)
        {
            const peer_t *peer = PeerManager::getSingleton()->getPeer(i);
            if (peer->id > 0 && peer->lost == 0)
            {
                diff = sys->last_tx_end - peer->updated;
                if (diff > 0 && diff < sys->lora_cycle)
                {
                    pos[i] = 128 - round(128 * diff / sys->lora_cycle);
                }
            }
            else
            {
                pos[i] = -1;
            }
        }

        int rect_l = StatsManager::getSingleton()->getLatest(STATS_KEY_OTA_SENDTIME_US) * 128 / sys->lora_cycle;

        for (int i = 0; i < cfg.lora_nodes; i++)
        {

            display.setTextAlignment(TEXT_ALIGN_LEFT);
            const peer_t *peer = PeerManager::getSingleton()->getPeer(i);

            if (pos[i] > -1)
            {
                display.drawRect(pos[i], 0, rect_l, 12);
                display.drawString(pos[i] + 2, 0, String(peer_slotname[peer->id]));
            }

            if (peer->id > 0 && j < 4)
            {
                line = j * 9 + 14;

                display.drawString(0, line, String(peer_slotname[peer->id]));
                display.drawString(12, line, String(peer->name));
                display.setTextAlignment(TEXT_ALIGN_RIGHT);

                if (peer->lost == 1)
                { // Peer timed out, short
                    display.drawString(127, line, "x:" + String((int)((millis() - peer->updated) / 1000)) + "s");
                }
                else if (peer->lost == 2)
                { // Peer timed out, long
                    display.drawString(127, line, "L:" + String((int)((millis() - peer->updated) / 1000)) + "s");
                }
                else
                {
                    if (sys->last_tx_end > peer->updated)
                    {
                        display.drawString(119, line, String(sys->last_tx_end - peer->updated));
                        display.drawString(127, line, "-");
                    }
                    else
                    {
                        display.drawString(119, line, String(sys->lora_cycle + sys->last_tx_end - peer->updated));
                        //display.drawString(119, line, String(peer->updated - sys->last_tx_end));
                        display.drawString(127, line, "+");
                    }
                }
                j++;
            }
        }
    }

    else if (sys->display_page == 2)
    {

        display.setFont(ArialMT_Plain_10);
        display.setTextAlignment(TEXT_ALIGN_LEFT);
        display.drawString(0, 0, "OTA");
        display.drawString(0, 10, "MSP");
        display.drawString(0, 20, "OLED");
        display.drawString(0, 30, "CYCLE");
        display.drawString(0, 40, "SLOTS");
        display.drawString(0, 50, "UPTIME");

        display.drawString(112, 0, "us");
        display.drawString(112, 10, "us");
        display.drawString(112, 20, "us");
        display.drawString(112, 30, "ms");
        display.drawString(112, 40, "ms");
        display.drawString(112, 50, "s");

        display.setTextAlignment(TEXT_ALIGN_RIGHT);
        StatsManager *statsManager = StatsManager::getSingleton();
        display.drawString(111, 0,
                           String(statsManager->getAverage(STATS_KEY_OTA_SENDTIME_US)) + "/" +
                               String(statsManager->getHighest(STATS_KEY_OTA_SENDTIME_US)));
        display.drawString(111, 10,
                           String(statsManager->getAverage(STATS_KEY_MSP_SENDTIME_US)) + "/" +
                               String(statsManager->getHighest(STATS_KEY_MSP_SENDTIME_US)));
        display.drawString(111, 20,
                           String(statsManager->getAverage(STATS_KEY_DISPLAY_UPDATETIME_US)) + "/" +
                               String(statsManager->getHighest(STATS_KEY_DISPLAY_UPDATETIME_US)));
        display.drawString(111, 30, String(sys->lora_cycle));
        display.drawString(111, 40, String(cfg.lora_nodes) + " x " + String(cfg.slot_spacing));
        display.drawString(111, 50, String((int)millis() / 1000));
    }
    else if (sys->display_page >= 3)
    {

        int i = constrain(sys->display_page - 3, 0, cfg.lora_nodes - 1);
        bool iscurrent = (i + 1 == curr.id);
        const peer_t *peer = PeerManager::getSingleton()->getPeer(i);

        display.setFont(ArialMT_Plain_24);
        display.setTextAlignment(TEXT_ALIGN_LEFT);
        display.drawString(0, 0, String(peer_slotname[i + 1]));
        display.setFont(ArialMT_Plain_16);
        display.setTextAlignment(TEXT_ALIGN_RIGHT);

        if (iscurrent)
        {
            display.drawString(128, 0, String(curr.name));
        }
        else
        {
            display.drawString(128, 0, String(peer->name));
        }

        display.setTextAlignment(TEXT_ALIGN_LEFT);
        display.setFont(ArialMT_Plain_10);

        if (peer->id > 0 || iscurrent)
        {

            if (peer->lost > 0 && !iscurrent)
            {
                display.drawString(19, 0, "LOST");
            }
            else if (peer->lq == 0 && !iscurrent)
            {
                display.drawString(19, 0, "x");
            }
            else if (peer->lq == 1)
            {
                display.drawXbm(19, 2, 8, 8, icon_lq_1);
            }
            else if (peer->lq == 2)
            {
                display.drawXbm(19, 2, 8, 8, icon_lq_2);
            }
            else if (peer->lq == 3)
            {
                display.drawXbm(19, 2, 8, 8, icon_lq_3);
            }
            else if (peer->lq == 4)
            {
                display.drawXbm(19, 2, 8, 8, icon_lq_4);
            }

            if (iscurrent)
            {
                display.drawString(19, 0, "HOST");
                display.drawString(19, 12, String(host_name[curr.host]));
            }
            else
            {
                if (peer->lost == 0 && peer->rssi != 0)
                {
                    display.drawString(28, 0, String(peer->rssi) + "dBm");
                }
            }

            if (iscurrent)
            {
                display.drawString(50, 12, String(host_state[curr.state]));
            }

            display.setTextAlignment(TEXT_ALIGN_RIGHT);

            if (iscurrent)
            {
                display.drawString(128, 24, "LA " + String(loc.lat, 5));
                display.drawString(128, 34, "LO " + String(loc.lon, 5));
            }
            else
            {
                display.drawString(128, 24, "LA " + String((float)peer->gps.lat / 1000000.0, 5));
                display.drawString(128, 34, "LO " + String((float)peer->gps.lon / 1000000.0, 5));
            }

            display.setTextAlignment(TEXT_ALIGN_LEFT);

            if (iscurrent)
            {
                display.drawString(0, 24, "A " + String(loc.alt) + "m");
                display.drawString(0, 34, "S " + String(loc.groundSpeed / 100) + "m/s");
                display.drawString(0, 44, "C " + String(loc.groundCourse) + "°");
            }
            else
            {
                display.drawString(0, 24, "A " + String(peer->gps.alt) + "m");
                display.drawString(0, 34, "S " + String(peer->gps.groundSpeed / 100) + "m/s");
                display.drawString(0, 44, "C " + String(peer->gps.groundCourse / 10) + "°");
            }

            if (peer->gps.lat != 0 && peer->gps.lon != 0 && loc.lat != 0 && loc.lon != 0 && !iscurrent)
            {
                display.drawString(0, 54, "R " + String(peer->relalt) + "m");
                display.setTextAlignment(TEXT_ALIGN_RIGHT);
                display.drawString(128, 54, "B " + String(peer->direction) + "°");
                display.drawString(128, 44, "D " + String((int)peer->distance) + "m");
                display.setTextAlignment(TEXT_ALIGN_LEFT);
            }

            if (iscurrent)
            {
                msp_analog_t analogValues = MSPManager::getSingleton()->getAnalogValues();
                display.drawString(0, 54, String((float)analogValues.vbat / 10) + "v");
                display.drawString(50, 54, String((int)analogValues.mAhDrawn) + "mah");
            }

            display.setTextAlignment(TEXT_ALIGN_RIGHT);
        }
        else
        {
            display.drawString(35, 7, "SLOT IS EMPTY");
            sys->display_page++;
        }
    }
    display.display();
#endif
}

void display_draw_intro()
{
#ifdef HAS_OLED
    display_init();
    display.clear();
    display.setFont(ArialMT_Plain_16);
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    // display.drawString(0, 0, "ESP32");
    display.drawString(0, 21, PRODUCT_NAME);
    display.setTextAlignment(TEXT_ALIGN_RIGHT);
    display.drawString(127, 0, String(VERSION));
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    display.setFont(ArialMT_Plain_10);
    display.drawString(0, 52, "Press for full reset");
    display.display();
#endif
}

void display_draw_startup()
{
#ifdef HAS_OLED
    display.clear();
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_RIGHT);
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    display.drawString(0, 9, String(CFG_TARGET_NAME));
#ifdef LORA_BAND
    display.drawString(35, 9, String(LORA_BAND) + "MHz");
#endif
    display.drawString(0, 29, "HOST:");
    display.display();
#endif
}

void display_draw_scan(system_t *sys)
{
#ifdef HAS_OLED

    if (curr.host != HOST_NONE)
    {
        msp_fc_version_t version = MSPManager::getSingleton()->getFCVersion();
        display.drawString(35, 29, String(host_name[curr.host]) + " " + String(version.versionMajor) + "." + String(version.versionMinor) + "." + String(version.versionPatchLevel));
    }
    else
    {
        display.drawString(35, 29, String(host_name[curr.host]));
    }
    display.drawProgressBar(0, 0, 63, 6, 100);
    display.drawString(0, 39, "SCAN:");
    display.display();
#endif
}

void display_draw_progressbar(int progress)
{
#ifdef HAS_OLED
    display.drawProgressBar(0, 0, 63, 6, progress);
    display.display();
#endif
}

void display_on()
{
#ifdef HAS_OLED
    display.displayOn();
#endif
}

void display_off()
{
#ifdef HAS_OLED
    display.displayOff();
#endif
}

void display_draw_peername(int position)
{
#ifdef HAS_OLED
    display.drawString(28 + position * 8, 39, String(peer_slotname[position]));
    display.display();
#endif
}

void display_draw_scan_progressbar(int progress)
{
#ifdef HAS_OLED
    display.drawProgressBar(64, 0, 63, 6, progress);
    display.display();
#endif
}