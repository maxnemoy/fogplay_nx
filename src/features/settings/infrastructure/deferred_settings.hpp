#pragma once
#include "core/constants/stage1_options.hpp"
#include "core/diagnostics/thread_observation.hpp"
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

namespace fog::settings {
enum class Key : std::size_t { Video, Connection, Controller, Rumble, Count };

// Optional preferences only. Rental journals and authentication retain their
// synchronous durability contracts. The owner submits immutable value copies;
// disk operations run serially and completions run only in owner-thread poll().
// submit/poll/flush/finish are owner-thread operations. The baseline A/B profile
// performs save and completion immediately on that same owner instead.
class DeferredSettings {
public:
    using Clock = std::chrono::steady_clock;
    using Save = std::function<void()>;
    using Complete = std::function<void(bool)>;
    explicit DeferredSettings(std::chrono::milliseconds delay = std::chrono::milliseconds(500),
        bool asynchronous = fog::stage1::asyncSettings)
        : delay_(delay), asynchronous_(asynchronous) {}
    ~DeferredSettings() noexcept { finish(); }
    DeferredSettings(const DeferredSettings&) = delete;
    DeferredSettings& operator=(const DeferredSettings&) = delete;

    void submit(Key key, Save save, Complete complete) {
        if (!asynchronous_) {
            if (closing_) throw std::logic_error("Settings writer already closed");
            bool saved = true;
            try { save(); } catch (...) { saved = false; }
            try { complete(saved); } catch (...) {}
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (closing_) throw std::logic_error("Settings writer already closed");
        if (!worker_.joinable()) worker_ = std::thread([this] { run(); });
        auto& slot = slots_.at(static_cast<std::size_t>(key));
        const auto revision = ++slot.revision;
        slot.result.reset();
        slot.pending = Job{revision, Clock::now() + delay_, std::move(save), std::move(complete)};
        changed_.notify_one();
    }

    void poll() {
        std::array<std::optional<Result>, static_cast<std::size_t>(Key::Count)> results;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (std::size_t i = 0; i < slots_.size(); ++i) {
                results[i] = std::move(slots_[i].result);
                slots_[i].result.reset();
            }
        }
        for (auto& result : results) if (result) {
            // Status text/optional diagnostics must not interrupt rental
            // cleanup or terminate the process during destructor draining.
            try { result->complete(result->saved); } catch (...) {}
        }
    }

    // Explicit durability barrier for tests and non-streaming callers. Normal
    // UI updates use poll(); they never wait for storage.
    void flush() {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            for (auto& slot : slots_) if (slot.pending) slot.pending->due = Clock::now();
            changed_.notify_one();
            changed_.wait(lock, [this] {
                if (working_) return false;
                for (const auto& slot : slots_) if (slot.pending) return false;
                return true;
            });
        }
        poll();
    }

    // Owner-thread shutdown only. Bypass debounce, drain every latest value,
    // join before destroying captured state, and report final write failures.
    void finish() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closing_ = true;
            changed_.notify_one();
        }
        if (worker_.joinable()) worker_.join();
        poll();
    }

private:
    struct Job {
        std::uint64_t revision;
        Clock::time_point due;
        Save save;
        Complete complete;
    };
    struct Result { bool saved; Complete complete; };
    struct Slot {
        std::uint64_t revision = 0;
        std::optional<Job> pending;
        std::optional<Result> result;
    };
    void run() {
        fog::diagnostics::observeThread("settings-save");
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            std::size_t selected = slots_.size();
            for (std::size_t i = 0; i < slots_.size(); ++i) {
                if (slots_[i].pending && (selected == slots_.size() ||
                    slots_[i].pending->due < slots_[selected].pending->due)) selected = i;
            }
            if (selected == slots_.size()) {
                if (closing_) return;
                changed_.wait(lock);
                continue;
            }
            const auto due = slots_[selected].pending->due;
            if (!closing_ && Clock::now() < due) {
                changed_.wait_until(lock, due);
                continue;
            }
            auto job = std::move(*slots_[selected].pending);
            slots_[selected].pending.reset();
            working_ = true;
            lock.unlock();
            bool saved = true;
            try { job.save(); } catch (...) { saved = false; }
            lock.lock();
            auto& slot = slots_[selected];
            // A slow older write cannot report success/failure for newer edits.
            if (slot.revision == job.revision)
                slot.result = Result{saved, std::move(job.complete)};
            working_ = false;
            changed_.notify_all();
        }
    }
    const std::chrono::milliseconds delay_;
    const bool asynchronous_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::array<Slot, static_cast<std::size_t>(Key::Count)> slots_;
    bool closing_ = false;
    bool working_ = false;
    std::thread worker_;
};
}
