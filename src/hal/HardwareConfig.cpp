#include "HardwareConfig.h"

#include <Arduino.h>
#include <LittleFS.h>

#include <cstring>
#include <memory>

#if defined(PLATFORM_ESP32)
#include <esp_ota_ops.h>
#endif

#include "log.h"

namespace ff {

namespace {

// The ELRS trailer (UnifiedConfiguration.appendToFirmware), immediately after
// the application image.
constexpr size_t kProductNameSize = 128;
constexpr size_t kDeviceNameSize = 16;
constexpr size_t kOptionsSize = 512;
constexpr size_t kLayoutSize = 2048;
constexpr size_t kLayoutOffset = kProductNameSize + kDeviceNameSize + kOptionsSize;

constexpr const char* kLayoutPath = "/hardware.json";

void compiledLayout(HardwareLayout& hw) {
    bool any = false;
#if defined(SERIAL_PIN_RX) && defined(SERIAL_PIN_TX)
    hw.serial_rx = SERIAL_PIN_RX;
    hw.serial_tx = SERIAL_PIN_TX;
    any = true;
#endif
#ifdef LORA_PIN_CS
    hw.radio_sck = LORA_PIN_SCK;
    hw.radio_miso = LORA_PIN_MISO;
    hw.radio_mosi = LORA_PIN_MOSI;
    hw.radio[0].nss = LORA_PIN_CS;
    hw.radio[0].rst = LORA_PIN_RST;
    any = true;
#endif
#ifdef LORA_PIN_DIO0
    hw.radio[0].dio0 = LORA_PIN_DIO0;
#endif
#ifdef LORA_PIN_DIO
    hw.radio[0].dio1 = LORA_PIN_DIO;
#endif
#ifdef LORA_PIN_BUSY
    hw.radio[0].busy = LORA_PIN_BUSY;
#endif
#ifdef LORA_DEFAULT_BAND
    hw.default_band = LORA_DEFAULT_BAND;
#endif
#ifdef LORA_POWER
    hw.power_dbm = LORA_POWER;
    hw.has_power = true;
#endif
#ifdef IO_LED_PIN
    hw.led = IO_LED_PIN;
#endif
#ifdef PIN_BUTTON
    hw.button = PIN_BUTTON;
#endif
#ifdef CFG_TARGET_FULLNAME
    snprintf(hw.product_name, sizeof(hw.product_name), "%s", CFG_TARGET_FULLNAME);
#endif
    if (any) {
        hw.source = LayoutSource::Compiled;
    }
}

bool loadFromFile(HardwareLayout& hw) {
    File f = LittleFS.open(kLayoutPath, "r");
    if (!f || f.isDirectory()) {
        if (f) {
            f.close();
        }
        return false;
    }
    String json = f.readString();
    f.close();
    const char* err = nullptr;
    if (!parseLayoutJson(json.c_str(), json.length(), hw, &err)) {
        // Not fatal, and not silently ignored either: someone put this file
        // there on purpose, and is about to wonder why it did nothing.
        FF_LOGE("%s does not parse (%s): ignoring it", kLayoutPath, err);
        return false;
    }
    hw.source = LayoutSource::File;
    return true;
}

uint32_t trailerAddress() {
    uint32_t base = 0;  // ESP8266: the sketch is always at the start of flash
#if defined(PLATFORM_ESP32)
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running != nullptr) {
        base = running->address;
    }
#endif
    // The image size including its padding and, on ESP32, its SHA-256: the
    // same byte scripts/elrs_targets.py findFirmwareEnd() computes.
    return base + ESP.getSketchSize();
}

// An unconfigured image has erased flash (0xFF) or nothing (0x00) here, and
// either way no printable text.
bool looksLikeText(const char* s) {
    return s[0] >= 0x20 && s[0] < 0x7F;
}

bool loadFromTrailer(HardwareLayout& hw) {
    const uint32_t addr = trailerAddress();

    // ESP.flashRead wants 4-byte aligned buffers and lengths; both sizes are.
    uint32_t names[(kProductNameSize + kDeviceNameSize) / 4];
    if (!ESP.flashRead(addr, names, sizeof(names))) {
        return false;
    }
    const char* raw = reinterpret_cast<const char*>(names);
    if (!looksLikeText(raw)) {
        return false;
    }

    // The options block first, and whether or not a layout follows: the web
    // installer appends options (band, first-boot name and passphrase) to the
    // hand-written targets too, whose pins are compiled in. On the heap, and
    // only for as long as the parse: RAM is short on an ESP8285.
    {
        std::unique_ptr<uint32_t[]> options(new uint32_t[kOptionsSize / 4 + 1]);
        if (ESP.flashRead(addr + kProductNameSize + kDeviceNameSize, options.get(),
                          kOptionsSize)) {
            char* opts = reinterpret_cast<char*>(options.get());
            opts[kOptionsSize] = '\0';
            if (opts[0] == '{') {
                parseOptionsJson(opts, strnlen(opts, kOptionsSize), hw);
            }
        }
    }

    std::unique_ptr<uint32_t[]> layout(new uint32_t[kLayoutSize / 4 + 1]);
    if (!ESP.flashRead(addr + kLayoutOffset, layout.get(), kLayoutSize)) {
        return false;
    }
    char* json = reinterpret_cast<char*>(layout.get());
    json[kLayoutSize] = '\0';
    if (json[0] != '{') {
        // Named but no layout: a legacy target configured by the installer, or
        // ELRS' 2 KB of zeroes for a target with no layout file. The compiled
        // pins stand.
        return false;
    }

    const char* err = nullptr;
    if (!parseLayoutJson(json, strnlen(json, kLayoutSize), hw, &err)) {
        FF_LOGE("firmware layout does not parse (%s)", err);
        return false;
    }
    std::memcpy(hw.product_name, raw, kProductNameSize);
    hw.product_name[kProductNameSize] = '\0';
    std::memcpy(hw.device_name, raw + kProductNameSize, kDeviceNameSize);
    hw.device_name[kDeviceNameSize] = '\0';
    hw.source = LayoutSource::Trailer;
    return true;
}

}  // namespace

void loadHardwareLayout(HardwareLayout& out) {
    out = HardwareLayout{};
    compiledLayout(out);
    loadFromTrailer(out);
    loadFromFile(out);
}

}  // namespace ff
