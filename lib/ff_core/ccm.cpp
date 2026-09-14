#include "ccm.h"

namespace ff {

namespace {

// AAD lengths below 0xFF00 encode as two big-endian bytes. Longer associated
// data needs the 6- and 10-byte encodings, which nothing here produces.
constexpr size_t kMaxAadLen = 0xFF00;

bool paramsValid(size_t nonce_len, size_t data_len, size_t aad_len, size_t tag_len) {
    if (nonce_len < 7 || nonce_len > 13) {
        return false;
    }
    if (tag_len < 4 || tag_len > 16 || (tag_len % 2) != 0) {
        return false;
    }
    if (aad_len >= kMaxAadLen) {
        return false;
    }
    // The length field must be wide enough to hold data_len.
    const size_t l = 15 - nonce_len;
    if (l < 8) {
        uint64_t limit = 1;
        limit <<= (8 * l);
        if (static_cast<uint64_t>(data_len) >= limit) {
            return false;
        }
    }
    return true;
}

// Running CBC-MAC over zero-padded blocks.
class CbcMac {
public:
    CbcMac(const Aes128& aes) : aes_(aes) {}

    void feed(const uint8_t* data, size_t len) {
        for (size_t i = 0; i < len; i++) {
            x_[fill_++] ^= data[i];
            if (fill_ == kAesBlockSize) {
                aes_.encryptBlock(x_);
                fill_ = 0;
            }
        }
    }

    // Pad the partial block with zeros and encrypt it, if one is open.
    void flushBlock() {
        if (fill_ != 0) {
            aes_.encryptBlock(x_);
            fill_ = 0;
        }
    }

    const uint8_t* state() const { return x_; }

private:
    const Aes128& aes_;
    uint8_t x_[kAesBlockSize] = {0};
    size_t fill_ = 0;
};

// Builds counter block A_i for the CTR keystream.
void buildCtrBlock(uint8_t out[kAesBlockSize], const uint8_t* nonce, size_t nonce_len,
                   uint32_t counter) {
    const size_t l = 15 - nonce_len;
    out[0] = static_cast<uint8_t>(l - 1);  // flags: only the length field
    for (size_t i = 0; i < nonce_len; i++) {
        out[1 + i] = nonce[i];
    }
    for (size_t i = 0; i < l; i++) {
        out[kAesBlockSize - 1 - i] = static_cast<uint8_t>(counter >> (8 * i));
    }
}

// Computes the unencrypted authentication tag (the CBC-MAC over B0 || AAD ||
// payload) and the S0 keystream block that masks it.
void computeMac(const Aes128& aes, const uint8_t* nonce, size_t nonce_len, const uint8_t* aad,
                size_t aad_len, const uint8_t* plaintext, size_t data_len, size_t tag_len,
                uint8_t mac_out[kAesBlockSize]) {
    const size_t l = 15 - nonce_len;

    // B0: flags, nonce, then the payload length in the last L bytes.
    uint8_t b0[kAesBlockSize] = {0};
    b0[0] = static_cast<uint8_t>((aad_len > 0 ? 0x40 : 0x00) |
                                 (((tag_len - 2) / 2) << 3) | (l - 1));
    for (size_t i = 0; i < nonce_len; i++) {
        b0[1 + i] = nonce[i];
    }
    for (size_t i = 0; i < l; i++) {
        b0[kAesBlockSize - 1 - i] = static_cast<uint8_t>(data_len >> (8 * i));
    }

    CbcMac mac(aes);
    mac.feed(b0, kAesBlockSize);

    if (aad_len > 0) {
        const uint8_t len_be[2] = {static_cast<uint8_t>(aad_len >> 8),
                                   static_cast<uint8_t>(aad_len & 0xFF)};
        mac.feed(len_be, 2);
        mac.feed(aad, aad_len);
        mac.flushBlock();  // AAD is zero-padded to a block boundary
    }

    mac.feed(plaintext, data_len);
    mac.flushBlock();  // so is the payload

    for (size_t i = 0; i < kAesBlockSize; i++) {
        mac_out[i] = mac.state()[i];
    }
}

// XORs the CTR keystream over `data`, starting at counter block 1.
void ctrCrypt(const Aes128& aes, const uint8_t* nonce, size_t nonce_len, uint8_t* data,
              size_t data_len) {
    uint8_t ks[kAesBlockSize];
    uint32_t counter = 1;
    size_t offset = 0;
    while (offset < data_len) {
        buildCtrBlock(ks, nonce, nonce_len, counter++);
        aes.encryptBlock(ks);
        const size_t take =
            (data_len - offset < kAesBlockSize) ? (data_len - offset) : kAesBlockSize;
        for (size_t i = 0; i < take; i++) {
            data[offset + i] ^= ks[i];
        }
        offset += take;
    }
}

// S0, the keystream block that masks the tag.
void maskTag(const Aes128& aes, const uint8_t* nonce, size_t nonce_len, uint8_t* mac,
             size_t tag_len) {
    uint8_t s0[kAesBlockSize];
    buildCtrBlock(s0, nonce, nonce_len, 0);
    aes.encryptBlock(s0);
    for (size_t i = 0; i < tag_len; i++) {
        mac[i] ^= s0[i];
    }
}

}  // namespace

bool ccmEncrypt(const Aes128& aes, const uint8_t* nonce, size_t nonce_len, const uint8_t* aad,
                size_t aad_len, uint8_t* data, size_t data_len, uint8_t* tag, size_t tag_len) {
    if (!paramsValid(nonce_len, data_len, aad_len, tag_len)) {
        return false;
    }
    // The MAC covers the plaintext, so it must be computed before CTR runs.
    uint8_t mac[kAesBlockSize];
    computeMac(aes, nonce, nonce_len, aad, aad_len, data, data_len, tag_len, mac);
    maskTag(aes, nonce, nonce_len, mac, tag_len);
    ctrCrypt(aes, nonce, nonce_len, data, data_len);
    for (size_t i = 0; i < tag_len; i++) {
        tag[i] = mac[i];
    }
    return true;
}

bool ccmDecrypt(const Aes128& aes, const uint8_t* nonce, size_t nonce_len, const uint8_t* aad,
                size_t aad_len, uint8_t* data, size_t data_len, const uint8_t* tag,
                size_t tag_len) {
    if (!paramsValid(nonce_len, data_len, aad_len, tag_len)) {
        return false;
    }
    ctrCrypt(aes, nonce, nonce_len, data, data_len);  // CTR is its own inverse
    uint8_t mac[kAesBlockSize];
    computeMac(aes, nonce, nonce_len, aad, aad_len, data, data_len, tag_len, mac);
    maskTag(aes, nonce, nonce_len, mac, tag_len);

    // Constant-time compare: a byte-at-a-time early return would leak how much
    // of a forged tag was correct, which is enough to forge one byte at a time.
    uint8_t diff = 0;
    for (size_t i = 0; i < tag_len; i++) {
        diff |= static_cast<uint8_t>(mac[i] ^ tag[i]);
    }
    return diff == 0;
}

}  // namespace ff
