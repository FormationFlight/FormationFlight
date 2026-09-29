#pragma once
//
// AES-CCM authenticated encryption (RFC 3610).
//
// CCM is CTR mode for confidentiality plus CBC-MAC for integrity, both built
// from the same forward block cipher, which is why it fits an ESP8285 far better
// than GCM: no GF(2^128) multiply, no tables beyond the AES S-box, and the
// implementation below is ~120 lines.
//
// The tag length is a parameter because v2 pays for it in airtime on every
// beacon; see kFrameTagLen in crypto.h for the size actually flown and the
// reasoning behind it.
//
// Pure and host-testable. Verified against RFC 3610's packet vectors.
//
#include <cstddef>
#include <cstdint>

#include "aes.h"

namespace ff {

// Nonce length must be 7..13; the length field L is 15 - nonce_len, so a
// 13-byte nonce allows messages up to 65535 bytes. Tag length must be even and
// in 4..16. Both functions work in place on `data` and fail (returning false)
// only on parameter violations, never on message content.
bool ccmEncrypt(const Aes128& aes, const uint8_t* nonce, size_t nonce_len, const uint8_t* aad,
                size_t aad_len, uint8_t* data, size_t data_len, uint8_t* tag, size_t tag_len);

// Returns false if the parameters are invalid or the tag does not match. On a
// tag mismatch `data` has already been decrypted in place, so callers must treat
// its contents as untrusted garbage and discard them.
bool ccmDecrypt(const Aes128& aes, const uint8_t* nonce, size_t nonce_len, const uint8_t* aad,
                size_t aad_len, uint8_t* data, size_t data_len, const uint8_t* tag,
                size_t tag_len);

}  // namespace ff
