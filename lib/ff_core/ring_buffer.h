#pragma once
//
// Single-producer / single-consumer lock-free ring buffer.
//
// The v2 receive path hands frames from an interrupt/callback context (LoRa DIO
// ISR, or the ESP-NOW receive callback that runs in the WiFi task) to the main
// loop without disabling interrupts or taking a lock. One producer, one consumer,
// fixed capacity, no allocation.
//
// Correctness rests on release/acquire ordering of the head/tail indices, which
// std::atomic provides. size_t atomics are lock-free on both the host and the
// ESP32/ESP8266 targets, so the same code is valid in an ISR and under host tests.
//
// Usable capacity is Capacity - 1 (one slot distinguishes full from empty).
//
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace ff {

template <typename T, size_t Capacity>
class SpscRing {
    static_assert(Capacity >= 2, "SpscRing needs at least 2 slots");

public:
    // Producer side. Returns false and increments the drop counter if full.
    bool push(const T& item) {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t next_head = next(head);
        if (next_head == tail_.load(std::memory_order_acquire)) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        buf_[head] = item;
        head_.store(next_head, std::memory_order_release);
        return true;
    }

    // Consumer side. Returns false if empty.
    bool pop(T& out) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) {
            return false;
        }
        out = buf_[tail];
        tail_.store(next(tail), std::memory_order_release);
        return true;
    }

    bool empty() const {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

    bool full() const {
        return next(head_.load(std::memory_order_acquire)) ==
               tail_.load(std::memory_order_acquire);
    }

    size_t size() const {
        const size_t head = head_.load(std::memory_order_acquire);
        const size_t tail = tail_.load(std::memory_order_acquire);
        return (head + Capacity - tail) % Capacity;
    }

    uint32_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

private:
    static size_t next(size_t i) { return (i + 1) % Capacity; }

    T buf_[Capacity];
    std::atomic<size_t> head_{0};  // next slot to write (producer)
    std::atomic<size_t> tail_{0};  // next slot to read (consumer)
    std::atomic<uint32_t> dropped_{0};
};

}  // namespace ff
