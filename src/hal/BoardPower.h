#pragma once
#include <cstdint>

namespace ff {

// Board power rails.
//
// On most targets the ESP powers everything directly and every call here is a
// no-op. On the Lilygo T-Beam an AXP192 PMIC sits between the battery and the
// GPS, the LoRa radio and the peripheral rail, all of which come up OFF. Without
// this the board boots, the firmware runs, the GPS driver happily baud-sweeps a
// module that has no power, and nothing in the logs says why.
//
// Deliberately not selective: every rail the board has is switched on, rather
// than only the ones the current config wants. The few milliamps saved by
// leaving the LoRa rail dark when `radios.lora_enabled` is false are not worth
// the failure mode, which is a radio that initialises, reports itself healthy
// and transmits into an unpowered PA.
//
// This runs before anything else in setup(), because the SPI bring-up of a LoRa
// module on an unpowered rail is not a well-defined operation.
class BoardPower {
public:
    void begin();

    // True when this board has a PMIC and it answered on I2C. False on a board
    // that has none, which is normal, and false on a T-Beam whose I2C is not
    // working, which is not: the GPS will have no power.
    bool present() const { return present_; }

    // A snapshot of everything the PMIC can tell us. Every field is zero or
    // false when there is no PMIC. Read together rather than one call at a
    // time: each one is an I2C transaction.
    struct Reading {
        float battery_v = 0.0f;
        float supply_v = 0.0f;
        // Charge and discharge are reported separately by the PMIC, so a node
        // on USB with a battery attached shows both a supply voltage and a
        // charge current, which is the state people most often misread.
        float charge_ma = 0.0f;
        float discharge_ma = 0.0f;
        float pmic_temp_c = 0.0f;
        int16_t battery_pct = -1;  // -1 when the PMIC will not estimate one
        bool battery_present = false;
        bool charging = false;
        bool usb_present = false;
    };
    Reading read() const;

    float batteryVolts() const { return read().battery_v; }
    float supplyVolts() const { return read().supply_v; }

private:
    bool present_ = false;
};

}  // namespace ff
