#include "scheduler.h"

namespace ff {

TimerHandle Scheduler::alloc() {
    for (size_t i = 0; i < kMaxTimers; i++) {
        if (!timers_[i].active) {
            return static_cast<TimerHandle>(i);
        }
    }
    return kInvalidTimer;
}

TimerHandle Scheduler::after(uint32_t delay_ms, TaskFn fn, void* ctx) {
    TimerHandle h = alloc();
    if (h == kInvalidTimer) {
        return kInvalidTimer;
    }
    Timer& t = timers_[h];
    t.due_ms = now_ms_ + delay_ms;
    t.period_ms = 0;
    t.fn = fn;
    t.ctx = ctx;
    t.active = true;
    return h;
}

TimerHandle Scheduler::every(uint32_t period_ms, TaskFn fn, void* ctx) {
    TimerHandle h = alloc();
    if (h == kInvalidTimer) {
        return kInvalidTimer;
    }
    Timer& t = timers_[h];
    t.due_ms = now_ms_ + period_ms;
    t.period_ms = period_ms;
    t.fn = fn;
    t.ctx = ctx;
    t.active = true;
    return h;
}

void Scheduler::cancel(TimerHandle h) {
    if (h < 0 || static_cast<size_t>(h) >= kMaxTimers) {
        return;
    }
    timers_[h].active = false;
}

void Scheduler::rearm(TimerHandle h, uint32_t delay_ms) {
    if (h < 0 || static_cast<size_t>(h) >= kMaxTimers) {
        return;
    }
    Timer& t = timers_[h];
    t.due_ms = now_ms_ + delay_ms;
    t.period_ms = 0;
    t.active = true;
}

bool Scheduler::isActive(TimerHandle h) const {
    if (h < 0 || static_cast<size_t>(h) >= kMaxTimers) {
        return false;
    }
    return timers_[h].active;
}

uint32_t Scheduler::poll(uint32_t now_ms) {
    now_ms_ = now_ms;

    // Snapshot which timers are due at entry. Callbacks may schedule new timers
    // (in the future) or cancel others; snapshotting keeps a single poll() from
    // firing a timer a callback just created, and re-checking active respects
    // cancellations made by earlier callbacks in the same poll.
    bool due[kMaxTimers];
    for (size_t i = 0; i < kMaxTimers; i++) {
        due[i] = timers_[i].active && reached(now_ms_, timers_[i].due_ms);
    }

    for (size_t i = 0; i < kMaxTimers; i++) {
        if (!due[i] || !timers_[i].active) {
            continue;
        }
        Timer& t = timers_[i];
        TaskFn fn = t.fn;
        void* ctx = t.ctx;

        if (t.period_ms == 0) {
            t.active = false;  // one-shot
        } else {
            // Advance before firing so a callback can cancel/rearm cleanly.
            t.due_ms += t.period_ms;
            if (reached(now_ms_, t.due_ms)) {
                // We fell more than a full period behind; resync rather than
                // firing in a catch-up burst.
                t.due_ms = now_ms_ + t.period_ms;
            }
        }
        fn(ctx);
    }

    uint32_t next_delay = UINT32_MAX;
    for (size_t i = 0; i < kMaxTimers; i++) {
        if (!timers_[i].active) {
            continue;
        }
        int32_t d = static_cast<int32_t>(timers_[i].due_ms - now_ms_);
        uint32_t dd = d < 0 ? 0u : static_cast<uint32_t>(d);
        if (dd < next_delay) {
            next_delay = dd;
        }
    }
    return next_delay;
}

size_t Scheduler::activeCount() const {
    size_t n = 0;
    for (size_t i = 0; i < kMaxTimers; i++) {
        if (timers_[i].active) {
            n++;
        }
    }
    return n;
}

}  // namespace ff
