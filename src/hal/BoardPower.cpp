#include "BoardPower.h"

#include <Arduino.h>

#ifdef TARGET_TBEAM
// Wire first: the AXP202X library's header declares a TwoWire& parameter without
// including it itself.
#include <Wire.h>

#include <TBeamPower.h>

namespace {
// Default constructor uses the T-Beam's PMIC I2C pins (SDA 21, SCL 22). The
// sensor, battery and LED pins are left unset: this board gates those through
// the PMIC rails rather than through GPIO.
TBeamPower g_power;
}  // namespace
#endif

namespace ff {

void BoardPower::begin() {
#ifdef TARGET_TBEAM
    g_power.begin();
    present_ = g_power.hasAXP();
    if (!present_) {
        // Worth saying out loud. On this board a missing PMIC is not a missing
        // feature, it means the GPS and the LoRa radio have no power and
        // everything downstream is about to fail in a way that looks like
        // something else.
        Serial.println("[power] AXP192 not found; GPS and LoRa rails are unpowered");
        return;
    }
    g_power.power_peripherals(true);  // DCDC1/DCDC2 and both LDOs
    g_power.power_GPS(true);          // LDO3
    g_power.power_LoRa(true);         // LDO2
    // DCDC3 is the ESP32's own supply and is deliberately never touched.

    // The rails need a moment to come up before anything talks to what is on
    // them; the GPS in particular is about to be baud-swept.
    delay(50);
#else
    present_ = false;
#endif
}

float BoardPower::batteryVolts() const {
#ifdef TARGET_TBEAM
    if (present_) {
        return g_power.get_battery_voltage();
    }
#endif
    return 0.0f;
}

float BoardPower::supplyVolts() const {
#ifdef TARGET_TBEAM
    if (present_) {
        return g_power.get_supply_voltage();
    }
#endif
    return 0.0f;
}

}  // namespace ff
