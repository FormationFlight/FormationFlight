#pragma once
#include "node.h"

namespace ff {

// Plaintext passthrough, for bench work only.
//
// AES-CCM (ff_core/crypto.h) is what flies; this exists so a frame can be read
// off a logic analyzer or an SDR capture without a key, while bringing up a new
// radio. Selected by setting the group passphrase to the literal "none" (see
// ConfigStore), never by default. Frames produced here interoperate with
// nothing else, because the real cipher's counter and tag are absent.
class PassthroughCrypto : public ICrypto {
public:
    size_t encrypt(uint8_t* /*buf*/, size_t len, size_t /*cap*/) override {
        return len;
    }
    bool decrypt(uint8_t* /*buf*/, size_t len, size_t& out_len, uint32_t /*now_ms*/) override {
        out_len = len;
        return true;
    }
};

}  // namespace ff
