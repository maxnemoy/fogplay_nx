#pragma once
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace stream {
// One bounded reader for one transport lifetime. The callable owns transport
// handles only; neither its execution nor its destruction holds the mailbox.
// A non-cancellable RTC call is joined only at the owner's explicit stop boundary.
template<class Value> class AsyncIceSnapshot {
public:
    using Clock = std::chrono::steady_clock;
    struct Timing {
        std::uint64_t calls = 0, totalUs = 0, maxUs = 0;
        void add(Clock::duration elapsed) {
            const auto us = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
            ++calls; totalUs += us; maxUs = std::max(maxUs, us);
        }
    };
    struct Result {
        Value value;
        std::uint64_t revision = 0, serial = 0;
        Clock::time_point capturedAt{};
    };
    struct State {
        std::uint64_t revision = 1, requested = 0, completed = 0, discarded = 0;
        bool active = false, pending = false, stopping = false;
        std::uint64_t activeMs = 0;
        Timing wait, schedule, work, diagnostic, pair;
    };
    AsyncIceSnapshot() : worker_([this] { run(); }) {}
    ~AsyncIceSnapshot() { stop(); }
    AsyncIceSnapshot(const AsyncIceSnapshot&) = delete;
    AsyncIceSnapshot& operator=(const AsyncIceSnapshot&) = delete;
    void invalidate() {
        std::lock_guard<std::mutex> lock(mutex_);
        ++state_.revision;
        result_.reset();
        // Pending readers are small captures, retained until overwritten/stop.
        // Their handles must not be destroyed under this lock.
    }
    bool request(std::uint64_t revision, std::function<Value()> reader) {
        std::function<Value()> retired;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_.stopping || revision != state_.revision || state_.pending) return false;
            retired.swap(reader_); reader_ = std::move(reader);
            requestedRevision_ = revision; requestedAt_ = Clock::now();
            requestedSerial_ = ++state_.requested; state_.pending = true;
        }
        condition_.notify_one(); return true;
    }
    std::optional<Result> take() {
        std::lock_guard<std::mutex> lock(mutex_);
        auto out = std::move(result_); result_.reset(); return out;
    }
    State state() const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto out = state_;
        if (out.active) out.activeMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - activeAt_).count();
        return out;
    }
    void requestStop() {
        std::function<Value()> retired;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!state_.stopping) ++state_.revision;
            state_.stopping = true; state_.pending = false; result_.reset(); retired.swap(reader_);
        }
        condition_.notify_all();
    }
    void stop() { requestStop(); if (worker_.joinable()) worker_.join(); }
private:
    void run() noexcept {
        try {
            std::unique_lock<std::mutex> lock(mutex_);
            for (;;) {
                const auto waited = Clock::now();
                condition_.wait(lock, [this] { return state_.stopping || state_.pending; });
                state_.wait.add(Clock::now() - waited);
                if (state_.stopping) return;
                auto reader = std::move(reader_);
                const auto revision = requestedRevision_, serial = requestedSerial_;
                state_.pending = false;
                if (revision != state_.revision) {
                    ++state_.discarded; lock.unlock(); reader = {}; lock.lock(); continue;
                }
                activeAt_ = Clock::now(); state_.schedule.add(activeAt_ - requestedAt_); state_.active = true;
                lock.unlock();
                std::optional<Value> value;
                try { value = reader(); } catch (...) {}
                const auto completedAt = Clock::now();
                reader = {}; // Transport release outside the mailbox lock.
                lock.lock();
                state_.work.add(completedAt - activeAt_); state_.active = false; ++state_.completed;
                if (value) {
                    state_.diagnostic.calls += value->diagnosticTiming.calls;
                    state_.diagnostic.totalUs += value->diagnosticTiming.totalUs;
                    state_.diagnostic.maxUs = std::max(state_.diagnostic.maxUs, value->diagnosticTiming.maxUs);
                    state_.pair.calls += value->pairTiming.calls;
                    state_.pair.totalUs += value->pairTiming.totalUs;
                    state_.pair.maxUs = std::max(state_.pair.maxUs, value->pairTiming.maxUs);
                }
                if (value && !state_.stopping && revision == state_.revision)
                    result_ = Result{std::move(*value), revision, serial, completedAt};
                else ++state_.discarded;
            }
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex_);
            state_.active = false; state_.stopping = true;
        }
    }
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    State state_;
    std::function<Value()> reader_;
    std::optional<Result> result_;
    Clock::time_point requestedAt_{}, activeAt_{};
    std::uint64_t requestedRevision_ = 0, requestedSerial_ = 0;
    std::thread worker_;
};
}
