#ifdef GNSS_ENABLED

#include "DirectGpsLocationSource.h"

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

    // Emit UBX-NAV-PVT every solution.
    n = buildCfgMsg(kUbxClassNav, kUbxIdNavPvt, 1, buf, sizeof(buf));
    serial_->write(buf, n);

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
    last_pvt_ms_ = millis();
}

void DirectGpsLocationSource::service() {
    if (serial_ == nullptr) {
        return;
    }

    // Drain buffered bytes (capped) -- never wait.
    for (int budget = 256; budget > 0 && serial_->available() > 0; budget--) {
        if (parser_.feed(static_cast<uint8_t>(serial_->read()))) {
            if (parser_.msgClass() == kUbxClassNav && parser_.msgId() == kUbxIdNavPvt) {
                handlePvt();
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
    }
}

}  // namespace ff

#endif  // GNSS_ENABLED
