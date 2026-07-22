#include "rate_control.h"

namespace ff {

namespace {

uint32_t clampU32(double v, uint32_t lo, uint32_t hi) {
    if (v < static_cast<double>(lo)) {
        return lo;
    }
    if (v > static_cast<double>(hi)) {
        return hi;
    }
    return static_cast<uint32_t>(v + 0.5);  // round to nearest
}

}  // namespace

RateController::RateController(const RateConfig& cfg) : cfg_(cfg) {}

void RateController::setAirtimeMs(double airtime_ms) {
    if (airtime_ms > 0.0) {
        airtime_ms_ = airtime_ms;
    }
}

uint32_t RateController::baseIntervalMs(uint32_t active_peers) const {
    // Include ourselves in the node count sharing the channel.
    const double n_total = static_cast<double>(active_peers) + 1.0;
    const double load = (cfg_.target_load > 0.0f) ? cfg_.target_load : 0.15;
    const double base = (n_total * airtime_ms_) / load;
    return clampU32(base, cfg_.min_interval_ms, cfg_.max_interval_ms);
}

uint32_t RateController::nextDelayMs(uint32_t active_peers, float rand01) const {
    const uint32_t base = baseIntervalMs(active_peers);
    // Map rand01 in [0,1) to a factor in [1 - jitter, 1 + jitter).
    const double factor = 1.0 + (static_cast<double>(rand01) * 2.0 - 1.0) *
                                    static_cast<double>(cfg_.jitter_frac);
    double delay = static_cast<double>(base) * factor;
    if (delay < 0.0) {
        delay = 0.0;
    }
    return static_cast<uint32_t>(delay + 0.5);
}

}  // namespace ff
