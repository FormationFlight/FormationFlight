#include "loop_stats.h"

namespace ff {

void LoopStats::addSample(uint32_t duration_us, uint32_t excluded_us) {
    // Saturating, not wrapping. The two clock reads that produce these bracket
    // a preemption point, so a subtraction that went negative would otherwise
    // turn into a four-billion-microsecond iteration and pin the maximum for
    // the rest of the node's life.
    if (excluded_us != 0) {
        excluded_us_ += excluded_us;
        duration_us = (excluded_us < duration_us) ? (duration_us - excluded_us) : 0;
    }
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
    // overruns_ and excluded_us_ deliberately survive; see the header.
}

}  // namespace ff
