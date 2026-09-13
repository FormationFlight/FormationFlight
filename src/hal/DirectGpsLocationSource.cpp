#ifdef GNSS_ENABLED

#include "DirectGpsLocationSource.h"

#include <cstring>

#include "log.h"

namespace ff {

namespace {
// Baud rates to try. Target first (common if already configured), then the u-blox
// factory default (9600), then other likely speeds.
const uint32_t kCandidateBauds[] = {115200, 9600, 38400, 57600, 230400};
constexpr uint8_t kNumBauds = sizeof(kCandidateBauds) / sizeof(kCandidateBauds[0]);
constexpr uint32_t kSweepStepMs = 200;
constexpr uint32_t kSilenceTimeoutMs = 4000;
}  // namespace

void DirectGpsLocationSource::begin(uint8_t uart_index, int8_t pin_rx, int8_t pin_tx,
                                    uint8_t rate_hz) {
    serial_ = new HardwareSerial(uart_index);
    pin_rx_ = pin_rx;
    pin_tx_ = pin_tx;

    if (rate_hz < 1) rate_hz = 1;
    if (rate_hz > 18) rate_hz = 18;  // u-blox practical ceiling
    meas_ms_ = static_cast<uint16_t>(1000 / rate_hz);

    state_ = Sweep;
    sweep_idx_ = 0;
    // Fire the first sweep step on the next service() call.
    sweep_step_ms_ = millis() - kSweepStepMs;
    setBaud(target_baud_);
}

void DirectGpsLocationSource::setBaud(uint32_t baud) {
    serial_->begin(baud, SERIAL_8N1, pin_rx_, pin_tx_);
    current_baud_ = baud;
    parser_.reset();
}

void DirectGpsLocationSource::sendConfig() {
    uint8_t buf[32];
    size_t n;

    // Navigation rate (the "highest reasonable rate" the caller asked for).
    n = buildCfgRate(meas_ms_, buf, sizeof(buf));
    serial_->write(buf, n);

    if (legacy_) {
        // Already established that this module predates NAV-PVT. Go straight to
        // the older set so a re-sweep does not spend four seconds relearning it.
        uint8_t legacy[64];
        n = buildLegacyNavConfig(legacy, sizeof(legacy));
        if (n != 0) {
            serial_->write(legacy, n);
        }
    } else {
        // Emit UBX-NAV-PVT every solution, and watch for the answer: a module
        // old enough to lack it will NAK this, and that NAK is the only thing
        // that distinguishes it from a module that is simply not wired up.
        n = buildCfgMsg(kUbxClassNav, kUbxIdNavPvt, 1, buf, sizeof(buf));
        serial_->write(buf, n);
        awaiting_pvt_ack_ = true;
    }

    // Silence the standard NMEA sentences (GGA,GLL,GSA,GSV,RMC,VTG) so the fast
    // stream stays compact.
    for (uint8_t id = 0x00; id <= 0x05; id++) {
        n = buildCfgMsg(kUbxClassNmea, id, 0, buf, sizeof(buf));
        serial_->write(buf, n);
    }
}

void DirectGpsLocationSource::handlePvt() {
    UbxFix fix{};
    if (!decodeNavPvt(parser_.payload(), parser_.length(), fix)) {
        return;
    }
    cached_.valid = fix.valid;
    cached_.lat = fix.lat;
    cached_.lon = fix.lon;
    cached_.alt_m = fix.alt_m;
    cached_.speed_cms = fix.speed_cms;
    cached_.course_ddeg = fix.course_ddeg;
    cached_.sats = fix.num_sat;
    cached_.fix_type = fix.fix_type;
    last_pvt_ms_ = millis();
}

void DirectGpsLocationSource::service() {
    if (serial_ == nullptr) {
        return;
    }

    // Drain buffered bytes (capped) -- never wait.
    for (int budget = 256; budget > 0 && serial_->available() > 0; budget--) {
        const uint8_t b = static_cast<uint8_t>(serial_->read());
        noteByte(b);
        if (parser_.feed(b)) {
            ubx_frames_++;
            noteMessage(parser_.msgClass(), parser_.msgId());
            if (parser_.msgClass() == kUbxClassNav) {
                if (parser_.msgId() == kUbxIdNavPvt) {
                    nav_pvt_++;
                    handlePvt();
                } else {
                    handleLegacyNav(parser_.msgId());
                }
            } else if (parser_.msgClass() == kUbxClassAck) {
                handleAck(parser_.msgId() == kUbxIdAckAck);
            }
        }
    }

    const uint32_t now = millis();

    if (state_ == Sweep) {
        if (now - sweep_step_ms_ < kSweepStepMs) {
            return;
        }
        sweep_step_ms_ = now;
        if (sweep_idx_ < kNumBauds) {
            // Try to switch the module (whatever baud it is at) to the target.
            setBaud(kCandidateBauds[sweep_idx_]);
            uint8_t buf[32];
            size_t n = buildCfgPrtUart(target_baud_, buf, sizeof(buf));
            serial_->write(buf, n);
            sweep_idx_++;
        } else {
            // Settle at the target baud and configure rate + messages.
            setBaud(target_baud_);
            sendConfig();
            last_pvt_ms_ = now;
            state_ = Parse;
        }
        return;
    }

    // Parse state: if the module has gone quiet, re-run detection.
    if (now - last_pvt_ms_ > kSilenceTimeoutMs) {
        cached_.valid = false;
        state_ = Sweep;
        sweep_idx_ = 0;
        sweep_step_ms_ = now - kSweepStepMs;
        sweeps_++;
        // Only the first one. This loops every four seconds when a module will
        // not speak to us, and 48 log entries would be gone in three minutes.
        if (sweeps_ == 1) {
            FF_LOGW("GNSS: no NAV-PVT in %us, restarting baud sweep (%u bytes seen)",
                    static_cast<unsigned>(kSilenceTimeoutMs / 1000),
                    static_cast<unsigned>(bytes_));
        }
    }
}

// The older navigation messages, each carrying one part of what NAV-PVT says in
// a single frame. They are merged into the same cached location, so whichever
// protocol generation the module speaks, everything above this driver sees the
// same thing.
void DirectGpsLocationSource::handleLegacyNav(uint8_t id) {
    switch (id) {
        case kUbxIdNavPosllh: {
            UbxPosLlh p{};
            if (!decodeNavPosllh(parser_.payload(), parser_.length(), p)) return;
            cached_.lat = p.lat;
            cached_.lon = p.lon;
            cached_.alt_m = p.alt_m;
            break;
        }
        case kUbxIdNavSol: {
            UbxSol sol{};
            if (!decodeNavSol(parser_.payload(), parser_.length(), sol)) return;
            cached_.valid = sol.valid;
            cached_.fix_type = sol.fix_type;
            cached_.sats = sol.num_sat;
            break;
        }
        case kUbxIdNavVelned: {
            UbxVelNed v{};
            if (!decodeNavVelned(parser_.payload(), parser_.length(), v)) return;
            cached_.speed_cms = v.speed_cms;
            cached_.course_ddeg = v.course_ddeg;
            break;
        }
        case kUbxIdNavDop: {
            uint16_t hdop = 0;
            if (!decodeNavDop(parser_.payload(), parser_.length(), hdop)) return;
            cached_.hdop = hdop;
            break;
        }
        default:
            return;
    }
    // Any of the four counts as the module talking to us, so the silence
    // watchdog does not restart a sweep that has already succeeded.
    last_pvt_ms_ = millis();
}

// A NAK for the message we just asked for is the module saying it has never
// heard of it, which is exactly what a pre-protocol-14 receiver does when asked
// for NAV-PVT. Without acting on it the driver waits forever for a message that
// is never going to arrive, while the receiver sits there holding a fix.
void DirectGpsLocationSource::handleAck(bool acked) {
    if (!awaiting_pvt_ack_ || legacy_) {
        return;
    }
    uint8_t cls = 0, id = 0;
    if (!decodeAck(parser_.payload(), parser_.length(), cls, id)) {
        return;
    }
    if (cls != kUbxClassCfg || id != kUbxIdCfgMsg) {
        return;  // some other request; not the answer we are waiting on
    }
    awaiting_pvt_ack_ = false;
    if (acked) {
        return;  // modern module, NAV-PVT is on its way
    }

    legacy_ = true;
    FF_LOGW("GNSS: module has no NAV-PVT, falling back to the legacy nav set");
    uint8_t buf[64];
    const size_t n = buildLegacyNavConfig(buf, sizeof(buf));
    if (n != 0) {
        serial_->write(buf, n);
    }
    // Give the module the full silence window to answer the new request rather
    // than tearing the sweep down while it is still being configured.
    last_pvt_ms_ = millis();
}

void DirectGpsLocationSource::noteByte(uint8_t b) {
    bytes_++;
    last_byte_ms_ = millis();
    // An NMEA sentence starts "$G..." for every GNSS talker, or "$P" for a
    // proprietary one. Testing the second byte as well matters: a bare '$' is
    // 0x24, which turns up constantly inside binary UBX payloads, and counting
    // those made a pure-UBX stream look like it was full of NMEA.
    if (prev_byte_ == '$' && (b == 'G' || b == 'P')) {
        nmea_++;
    }
    prev_byte_ = b;
    sniff_[sniff_head_] = b;
    sniff_head_ = (sniff_head_ + 1) % kSniffBytes;
    if (sniff_count_ < kSniffBytes) {
        sniff_count_++;
    }
}

void DirectGpsLocationSource::noteMessage(uint8_t cls, uint8_t id) {
    for (size_t i = 0; i < kGnssSeenTypes; i++) {
        if (seen_[i].count != 0 && seen_[i].cls == cls && seen_[i].id == id) {
            seen_[i].count++;
            return;
        }
        if (seen_[i].count == 0) {
            seen_[i].cls = cls;
            seen_[i].id = id;
            seen_[i].count = 1;
            return;
        }
    }
    // Table full. Nine distinct message types means the module is sending its
    // defaults rather than what we asked for, which the counts already show.
}

GnssLinkStats DirectGpsLocationSource::gnssStats(uint32_t now_ms) const {
    GnssLinkStats s;
    s.bytes = bytes_;
    s.ubx_frames = ubx_frames_;
    s.nav_pvt = nav_pvt_;
    s.nmea = nmea_;
    s.sweeps = sweeps_;
    s.baud = current_baud_;
    s.configured = (state_ == Parse);
    s.last_byte_age_ms = (last_byte_ms_ == 0) ? 0 : (now_ms - last_byte_ms_);
    s.last_pvt_age_ms = (nav_pvt_ == 0) ? 0 : (now_ms - last_pvt_ms_);
    for (size_t i = 0; i < kGnssSeenTypes; i++) {
        s.seen[i] = seen_[i];
    }
    return s;
}

size_t DirectGpsLocationSource::gnssSniff(uint8_t* out, size_t cap) const {
    const size_t n = (sniff_count_ < cap) ? sniff_count_ : cap;
    // Oldest first, so a hex dump reads in the order the bytes arrived.
    const size_t start = (sniff_head_ + kSniffBytes - n) % kSniffBytes;
    for (size_t i = 0; i < n; i++) {
        out[i] = sniff_[(start + i) % kSniffBytes];
    }
    return n;
}

}  // namespace ff

#endif  // GNSS_ENABLED
