#pragma once
//
// The v2 frame cipher: AES-128-CCM over every packet that goes on the air.
//
// v1 used XTS-AES with a static tweak, which authenticated nothing (a flipped
// bit produced garbage plaintext that a CRC8 was then asked to catch) and forced
// every packet to be exactly 16 bytes. v2 replaces it with an AEAD: one pass
// gives confidentiality, integrity and sender authentication, and the packet can
// be any length.
//
// Frame layout. The first ten bytes travel in the clear because the receiver
// needs them before it can derive the nonce, and they are bound into the tag as
// associated data, so clear does not mean unprotected - altering any of them
// invalidates the frame.
//
//   offset  size  field
//   0       1     protocol version   |
//   1       1     packet type        | plaintext header, authenticated as AAD
//   2..5    4     sender UID (LE)    |
//   6..9    4     frame counter (LE) |
//   10..    N     ciphertext of the packet payload
//   end     6     authentication tag
//
// Overhead is 10 bytes over the plaintext packet: 4 for the counter and 6 for
// the tag. A position beacon goes from 21 to 31 bytes, which at SF6/250kHz is
// roughly 2 ms of extra airtime - the rate controller absorbs it, and a 48-bit
// tag still costs a forger 2^48 tries for a single accepted packet.
//
// What is deliberately NOT hidden: the UID. It has to be readable to pick the
// nonce, so an observer can tell that a particular node is transmitting and
// count the fleet. Position, altitude, speed, course, craft name and state are
// all encrypted. Hiding the UID would require trial-decrypting against every
// known peer on every received frame, which an ESP8285 cannot afford.
//
// Replay: the counter is strictly increasing per sender. A frame whose counter
// is not above the last one accepted from that UID is dropped, so a recorded
// packet cannot be re-injected. Because the counter restarts at boot, a peer
// that has been silent longer than the resync window (see kReplayResyncMs) is
// allowed to restart its sequence - otherwise a rebooted aircraft could never
// rejoin. Inside that window a replay is rejected; outside it, an attacker who
// waits out the window can replay a frame once before the real node's next
// beacon moves the counter past it. The exposure is bounded to stale position
// data from a node that has been gone for seconds, which Follow's own
// maxTargetDistM check already refuses to chase.
//
#include <cstddef>
#include <cstdint>

#include "aes.h"
#include "node.h"
#include "peer_table.h"

namespace ff {

// Clear, authenticated header: version, type, uid, counter.
constexpr size_t kFrameHeaderLen = 10;
constexpr size_t kFrameCounterOffset = 6;
// 48 bits. Sized against airtime rather than paranoia: a forger gets one try per
// transmitted packet, and at 10 Hz a 2^48 search is not a flight-duration
// attack. Bump to 8 only if you are prepared to pay the airtime on every beacon.
constexpr size_t kFrameTagLen = 6;
constexpr size_t kFrameOverhead = 4 + kFrameTagLen;  // counter + tag
static_assert(kFrameOverhead == kFrameCryptoOverhead,
              "protocol.h's frame sizing must match the real cipher overhead");
// CCM nonce: uid(4) || counter(4) || 5 zero bytes. 13 bytes puts L at 2.
constexpr size_t kNonceLen = 13;
// A sender silent for longer than this may restart its counter sequence.
// Deliberately several beacon intervals: long enough that a live node can never
// trip it, short enough that a genuine reboot rejoins the formation quickly.
constexpr uint32_t kReplayResyncMs = 10000;

// Derives the 128-bit group key from a passphrase: the first half of its
// SHA-256. An empty passphrase yields a fixed, publicly known key, which is
// interoperable but offers no secrecy - it exists so a fresh node talks to
// another fresh node out of the box, exactly like an unbound ExpressLRS link.
void deriveGroupKey(const char* passphrase, uint8_t out_key[kAesKeySize]);

class CcmCrypto : public ICrypto {
public:
    // uid is this node's own UID; it seeds the nonce for transmitted frames.
    void begin(uint32_t uid, const uint8_t key[kAesKeySize], uint32_t initial_counter);

    // Swap the group key without disturbing the counter (a config change).
    void setKey(const uint8_t key[kAesKeySize]);

    size_t encrypt(uint8_t* buf, size_t len, size_t cap) override;
    bool decrypt(uint8_t* buf, size_t len, size_t& out_len, uint32_t now_ms) override;
    bool lastRejectWasReplay() const override { return last_reject_replay_; }

    uint32_t txCounter() const { return counter_; }

    // Encrypts a frame as though it came from another node. This exists solely
    // for the traffic simulator (hal/SimRadio), which has to produce frames that
    // are indistinguishable from real ones all the way through the receive path:
    // same cipher, same nonce construction, same replay counter. Nothing on the
    // normal transmit path may call it -- a node that can forge its peers'
    // frames is exactly what the tag is there to prevent.
    size_t encryptAs(uint32_t uid, uint32_t counter, uint8_t* buf, size_t len, size_t cap);

    // Counts, for the status view: frames rejected because the tag did not
    // verify (wrong group key, corruption or forgery) and frames rejected as
    // replays.
    uint32_t badTagCount() const { return bad_tag_; }
    uint32_t replayCount() const { return replay_; }

private:
    struct ReplaySlot {
        uint32_t uid = 0;
        uint32_t counter = 0;
        uint32_t last_ms = 0;
        bool used = false;
    };

    // Returns the slot for `uid`, allocating or evicting the least recently used
    // one as needed. `fresh` is set when the slot was just created, i.e. this is
    // first contact and any counter value is acceptable.
    ReplaySlot* slotFor(uint32_t uid, uint32_t now_ms, bool& fresh);

    Aes128 aes_;
    uint32_t uid_ = 0;
    uint32_t counter_ = 0;
    uint32_t bad_tag_ = 0;
    uint32_t replay_ = 0;
    bool last_reject_replay_ = false;
    ReplaySlot replay_slots_[kMaxPeers];
};

}  // namespace ff
