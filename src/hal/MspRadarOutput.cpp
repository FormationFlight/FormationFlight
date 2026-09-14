#include "MspRadarOutput.h"

#include <Arduino.h>

namespace ff {

void MspRadarOutput::begin(Stream& stream, uint32_t peer_timeout_ms) {
    stream_ = &stream;
    peer_timeout_ms_ = peer_timeout_ms;
}

void MspRadarOutput::sendRadarPosition(uint8_t slot_id, const Peer& peer) {
    if (stream_ == nullptr) {
        return;
    }
    uint8_t buf[kMspRadarFrameSize];
    size_t n = buildMspSetRadarPos(slot_id, peer, millis(), peer_timeout_ms_, buf,
                                   sizeof(buf));
    if (n == 0) {
        return;
    }
    stream_->write(buf, n);
    stream_->flush();
}

}  // namespace ff
