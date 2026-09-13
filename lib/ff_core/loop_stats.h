#pragma once
//
// Main-loop timing.
//
// ALOHA tolerates jitter by design, which is exactly why this is worth
// measuring: the firmware will keep working as it gets slower, and go on
// working right up until it does not. A node whose loop occasionally takes
// 200 ms is one missed interrupt away from dropping frames, and nothing else in
// the status view would say so.
//
// The number that matters is the worst case, not the mean. A 2 ms average with
// a 300 ms peak is a node with a problem; the mean alone looks healthy.
//
#include <cstdint>

namespace ff {

class LoopStats {
public:
    // Duration of one loop iteration. Call once per loop.
    void addSample(uint32_t duration_us);

    // A sample above this counts as an overrun. Set from the shortest beacon
    // interval: a loop longer than that can miss a transmission outright.
    void setOverrunThresholdUs(uint32_t us) { overrun_us_ = us; }
    uint32_t overrunThresholdUs() const { return overrun_us_; }

    uint32_t lastUs() const { return last_us_; }
    uint32_t minUs() const { return samples_ ? min_us_ : 0; }
    uint32_t maxUs() const { return max_us_; }
    uint32_t meanUs() const { return samples_ ? static_cast<uint32_t>(total_us_ / samples_) : 0; }
    uint32_t samples() const { return samples_; }
    uint32_t overruns() const { return overruns_; }
    // Loops per second, derived from the mean. 0 before the first sample.
    uint32_t rateHz() const {
        const uint32_t mean = meanUs();
        return mean ? (1000000u / mean) : 0;
    }

    // Clears min/max/mean and the sample count. The overrun counter survives,
    // because "it has stalled at some point since boot" does not stop being
    // true when someone presses a button in a web page.
    void reset();

private:
    uint32_t last_us_ = 0;
    uint32_t min_us_ = 0;
    uint32_t max_us_ = 0;
    uint32_t samples_ = 0;
    uint64_t total_us_ = 0;
    uint32_t overruns_ = 0;
    uint32_t overrun_us_ = 100000;  // 100 ms, the fastest beacon interval
};

}  // namespace ff
