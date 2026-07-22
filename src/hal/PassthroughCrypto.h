#pragma once
#include "node.h"

namespace ff {

// Phase 1 placeholder: no confidentiality or integrity. This exists so the radio
// path can be brought up on the clean-break v2 wire format (variable-length
// packets that the old fixed-16-byte XTS cipher cannot carry). Phase 2 replaces
// it with an AES-CCM AEAD (nonce = UID + counter, truncated tag). v2 is
// unreleased, so no plaintext is ever exposed to a deployed fleet.
class PassthroughCrypto : public ICrypto {
public:
    size_t encrypt(uint8_t* /*buf*/, size_t len, size_t /*cap*/) override {
        return len;
    }
    bool decrypt(uint8_t* /*buf*/, size_t len, size_t& out_len) override {
        out_len = len;
        return true;
    }
};

}  // namespace ff
