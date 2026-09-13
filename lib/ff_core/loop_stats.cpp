#include "loop_stats.h"

namespace ff {

void LoopStats::addSample(uint32_t duration_us) {
    last_us_ = duration_us;
    if (samples_ == 0 || duration_us < min_us_) {
        min_us_ = duration_us;
    }
    if (duration_us > max_us_) {
        max_us_ = duration_us;
    }
    samples_++;
    total_us_ += duration_us;
    if (overrun_us_ != 0 && duration_us >= overrun_us_) {
        overruns_++;
    }
}

void LoopStats::reset() {
    last_us_ = 0;
    min_us_ = 0;
    max_us_ = 0;
    samples_ = 0;
    total_us_ = 0;
    // overruns_ deliberately survives; see the header.
}

}  // namespace ff
