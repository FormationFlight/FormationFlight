#pragma once
//
// ALOHA beacon rate control.
//
// There are no timeslots. Every node transmits position beacons at a randomized
// interval. To keep the shared channel from saturating as the group grows, each
// node scales its beacon interval by the number of nodes it can hear, targeting a
// fixed aggregate channel utilisation (offered load G).
//
// Because every node observes roughly the same peer count and uses the same
// formula, their intervals converge on the same value without any coordination,
// master, or handshake. Uniform jitter on each transmission decorrelates the
// nodes so they don't lock into synchronized collisions.
//
//   base_interval = (N_total * airtime) / G_target      (N_total = peers + self)
//   interval      = clamp(base_interval, [min, max])
//   next_delay    = interval * (1 +/- jitter_frac)
//
// This module is pure: the RNG for jitter is injected as a [0,1) sample so tests
// are deterministic.
//
#include <cstdint>

namespace ff {

// Regional duty-cycle ceiling as a percentage, 0 for none. Set per band in
// platformio.ini; EU 868 is the one that really bites.
#ifndef LORA_DUTY_PCT
#define LORA_DUTY_PCT 0
#endif

struct RateConfig {
    // Target aggregate channel utilisation. Pure-ALOHA throughput peaks near
    // G = 0.5; we run well below that (default 0.15) to keep collision
    // probability low, trading raw throughput for reliability of a redundant
    // beacon stream.
    float target_load = 0.15f;
    uint32_t min_interval_ms = 100;   // fastest beacon (alone / few peers)
    uint32_t max_interval_ms = 1000;  // slowest beacon (crowded channel)
    float jitter_frac = 0.25f;        // +/- fraction applied per transmission

    // Hard ceiling on transmit duty cycle, as a percentage. 0 disables it.
    //
    // This is not a tuning knob, it is a legal limit: EU 868 permits 10% in the
    // sub-band v2 uses, and 1% in most of the others. Enforced here rather than
    // left to whoever fills in min_interval_ms, because the failure mode of
    // getting it wrong is transmitting illegally while everything looks fine.
    // The resulting floor is airtime * 100 / duty: at 72 ms per frame and 10%,
    // that is a beacon every 720 ms no matter what min_interval_ms says.
    uint8_t duty_cycle_pct = LORA_DUTY_PCT;
};

class RateController {
public:
    explicit RateController(const RateConfig& cfg);

    // Airtime of one beacon at the current radio mode, in milliseconds.
    void setAirtimeMs(double airtime_ms);
    double airtimeMs() const { return airtime_ms_; }

    // Deterministic base interval (pre-jitter), clamped to config bounds, using
    // the airtime set via setAirtimeMs(). active_peers excludes self.
    uint32_t baseIntervalMs(uint32_t active_peers) const;

    // The shortest interval the duty-cycle ceiling allows for this airtime, or
    // 0 when no ceiling is configured. Exposed so the status view can explain
    // why a radio is slower than min_interval_ms asked for.
    uint32_t dutyFloorMs(double airtime_ms) const;

    // Base interval with jitter applied. rand01 is a uniform sample in [0,1).
    uint32_t nextDelayMs(uint32_t active_peers, float rand01) const;

    // Airtime-parameterized variants: same math, but with the airtime supplied by
    // the caller. Used to pace each radio independently -- ESP-NOW's small airtime
    // keeps it fast while LoRa's large airtime slows it as peers grow -- without
    // needing a separate controller (config/clamps are shared) per radio.
    uint32_t baseIntervalMs(uint32_t active_peers, double airtime_ms) const;
    uint32_t nextDelayMs(uint32_t active_peers, float rand01, double airtime_ms) const;

    const RateConfig& config() const { return cfg_; }

private:
    RateConfig cfg_;
    double airtime_ms_ = 10.0;  // conservative default until set by radio
};

}  // namespace ff
