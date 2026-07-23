#include "msp_parser.h"

namespace ff {

void MspParser::reset() {
    state_ = Idle;
    size_ = 0;
    id_ = 0;
    idx_ = 0;
    crc_ = 0;
    oversize_ = false;
}

bool MspParser::feed(uint8_t b) {
    switch (state_) {
        case Idle:
            if (b == '$') {
                state_ = HdrM;
            }
            break;

        case HdrM:
            state_ = (b == 'M') ? HdrDir : (b == '$' ? HdrM : Idle);
            break;

        case HdrDir:
            // Responses are '>'. Anything else (e.g. '!' error, or noise) resyncs.
            state_ = (b == '>') ? Size : (b == '$' ? HdrM : Idle);
            break;

        case Size:
            size_ = b;
            crc_ = b;
            idx_ = 0;
            oversize_ = (size_ > kMspMaxPayload);
            state_ = Id;
            break;

        case Id:
            id_ = b;
            crc_ ^= b;
            state_ = (size_ == 0) ? Crc : Payload;
            break;

        case Payload:
            crc_ ^= b;
            if (!oversize_) {
                payload_[idx_] = b;
            }
            idx_++;
            if (idx_ >= size_) {
                state_ = Crc;
            }
            break;

        case Crc: {
            const bool ok = (b == crc_) && !oversize_;
            state_ = Idle;
            return ok;
        }
    }
    return false;
}

}  // namespace ff
