#ifdef BT_CONFIG
#include <Arduino.h>
#include "BTConfig.h"
#include <esp_system.h>
#include "BluetoothSerial.h"

BluetoothSerial SerialBT;

void initConfigInterface()
{
    SerialBT.begin((String) "ESP32");
}

void handleConfig()
{
    char incoming;
    static String message;
    if (SerialBT.available())
    {
        incoming = SerialBT.read();

        if (incoming != '\n')
        {
            message += String(incoming);
        }
        else
        {
            handleConfigMessage(SerialBT, message);
            message = "";
        }
    }
}

void debugPrintln(String input)
{
    SerialBT.println(input);
}
#endif