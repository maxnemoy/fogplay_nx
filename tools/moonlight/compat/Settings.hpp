#pragma once

#include <string>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <utility>
#include <vector>

// Minimal boundary for the upstream GameStream code; no Moonlight UI settings.
// Configure once before gs_init(); the caller creates this private directory.
class Settings {
public:
    static Settings& instance() {
        static Settings settings;
        return settings;
    }

    const std::string& key_dir() const { return keyDirectory_; }
    void set_key_dir(std::string directory) { keyDirectory_ = std::move(directory); }
    void set_production(bool enabled) { production_ = enabled; pinnedKey_.clear(); dispatched_.store(false); }
    bool production() const { return production_; }
    const std::string& pinned_key() const { return pinnedKey_; }
    void set_pinned_key(std::string key) { pinnedKey_ = std::move(key); }
    void pairing_dispatched() noexcept { dispatched_.store(true); }
    bool pairing_was_dispatched() const noexcept { return dispatched_.load(); }
    void request_cancel() noexcept { cancelled_.store(true); }
    void reset_cancel() noexcept {
        deadlineMs_.store(0);
        cancelled_.store(false);
    }
    void set_timeout_ms(unsigned timeout) noexcept {
        deadlineMs_.store(monotonic_ms() + timeout);
    }
    bool request_cancelled() const noexcept {
        const auto deadline = deadlineMs_.load();
        return cancelled_.load() || (deadline != 0 && monotonic_ms() >= deadline);
    }
    long remaining_timeout_ms(long requestTimeout) const noexcept {
        const auto deadline = deadlineMs_.load();
        if (deadline == 0) return requestTimeout;
        const auto remaining = deadline - monotonic_ms();
        if (remaining <= 0) return 1;
        return remaining < requestTimeout ? static_cast<long>(remaining) : requestTimeout;
    }

private:
    bool production_ = false; // Configured only while the exclusive owner is idle.
    std::string pinnedKey_;
    std::atomic<bool> dispatched_{false};
    std::string keyDirectory_ = "sdmc:/switch/fogplay_nx/moonlight-probe/keys";
    std::atomic<bool> cancelled_{false};
    std::atomic<std::int64_t> deadlineMs_{0};
    static std::int64_t monotonic_ms() noexcept {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
};
