#include "msp_parser.h"

#include "msp_crc.h"

namespace ff {

void MspParser::reset() {
    state_ = Idle;
    version_ = 0;
    size_ = 0;
    id_ = 0;
    flag_ = 0;
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
            if (b == 'M') {
                state_ = V1Dir;
            } else if (b == 'X') {
                state_ = V2Dir;
            } else {
                state_ = (b == '$') ? HdrM : Idle;
            }
            break;

        // ---- v1 ----------------------------------------------------------
        case V1Dir:
            // Responses are '>'. Anything else (e.g. '!' error, or noise) resyncs.
            state_ = (b == '>') ? V1Size : (b == '$' ? HdrM : Idle);
            break;

        case V1Size:
            version_ = 1;
            flag_ = 0;
            size_ = b;
            crc_ = b;
            idx_ = 0;
            oversize_ = (size_ > kMspMaxPayload);
            state_ = V1Id;
            break;

        case V1Id:
            id_ = b;
            crc_ ^= b;
            state_ = (size_ == 0) ? V1Crc : V1Payload;
            break;

        case V1Payload:
            crc_ ^= b;
            if (!oversize_) {
                payload_[idx_] = b;
            }
            idx_++;
            if (idx_ >= size_) {
                state_ = V1Crc;
            }
            break;

        case V1Crc: {
            const bool ok = (b == crc_) && !oversize_;
            state_ = Idle;
            return ok;
        }

        // ---- v2 ----------------------------------------------------------
        case V2Dir:
            state_ = (b == '>') ? V2Flag : (b == '$' ? HdrM : Idle);
            break;

        case V2Flag:
            version_ = 2;
            flag_ = b;
            crc_ = mspCrc8DvbS2(0, b);
            state_ = V2IdLo;
            break;

        case V2IdLo:
            id_ = b;
            crc_ = mspCrc8DvbS2(crc_, b);
            state_ = V2IdHi;
            break;

        case V2IdHi:
            id_ |= static_cast<uint16_t>(static_cast<uint16_t>(b) << 8);
            crc_ = mspCrc8DvbS2(crc_, b);
            state_ = V2SizeLo;
            break;

        case V2SizeLo:
            size_ = b;
            crc_ = mspCrc8DvbS2(crc_, b);
            state_ = V2SizeHi;
            break;

        case V2SizeHi:
            size_ |= static_cast<uint16_t>(static_cast<uint16_t>(b) << 8);
            crc_ = mspCrc8DvbS2(crc_, b);
            idx_ = 0;
            oversize_ = (size_ > kMspMaxPayload);
            state_ = (size_ == 0) ? V2Crc : V2Payload;
            break;

        case V2Payload:
            crc_ = mspCrc8DvbS2(crc_, b);
            if (!oversize_) {
                payload_[idx_] = b;
            }
            idx_++;
            if (idx_ >= size_) {
                state_ = V2Crc;
            }
            break;

        case V2Crc: {
            const bool ok = (b == crc_) && !oversize_;
            state_ = Idle;
            return ok;
        }
    }
    return false;
}

}  // namespace ff
