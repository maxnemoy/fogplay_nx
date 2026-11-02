#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace stream {
// Observe before polling the queue, then wait only while that revision is
// unchanged. Publish the queue/command before notifying. The mutex closes the
// predicate-to-sleep race; consumers never hold a queue lock while waiting.
class MediaSignal {
public:
    std::uint64_t sequence() const { return sequence_.load(std::memory_order_acquire); }
    void notify() {
        { std::lock_guard<std::mutex> lock(mutex_); sequence_.fetch_add(1, std::memory_order_release); }
        wake_.notify_all();
    }
    void wait(std::uint64_t observed, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        wake_.wait_for(lock, timeout, [&] { return sequence() != observed; });
    }
private:
    std::atomic<std::uint64_t> sequence_{0};
    std::mutex mutex_;
    std::condition_variable wake_;
};
struct MediaSignals {
    MediaSignal video, audio;
    void notifyAll() { video.notify(); audio.notify(); }
};
}
