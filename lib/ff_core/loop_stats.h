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
// One subtlety decides whether any of this means anything: what the clock is
// actually measuring. An iteration is timed wall-clock, top to bottom, so on a
// platform with a preemptive scheduler any higher-priority task that interrupts
// the loop is charged to the loop. Measured on real hardware, an ESP32 serving
// one status request booked the entire request against whichever iteration it
// landed in, while an ESP8266 running the identical firmware and the identical
// request booked nothing at all, because its network callbacks run between
// iterations rather than through them. Same work, same cost, and two numbers
// that could not be compared.
//
// So the caller passes the part of an iteration it can attribute elsewhere, and
// it is deducted rather than discarded: the total is still reported, against
// the thing that spent it. Attribution is not complete and cannot be. Time
// inside our own request handlers is measurable; time the network stack spends
// on its own behalf is not, so some of it still lands on the loop. Measured on
// a T-Beam, deducting handler time took a 322 ms iteration down by about half.
// That is the reason the overrun threshold is set where it is rather than at
// something that assumes a perfectly clean measurement.
//
#include <cstdint>

namespace ff {

class LoopStats {
public:
    // Duration of one loop iteration, and however much of that belonged to
    // something other than the loop -- time a higher-priority task held the CPU
    // while this iteration was open. Call once per loop. `excluded_us` is
    // subtracted with saturation, so a clock that ran backwards across the two
    // reads records a zero rather than an enormous number.
    void addSample(uint32_t duration_us, uint32_t excluded_us = 0);

    // A sample at or above this counts as an overrun.
    //
    // Not the shortest beacon interval, which is what this used to be. ALOHA is
    // built to lose transmissions: the beacon stream is redundant and peers do
    // not give up on a node for six seconds. A stall that costs one beacon is
    // therefore not a fault, and counting it as one produced a number that sat
    // permanently red on hardware with nothing wrong with it. What is worth
    // flagging is a stall long enough to be a real fraction of the way to
    // having every peer drop this node.
    void setOverrunThresholdUs(uint32_t us) { overrun_us_ = us; }
    uint32_t overrunThresholdUs() const { return overrun_us_; }

    uint32_t lastUs() const { return last_us_; }
    uint32_t minUs() const { return samples_ ? min_us_ : 0; }
    uint32_t maxUs() const { return max_us_; }
    uint32_t meanUs() const { return samples_ ? static_cast<uint32_t>(total_us_ / samples_) : 0; }
    uint32_t samples() const { return samples_; }
    uint32_t overruns() const { return overruns_; }
    // Total time deducted, so the cost is attributable rather than lost.
    uint64_t excludedUs() const { return excluded_us_; }
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
    uint64_t excluded_us_ = 0;
    // 1.5 s. A quarter of the default peer timeout, so an overrun means the
    // node went quiet for long enough to be a quarter of the way to every peer
    // dropping it. main.cpp derives the real value from the configured timeout.
    uint32_t overrun_us_ = 1500000;
};

}  // namespace ff
