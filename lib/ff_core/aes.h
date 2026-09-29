#pragma once
//
// AES-128 block encryption.
//
// Encrypt-only on purpose: CCM (see ccm.h) never calls the inverse cipher, so
// the decrypt tables and InvMixColumns would be dead weight on an ESP8285. This
// is the whole of the block-cipher surface v2 needs.
//
// Written out rather than pulled from a platform crypto library so the same
// bytes run on ESP32, ESP8266 and the host test runner. Verified against the
// FIPS-197 Appendix C.1 vector.
//
// Not constant-time: it uses a table-driven S-box, so it is vulnerable to cache
// timing attacks by an attacker running code on the same MCU. On a flight
// controller peripheral with no untrusted code execution that is not a
// meaningful threat, and the alternative (bitsliced AES) costs far more than
// the 5 blocks per packet budget allows.
//
#include <cstdint>

namespace ff {

constexpr int kAesBlockSize = 16;
constexpr int kAesKeySize = 16;    // AES-128
constexpr int kAesRounds = 10;
constexpr int kAesRoundKeyBytes = kAesBlockSize * (kAesRounds + 1);  // 176

class Aes128 {
public:
    // Expands `key` into the round-key schedule. Safe to call again to rekey.
    void setKey(const uint8_t key[kAesKeySize]);

    // Encrypts one block in place.
    void encryptBlock(uint8_t block[kAesBlockSize]) const;

private:
    uint8_t rk_[kAesRoundKeyBytes] = {0};
};

}  // namespace ff
