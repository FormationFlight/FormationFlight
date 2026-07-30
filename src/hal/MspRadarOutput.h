#pragma once
#include <Stream.h>

#include "msp_radar.h"
#include "node.h"

namespace ff {

// Pushes this node's known peers out over a serial stream as MSP radar
// positions -- to an attached flight controller's OSD radar, or to any
// MSP-speaking ground-station software when used in listen-only (GCS) mode.
//
// Explicitly flushes after every write. That is the direct fix for a real,
// previously-observed bug: a listen-only node's received peers never reached
// the USB-attached ground station, because v1 only ever pushed MSP output as a
// side effect of the node's own radio transmit cycle -- which a listen-only node,
// by definition, never runs. Node schedules sendRadarPosition() on its own
// independent timer regardless of transmit activity (see IMspRadarSink in
// node.h); flushing here on every send is the belt-and-braces half of the fix,
// so delivery never silently depends on anything else happening to also touch
// this stream.
class MspRadarOutput : public IMspRadarSink {
public:
    void begin(Stream& stream, uint32_t peer_timeout_ms);

    void sendRadarPosition(uint8_t slot_id, const Peer& peer) override;

private:
    Stream* stream_ = nullptr;
    uint32_t peer_timeout_ms_ = 6000;
};

}  // namespace ff
