#include "radio_hub.h"

namespace ff {

bool RadioHub::add(RadioDriver* driver) {
    if (driver == nullptr || count_ >= kMaxRadios) {
        return false;
    }
    radios_[count_++] = driver;
    return true;
}

bool RadioHub::radioEnabled(size_t index) const {
    return index < count_ && radios_[index]->enabled();
}

double RadioHub::airtimeMs(size_t index, size_t payload_len) const {
    if (index >= count_) {
        return 0.0;
    }
    return radios_[index]->airtimeMs(payload_len);
}

void RadioHub::transmit(size_t index, const uint8_t* data, size_t len) {
    if (index < count_ && radios_[index]->enabled()) {
        radios_[index]->transmit(data, len);
    }
}

void RadioHub::service(Node& node) {
    for (size_t i = 0; i < count_; i++) {
        RadioDriver* r = radios_[i];
        r->serviceRx();
        RxFrame frame;
        while (r->popRx(frame)) {
            node.onReceive(frame.data, frame.len, frame.timestamp_ms, frame.rssi, i);
        }
    }
}

}  // namespace ff
