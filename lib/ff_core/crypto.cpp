#include "crypto.h"

#include "ccm.h"
#include "sha256.h"
#include "wire.h"

namespace ff {

namespace {

// Builds the CCM nonce for a frame: uid || counter || zero padding.
void buildNonce(uint32_t uid, uint32_t counter, uint8_t out[kNonceLen]) {
    uint8_t* p = out;
    wire::put_u32(p, uid);
    wire::put_u32(p, counter);
    for (size_t i = 8; i < kNonceLen; i++) {
        out[i] = 0;
    }
}

}  // namespace

void deriveGroupKey(const char* passphrase, uint8_t out_key[kAesKeySize]) {
    size_t len = 0;
    if (passphrase != nullptr) {
        while (passphrase[len] != '\0') {
            len++;
        }
    }
    uint8_t digest[kSha256DigestSize];
    sha256(reinterpret_cast<const uint8_t*>(passphrase), len, digest);
    for (int i = 0; i < kAesKeySize; i++) {
        out_key[i] = digest[i];
    }
}

void CcmCrypto::begin(uint32_t uid, const uint8_t key[kAesKeySize], uint32_t initial_counter) {
    uid_ = uid;
    counter_ = initial_counter;
    aes_.setKey(key);
    for (size_t i = 0; i < kMaxPeers; i++) {
        replay_slots_[i] = ReplaySlot{};
    }
}

void CcmCrypto::setKey(const uint8_t key[kAesKeySize]) { aes_.setKey(key); }

size_t CcmCrypto::encrypt(uint8_t* buf, size_t len, size_t cap) {
    // The plaintext packet is [6-byte header][payload]; the frame inserts the
    // counter after the header and appends the tag.
    if (len < kHeaderSize || cap < len + kFrameOverhead) {
        return 0;
    }
    const size_t payload_len = len - kHeaderSize;

    // Open a 4-byte gap for the counter, working backwards so the move is safe
    // in place.
    for (size_t i = payload_len; i > 0; i--) {
        buf[kFrameHeaderLen + i - 1] = buf[kHeaderSize + i - 1];
    }

    counter_++;
    uint8_t* cursor = buf + kFrameCounterOffset;
    wire::put_u32(cursor, counter_);

    uint8_t nonce[kNonceLen];
    buildNonce(uid_, counter_, nonce);

    if (!ccmEncrypt(aes_, nonce, kNonceLen, buf, kFrameHeaderLen, buf + kFrameHeaderLen,
                    payload_len, buf + kFrameHeaderLen + payload_len, kFrameTagLen)) {
        return 0;
    }
    return kFrameHeaderLen + payload_len + kFrameTagLen;
}

CcmCrypto::ReplaySlot* CcmCrypto::slotFor(uint32_t uid, uint32_t now_ms, bool& fresh) {
    // Search the whole table first: stopping early at the first free slot would
    // miss an existing entry for this uid further along and track it twice,
    // which would silently disable replay rejection for that sender.
    for (size_t i = 0; i < kMaxPeers; i++) {
        if (replay_slots_[i].used && replay_slots_[i].uid == uid) {
            fresh = false;
            return &replay_slots_[i];
        }
    }

    ReplaySlot* victim = nullptr;
    for (size_t i = 0; i < kMaxPeers; i++) {
        if (!replay_slots_[i].used) {
            victim = &replay_slots_[i];
            break;
        }
        if (victim == nullptr ||
            static_cast<int32_t>(replay_slots_[i].last_ms - victim->last_ms) < 0) {
            victim = &replay_slots_[i];
        }
    }

    // Not tracked yet: claim a free slot, or evict the least recently heard.
    *victim = ReplaySlot{};
    victim->uid = uid;
    victim->used = true;
    victim->last_ms = now_ms;
    fresh = true;
    return victim;
}

bool CcmCrypto::decrypt(uint8_t* buf, size_t len, size_t& out_len, uint32_t now_ms) {
    last_reject_replay_ = false;
    if (len < kFrameHeaderLen + kFrameTagLen) {
        bad_tag_++;
        return false;
    }
    const size_t payload_len = len - kFrameHeaderLen - kFrameTagLen;

    const uint8_t* uid_cursor = buf + 2;
    const uint32_t uid = wire::get_u32(uid_cursor);
    const uint8_t* ctr_cursor = buf + kFrameCounterOffset;
    const uint32_t counter = wire::get_u32(ctr_cursor);

    uint8_t nonce[kNonceLen];
    buildNonce(uid, counter, nonce);

    if (!ccmDecrypt(aes_, nonce, kNonceLen, buf, kFrameHeaderLen, buf + kFrameHeaderLen,
                    payload_len, buf + kFrameHeaderLen + payload_len, kFrameTagLen)) {
        bad_tag_++;
        return false;
    }

    // Authenticated. Now reject replays: the counter must advance, unless this
    // sender has been quiet long enough to have plausibly rebooted.
    bool fresh = false;
    ReplaySlot* slot = slotFor(uid, now_ms, fresh);
    const bool resync = (now_ms - slot->last_ms) > kReplayResyncMs;
    if (!fresh && !resync && counter <= slot->counter) {
        replay_++;
        last_reject_replay_ = true;
        return false;
    }
    slot->counter = counter;
    slot->last_ms = now_ms;

    // Close the counter gap so the caller sees the original plaintext packet.
    for (size_t i = 0; i < payload_len; i++) {
        buf[kHeaderSize + i] = buf[kFrameHeaderLen + i];
    }
    out_len = kHeaderSize + payload_len;
    return true;
}

}  // namespace ff
