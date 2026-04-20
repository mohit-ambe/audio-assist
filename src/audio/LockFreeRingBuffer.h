#pragma once

#include <atomic>
#include <cstddef>
#include <optional>
#include <vector>

namespace audio_assist {

template <typename T>
class LockFreeRingBuffer {
public:
    explicit LockFreeRingBuffer(std::size_t capacity)
        : capacity_(capacity + 1), slots_(capacity_) {}

    bool push(T value) {
        const auto head = head_.load(std::memory_order_relaxed);
        const auto next = increment(head);
        if (next == tail_.load(std::memory_order_acquire)) {
            return false;
        }

        slots_[head] = std::move(value);
        head_.store(next, std::memory_order_release);
        return true;
    }

    std::optional<T> pop() {
        const auto tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) {
            return std::nullopt;
        }

        std::optional<T> out = std::move(slots_[tail]);
        slots_[tail].reset();
        tail_.store(increment(tail), std::memory_order_release);
        return out;
    }

    void clear() {
        while (pop().has_value()) {
        }
    }

private:
    std::size_t increment(std::size_t index) const {
        return (index + 1) % capacity_;
    }

    const std::size_t capacity_;
    std::vector<std::optional<T>> slots_;
    std::atomic<std::size_t> head_{0};
    std::atomic<std::size_t> tail_{0};
};

}  // namespace audio_assist
