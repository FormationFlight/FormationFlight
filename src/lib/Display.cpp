#include <Arduino.h>
#ifdef HAS_OLED
#include <SSD1306.h>
#endif
#include "Display.h"
#include "targets.h"
#include "ConfigStrings.h"
#include "pixel.h"
#include "Helpers.h"
#include "Peers/PeerManager.h"

#ifdef HAS_OLED
SSD1306 display(OLED_ADDRESS, OLED_SDA, OLED_SCL);
#endif

void display_init()
{
#ifdef HAS_OLED
    pinMode(16, OUTPUT);
    pinMode(2, OUTPUT);
    digitalWrite(16, LOW);
    delay(50);
    digitalWrite(16, HIGH);
    display.init();
    display.flipScreenVertically();
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_LEFT);
#endif
}

void display_draw_status(system_t *sys)
{
#ifdef HAS_OLED
    display.clear();
    int j = 0;
    int line;

    if (sys->display_page == 0)
    {

        if (sys->io_bt_enabled)
        {
            display.setFont(ArialMT_Plain_10);
            display.setTextAlignment(TEXT_ALIGN_LEFT);
            display.drawString(18, 0, "CONFIGURATION");
            display.drawString(0, 20, "Connect to the ESP32 AP");
            display.drawString(0, 30, "with a Bluetooth terminal");
            display.drawString(0, 40, "type CMD for commands");
        }
        else
        {
            display.setFont(ArialMT_Plain_24);
            display.setTextAlignment(TEXT_ALIGN_RIGHT);
            display.drawString(26, 11, String(curr.gps.numSat));
            display.drawString(13, 42, String(PeerManager::getSingleton()->count_active() + 1 - sys->lora_no_tx));
            display.drawString(125, 11, String(peer_slotname[curr.id]));
            display.setFont(ArialMT_Plain_10);
            display.drawString(126, 29, "_ _ _ _ _ _ _ _ _ _ _ _ _ _ _ ");
            display.drawString(107, 44, String(stats.percent_received));
            display.drawString(107, 54, String(sys->last_rssi));
            display.setTextAlignment(TEXT_ALIGN_CENTER);
            display.drawString(64, 0, String(sys->message));
            display.setTextAlignment(TEXT_ALIGN_LEFT);
            display.drawString(55, 12, String(curr.name));
            display.drawString(27, 23, "SAT");
            display.drawString(108, 44, "%E");
            display.drawString(35, 44, String(sys->pps) + "p/s");
            display.drawString(109, 54, "dB");
            display.drawString(55, 23, String(host_name[curr.host]));
            display.drawString(15, 44, "/" + String(cfg.lora_nodes));
            display.drawString(15, 54, String(loramode_name[cfg.lora_mode]));

            if (curr.gps.fixType == 1)
                display.drawString(27, 12, "2D");
            if (curr.gps.fixType == 2)
                display.drawString(27, 12, "3D");
        }
    }

    else if (sys->display_page == 1)
    {

        long pos[LORA_NODES_MAX];
        long diff;

        display.setFont(ArialMT_Plain_10);
        display.setTextAlignment(TEXT_ALIGN_LEFT);
        display.drawHorizontalLine(0, 11, 128);

        for (int i = 0; i < cfg.lora_nodes; i++)
        {
            peer_t *peer = PeerManager::getSingleton()->getPeer(i);
            if (peer->id > 0 && peer->lost == 0)
            {
                diff = sys->last_tx - peer->updated;
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

        int rect_l = stats.last_tx_duration * 128 / sys->lora_cycle;

        for (int i = 0; i < cfg.lora_nodes; i++)
        {

            display.setTextAlignment(TEXT_ALIGN_LEFT);
            peer_t *peer = PeerManager::getSingleton()->getPeer(i);

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
                    display.drawString(127, line, "x:" + String((int)((sys->last_tx - peer->updated) / 1000)) + "s");
                }
                else if (peer->lost == 2)
                { // Peer timed out, long
                    display.drawString(127, line, "L:" + String((int)((sys->last_tx - peer->updated) / 1000)) + "s");
                }
                else
                {
                    if (sys->last_tx > peer->updated)
                    {
                        display.drawString(119, line, String(sys->last_tx - peer->updated));
                        display.drawString(127, line, "-");
                    }
                    else
                    {
                        display.drawString(119, line, String(sys->lora_cycle + sys->last_tx - peer->updated));
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
        display.drawString(0, 0, "LORA TX");
        display.drawString(0, 10, "MSP");
        display.drawString(0, 20, "OLED");
        display.drawString(0, 30, "CYCLE");
        display.drawString(0, 40, "SLOTS");
        display.drawString(0, 50, "UPTIME");

        display.drawString(112, 0, "ms");
        display.drawString(112, 10, "ms");
        display.drawString(112, 20, "ms");
        display.drawString(112, 30, "ms");
        display.drawString(112, 40, "ms");
        display.drawString(112, 50, "s");

        display.setTextAlignment(TEXT_ALIGN_RIGHT);
        display.drawString(111, 0, String(stats.last_tx_duration));
        display.drawString(111, 10, String(stats.last_msp_duration[0]) + " / " + String(stats.last_msp_duration[1]));
        display.drawString(111, 20, String(stats.last_oled_duration));
        display.drawString(111, 30, String(sys->lora_cycle));
        display.drawString(111, 40, String(cfg.lora_nodes) + " x " + String(cfg.slot_spacing));
        display.drawString(111, 50, String((int)millis() / 1000));
    }
    else if (sys->display_page >= 3)
    {

        int i = constrain(sys->display_page - 3, 0, cfg.lora_nodes - 1);
        bool iscurrent = (i + 1 == curr.id);
        peer_t *peer = PeerManager::getSingleton()->getPeer(i);


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
                    display.drawString(28, 0, "-" + String(peer->rssi) + "dBm");
                }
            }

            if (iscurrent)
            {
                display.drawString(50, 12, String(host_state[curr.state]));
            }

            display.setTextAlignment(TEXT_ALIGN_RIGHT);

            if (iscurrent)
            {
                display.drawString(128, 24, "LA " + String((float)curr.gps.lat / 10000000, 5));
                display.drawString(128, 34, "LO " + String((float)curr.gps.lon / 10000000, 5));
            }
            else
            {
                display.drawString(128, 24, "LA " + String((float)peer->gps_rec.lat / 10000000, 5));
                display.drawString(128, 34, "LO " + String((float)peer->gps_rec.lon / 10000000, 5));
            }

            display.setTextAlignment(TEXT_ALIGN_LEFT);

            if (iscurrent)
            {
                display.drawString(0, 24, "A " + String(curr.gps.alt) + "m");
                display.drawString(0, 34, "S " + String(curr.gps.groundSpeed / 100) + "m/s");
                display.drawString(0, 44, "C " + String(curr.gps.groundCourse / 10) + "°");
            }
            else
            {
                display.drawString(0, 24, "A " + String(peer->gps_rec.alt) + "m");
                display.drawString(0, 34, "S " + String(peer->gps_rec.groundSpeed / 100) + "m/s");
                display.drawString(0, 44, "C " + String(peer->gps_rec.groundCourse / 10) + "°");
            }

            if (peer->gps.lat != 0 && peer->gps.lon != 0 && curr.gps.lat != 0 && curr.gps.lon != 0 && !iscurrent)
            {

                double lat1 = (double)curr.gps.lat / 10000000;
                double lon1 = (double)curr.gps.lon / 10000000;
                double lat2 = (double)peer->gps_rec.lat / 10000000;
                double lon2 = (double)peer->gps_rec.lon / 10000000;

                peer->distance = gpsDistanceBetween(lat1, lon1, lat2, lon2);
                peer->direction = gpsCourseTo(lat1, lon1, lat2, lon2);
                peer->relalt = peer->gps_rec.alt - curr.gps.alt;

                display.drawString(0, 54, "R " + String(peer->relalt) + "m");
                display.drawString(50, 54, "B " + String(peer->direction) + "°");
                display.setTextAlignment(TEXT_ALIGN_RIGHT);
                display.drawString(128, 44, "D " + String((int)peer->distance) + "m");
                display.setTextAlignment(TEXT_ALIGN_LEFT);
            }

            if (iscurrent)
            {
                display.drawString(0, 54, String((float)curr.fcanalog.vbat / 10) + "v");
                display.drawString(50, 54, String((int)curr.fcanalog.mAhDrawn) + "mah");
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
    display.drawString(0, 0, "ESP32");
    display.drawString(0, 17, "RADAR");
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
    display.drawString(0, 52, "Press to start BT config");
    display.setTextAlignment(TEXT_ALIGN_RIGHT);
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    display.drawString(0, 9, String(cfg.target_name));
    display.drawString(35, 9, String(cfg.lora_band) + "MHz");
    display.drawString(0, 19, "MODE:");
    display.drawString(35, 19, String(loramode_name[cfg.lora_mode]));
    display.drawString(0, 29, "HOST:");
    display.display();
#endif
}

void display_draw_clearconfig()
{
#ifdef HAS_OLED
    display.clear();
    display.setFont(ArialMT_Plain_10);
    display.drawString(0, 52, "Press to start BT config");
    display.setTextAlignment(TEXT_ALIGN_RIGHT);
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    display.drawString(0, 9, String(cfg.target_name));
    display.drawString(35, 9, String(cfg.lora_band) + "MHz");
    display.drawString(0, 19, "MODE:");
    display.drawString(35, 19, String(loramode_name[cfg.lora_mode]));
    display.drawString(0, 29, "HOST:");
    display.display();
#endif
}

void display_draw_scan(system_t *sys)
{
#ifdef HAS_OLED

    if (curr.host != HOST_NONE)
    {
        display.drawString(35, 29, String(host_name[curr.host]) + " " + String(curr.fcversion.versionMajor) + "." + String(curr.fcversion.versionMinor) + "." + String(curr.fcversion.versionPatchLevel));
    }
    else
    {
        display.drawString(35, 29, String(host_name[curr.host]));
    }
    if (sys->io_bt_enabled)
    {
        display.drawString(105, 29, "+BT");
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