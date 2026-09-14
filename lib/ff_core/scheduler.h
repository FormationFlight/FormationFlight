#pragma once
//
// Cooperative timer scheduler.
//
// This replaces the v1 "megaloop", where every periodic action was an inline
// `if (millis() > next_x)` check smeared through one 250-line loop(). Modules now
// register timed callbacks and the scheduler dispatches them. It stays single-
// threaded and non-preemptive -- which is fine, because the v2 design removed the
// only thing that needed hard real-time precision (TDMA slots): ALOHA tolerates
// loop jitter by construction, and blocking work (MSP serial) becomes a byte-pump
// state machine driven from here rather than a busy-wait.
//
// Fixed capacity, no dynamic allocation. Time is injected via poll(now_ms) so the
// logic is fully deterministic under host tests. Deadline comparisons use signed
// differences, so they remain correct across the ~49-day millis() wraparound.
//
#include <cstddef>
#include <cstdint>

namespace ff {

using TaskFn = void (*)(void* ctx);

constexpr size_t kMaxTimers = 16;

using TimerHandle = int;
constexpr TimerHandle kInvalidTimer = -1;

class Scheduler {
public:
    // One-shot: fire once, delay_ms from the current time.
    TimerHandle after(uint32_t delay_ms, TaskFn fn, void* ctx);

    // Periodic: fire every period_ms, first firing at now + period_ms.
    TimerHandle every(uint32_t period_ms, TaskFn fn, void* ctx);

    // Stop a timer (safe to call on an already-inactive/one-shot handle, and
    // safe to call from within a callback).
    void cancel(TimerHandle h);

    // Re-arm a handle as a one-shot delay_ms from now. Used to give the ALOHA
    // beacon fresh jitter each transmission by re-arming from its own callback.
    void rearm(TimerHandle h, uint32_t delay_ms);

    bool isActive(TimerHandle h) const;

    // Advance the clock to now_ms and fire every timer that is due. Returns the
    // number of milliseconds until the next timer is due, or UINT32_MAX if none
    // are active (a caller may use this to decide how long to sleep).
    uint32_t poll(uint32_t now_ms);

    uint32_t now() const { return now_ms_; }
    size_t activeCount() const;

private:
    struct Timer {
        uint32_t due_ms = 0;
        uint32_t period_ms = 0;  // 0 => one-shot
        TaskFn fn = nullptr;
        void* ctx = nullptr;
        bool active = false;
    };

    TimerHandle alloc();
    static bool reached(uint32_t now_ms, uint32_t due_ms) {
        return static_cast<int32_t>(now_ms - due_ms) >= 0;
    }

    Timer timers_[kMaxTimers];
    uint32_t now_ms_ = 0;
};

}  // namespace ff
