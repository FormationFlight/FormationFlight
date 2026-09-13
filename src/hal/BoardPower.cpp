#include "BoardPower.h"

#include <Arduino.h>

#include "log.h"

#ifdef TARGET_TBEAM
// Wire first: the AXP202X library's header takes a TwoWire& and relies on
// Wire.h already being reachable.
#include <Wire.h>

#include <axp20x.h>

namespace {
// The T-Beam wires the PMIC to the same I2C pins as the OLED.
constexpr int kPmicSda = 21;
constexpr int kPmicScl = 22;

// Driven directly rather than through a wrapper library: the wrapper keeps the
// AXP object private, which hides every reading beyond the two voltages, and a
// power debug view that cannot show current or charge state is not much of one.
AXP20X_Class g_axp;
}  // namespace
#endif

namespace ff {

void BoardPower::begin() {
#ifdef TARGET_TBEAM
    Wire.begin(kPmicSda, kPmicScl);
    present_ = (g_axp.begin(Wire, AXP192_SLAVE_ADDRESS) == AXP_PASS);
    if (!present_) {
        // Worth saying out loud. On this board a missing PMIC is not a missing
        // feature: it means the GPS and the LoRa radio have no power, and
        // everything downstream then fails in a way that looks like something
        // else entirely.
        FF_LOGE("AXP192 not found: GPS and LoRa rails are unpowered");
        return;
    }

    g_axp.setPowerOutPut(AXP192_LDO3, AXP202_ON);   // GPS
    g_axp.setPowerOutPut(AXP192_LDO2, AXP202_ON);   // LoRa
    g_axp.setPowerOutPut(AXP192_DCDC1, AXP202_ON);  // peripherals, OLED
    g_axp.setPowerOutPut(AXP192_DCDC2, AXP202_ON);
    // AXP192_DCDC3 is the ESP32's own supply. Switching it off would power down
    // the processor executing this line, so it is never touched.

    // Without these the voltage and current registers read zero for ever, which
    // looks exactly like a flat battery.
    g_axp.adc1Enable(AXP202_BATT_VOL_ADC1 | AXP202_BATT_CUR_ADC1 | AXP202_VBUS_VOL_ADC1 |
                         AXP202_VBUS_CUR_ADC1,
                     true);
    g_axp.adc2Enable(AXP202_TEMP_MONITORING_ADC2, true);
    g_axp.setAdcSamplingRate(AXP_ADC_SAMPLING_RATE_25HZ);

    // Let the rails settle before anything talks to what is on them; the GPS in
    // particular is about to be baud-swept.
    delay(50);
    FF_LOGI("AXP192 up: GPS, LoRa and peripheral rails on");
#else
    present_ = false;
#endif
}

BoardPower::Reading BoardPower::read() const {
    Reading r;
#ifdef TARGET_TBEAM
    if (present_) {
        r.battery_present = g_axp.isBatteryConnect();
        r.charging = g_axp.isChargeing();
        r.usb_present = g_axp.isVBUSPlug();
        r.battery_v = r.battery_present ? g_axp.getBattVoltage() / 1000.0f : 0.0f;
        r.supply_v = g_axp.getVbusVoltage() / 1000.0f;
        r.charge_ma = g_axp.getBattChargeCurrent();
        r.discharge_ma = g_axp.getBattDischargeCurrent();
        r.pmic_temp_c = g_axp.getTemp();
        const int pct = g_axp.getBattPercentage();
        // The AXP192's fuel gauge is not calibrated for this board's cell and
        // returns values outside 0-100 when it has nothing to go on. Report
        // "unknown" rather than a number that looks authoritative and is not.
        r.battery_pct = (pct >= 0 && pct <= 100) ? static_cast<int16_t>(pct) : -1;
    }
#endif
    return r;
}

}  // namespace ff
