#pragma once
#include "features/streaming/domain/streaming_client.hpp"
#include <array>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace stream {
// One assembly producer and one decoder consumer. Only the consumer advances
// tail, including when dropping invalidated generations. Lifecycle destruction
// happens after both owners join. Atomic metadata supports observational UI
// snapshots without touching slots being moved by the consumer.
class SpscVideoQueue {
public:
    static constexpr std::size_t Capacity = 12, MaxBytes = 16 * 1024 * 1024;
    bool push(Packet packet) {
        const auto head = head_.load(std::memory_order_relaxed);
        if (head - tail_.load(std::memory_order_acquire) >= Capacity ||
            packet.data.size() > MaxBytes - bytes_.load(std::memory_order_acquire)) return false;
        auto& slot = slots_[head % Capacity];
        slot.enqueueMs.store(packet.enqueueMs, std::memory_order_relaxed);
        slot.generation.store(packet.videoGeneration, std::memory_order_relaxed);
        bytes_.fetch_add(packet.data.size(), std::memory_order_relaxed);
        slot.packet = std::move(packet);
        head_.store(head + 1, std::memory_order_release);
        return true;
    }
    bool pop(Packet& packet) {
        const auto tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) return false;
        packet = std::move(slots_[tail % Capacity].packet);
        bytes_.fetch_sub(packet.data.size(), std::memory_order_relaxed);
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }
    // Consumer-only: look at the next packet's generation before deciding
    // whether an otherwise closed admission gate may purge the stale slot.
    bool nextGeneration(std::uint64_t& generation) const {
        const auto tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) return false;
        generation = slots_[tail % Capacity].packet.videoGeneration;
        return true;
    }
    std::size_t size() const {
        const auto tail = tail_.load(std::memory_order_acquire);
        const auto head = head_.load(std::memory_order_acquire);
        return head >= tail ? std::min<std::size_t>(head - tail, Capacity) : 0;
    }
    std::size_t bytes() const { return bytes_.load(std::memory_order_relaxed); }
    bool oldestGeneration(std::uint64_t& generation) const {
        const auto tail = tail_.load(std::memory_order_acquire);
        if (tail == head_.load(std::memory_order_acquire)) return false;
        generation = slots_[tail % Capacity].generation.load(std::memory_order_relaxed);
        return tail == tail_.load(std::memory_order_acquire);
    }
    std::uint64_t age(std::uint64_t now) const {
        const auto tail = tail_.load(std::memory_order_acquire);
        if (tail == head_.load(std::memory_order_acquire)) return 0;
        const auto stamp = slots_[tail % Capacity].enqueueMs.load(std::memory_order_relaxed);
        // A concurrent pop may have freed/reused the slot. Such a sample is
        // unavailable rather than evidence for expiring a different frame.
        if (tail != tail_.load(std::memory_order_acquire)) return 0;
        return now >= stamp ? now - stamp : 0;
    }
private:
    struct Slot { Packet packet; std::atomic<std::uint64_t> enqueueMs{0}, generation{0}; };
    std::array<Slot, Capacity> slots_;
    alignas(64) std::atomic<std::uint64_t> head_{0};
    alignas(64) std::atomic<std::uint64_t> tail_{0};
    std::atomic<std::size_t> bytes_{0};
};
}
