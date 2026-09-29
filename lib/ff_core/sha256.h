#pragma once
//
// SHA-256.
//
// Used only to turn a human-typed group passphrase into a 128-bit AES key (see
// deriveGroupKey in crypto.h). Written out rather than taken from a platform
// library so key derivation is bit-identical on ESP32, ESP8266 and the host,
// and so the derivation can be unit-tested. Verified against the NIST
// one-block and two-block example vectors.
//
#include <cstddef>
#include <cstdint>

namespace ff {

constexpr size_t kSha256DigestSize = 32;

class Sha256 {
public:
    Sha256() { reset(); }
    void reset();
    void update(const uint8_t* data, size_t len);
    // Writes the digest and leaves the object finalized; call reset() to reuse.
    void finish(uint8_t out[kSha256DigestSize]);

private:
    void compress(const uint8_t block[64]);

    uint32_t h_[8];
    uint8_t buf_[64];
    size_t buf_len_;
    uint64_t total_bits_;
};

// One-shot convenience wrapper.
void sha256(const uint8_t* data, size_t len, uint8_t out[kSha256DigestSize]);

}  // namespace ff
