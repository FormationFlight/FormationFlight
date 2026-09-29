#pragma once
//
// MSP2_COMMON_SET_RADAR_POS packet builder.
//
// Feeds this node's known peers to an attached MSP host -- a flight controller's
// onboard OSD radar, or, in listen-only/ground-station use, any MSP-speaking
// ground-station software -- so it can display them. Pure: produces the exact
// MSP2 frame bytes ('$' 'X' '<' flag id(2) size(2) payload crc), with no serial
// or hardware dependency.
//
// This exists to fix a real bug class from v1: a listen-only (GCS) node has a
// peer table full of received positions but never transmits on the radio, and
// v1's MSP output schedule was only ever (re)armed from inside the radio-transmit
// code path -- so a listen-only node, which never transmits, never sent anything
// out over MSP either. The fix is architectural (see Node::IMspRadarSink in
// node.h): this output runs on its own independent scheduler timer, with no
// dependency on transmit activity at all.
//
#include <cstddef>
#include <cstdint>

#include "peer_table.h"

namespace ff {

constexpr uint16_t kMspSetRadarPos = 0x100B;  // MSP2_COMMON_SET_RADAR_POS

// Total frame size for one MSP2_COMMON_SET_RADAR_POS message (fixed payload).
constexpr size_t kMspRadarFrameSize = 28;  // 9 header/crc bytes + 19 payload bytes

// Coarse link-quality estimate (0..4) from how recently the peer was heard,
// relative to its own timeout window: peers heard within the last quarter of the
// timeout score 4, progressively lower as they approach the timeout, 0 once
// expired. This is an approximation, not a port of v1's tick-accumulated LQ.
uint8_t estimateRadarLq(const Peer& peer, uint32_t now_ms, uint32_t timeout_ms);

// Builds the full MSP2 frame for MSP2_COMMON_SET_RADAR_POS describing one peer.
// slot_id is the small integer ID (1..255) this sink assigns the peer for MSP
// purposes -- MSP radar consumers expect compact IDs, not our 32-bit UIDs.
// Returns the frame length (always kMspRadarFrameSize on success), or 0 if cap is
// too small.
size_t buildMspSetRadarPos(uint8_t slot_id, const Peer& peer, uint32_t now_ms,
                          uint32_t timeout_ms, uint8_t* buf, size_t cap);

}  // namespace ff
