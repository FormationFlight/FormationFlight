#include "radio_hub.h"

namespace ff {

bool RadioHub::add(RadioDriver* driver) {
    if (driver == nullptr || count_ >= kMaxRadios) {
        return false;
    }
    radios_[count_++] = driver;
    return true;
}

void RadioHub::transmit(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < count_; i++) {
        if (radios_[i]->enabled()) {
            radios_[i]->transmit(data, len);
        }
    }
}

double RadioHub::airtimeMs(size_t payload_len) const {
    double max_airtime = 0.0;
    for (size_t i = 0; i < count_; i++) {
        if (!radios_[i]->enabled()) {
            continue;
        }
        double a = radios_[i]->airtimeMs(payload_len);
        if (a > max_airtime) {
            max_airtime = a;
        }
    }
    return max_airtime;
}

void RadioHub::service(Node& node) {
    for (size_t i = 0; i < count_; i++) {
        RadioDriver* r = radios_[i];
        r->serviceRx();
        RxFrame frame;
        while (r->popRx(frame)) {
            node.onReceive(frame.data, frame.len, frame.timestamp_ms, frame.rssi);
        }
    }
}

}  // namespace ff
