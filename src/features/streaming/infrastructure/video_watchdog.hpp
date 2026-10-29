#pragma once
#include "core/diagnostics/thread_observation.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace fog::detail {
// Each lane has one owner (decoder or application thread). The observer never
// touches AVFrame/SDL/GPU objects and never tries to recover or free them.
class VideoFlight {
public:
    enum class Stage { Idle, DecoderInit, DecodeSend, DecodeReceive, FrameRelease, DecoderRelease,
        Transfer, Convert, TextureUpload, TextureRelease, Draw, Present };
    struct Metadata {
        int width = 0, height = 0, codec = 0, format = 0, stride = 0;
        std::uint64_t bytes = 0, nalTypes = 0, frame = 0;
    };
    struct Snapshot { std::uint64_t sequence = 0, began = 0; Stage stage = Stage::Idle; Metadata metadata; };
    static std::uint64_t now() noexcept {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    static const char* name(Stage stage) noexcept {
        switch (stage) {
            case Stage::DecoderInit: return "decoder-init";
            case Stage::DecodeSend: return "decode-send";
            case Stage::DecodeReceive: return "decode-receive";
            case Stage::FrameRelease: return "frame-release";
            case Stage::DecoderRelease: return "decoder-release";
            case Stage::Transfer: return "hardware-transfer";
            case Stage::Convert: return "pixel-convert";
            case Stage::TextureUpload: return "texture-upload";
            case Stage::TextureRelease: return "texture-release";
            case Stage::Draw: return "render-draw";
            case Stage::Present: return "render-present";
            default: return "idle";
        }
    }
    void begin(Stage stage, const Metadata& data, std::uint64_t at = now()) noexcept {
        ++revision_;
        ++sequence_; began_ = at; stage_ = stage;
        width_ = data.width; height_ = data.height; codec_ = data.codec;
        format_ = data.format; stride_ = data.stride;
        bytes_ = data.bytes; nalTypes_ = data.nalTypes; frame_ = data.frame;
        ++revision_;
    }
    void end() noexcept {
        ++revision_; stage_ = Stage::Idle; ++revision_;
    }
    bool read(Snapshot& out) const noexcept {
        // Bounded retry even if the writer itself stalls during publication.
        for (unsigned attempt = 0; attempt < 3; ++attempt) {
            const auto before = revision_.load();
            if (before & 1) continue;
            Snapshot next;
            next.sequence = sequence_; next.began = began_; next.stage = stage_;
            next.metadata = {width_.load(), height_.load(), codec_.load(), format_.load(), stride_.load(),
                bytes_.load(), nalTypes_.load(), frame_.load()};
            if (before == revision_.load()) { out = next; return true; }
        }
        return false;
    }
    class Scope {
    public:
        Scope(VideoFlight& flight, Stage stage, const Metadata& data) : flight_(flight) { flight_.begin(stage, data); }
        ~Scope() { finish(); }
        void finish() noexcept { if (active_) { flight_.end(); active_ = false; } }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    private:
        VideoFlight& flight_;
        bool active_ = true;
    };
private:
    std::atomic<std::uint64_t> revision_{0}, sequence_{0}, began_{0};
    std::atomic<Stage> stage_{Stage::Idle};
    std::atomic<int> width_{0}, height_{0}, codec_{0}, format_{0}, stride_{0};
    std::atomic<std::uint64_t> bytes_{0}, nalTypes_{0}, frame_{0};
};

class VideoWatchdog {
public:
    using Reporter = std::function<void(const std::string&)>;
    VideoFlight decoder, presentation;
    ~VideoWatchdog() { stop(); }
    void start(Reporter reporter) {
        stop();
        if (!reporter) return;
        reporter_ = std::move(reporter);
        budgets_ = {}; total_ = 0;
        stopping_ = false;
        try { worker_ = std::thread([this] {
        fog::diagnostics::observeThread("video-watchdog");
            std::unique_lock<std::mutex> lock(mutex_);
            while (!wake_.wait_for(lock, std::chrono::milliseconds(100), [this] { return stopping_.load(); })) {
                lock.unlock();
                try { inspect(VideoFlight::now(), reporter_); } catch (...) { /* Diagnostics must not terminate playback. */ }
                lock.lock();
            }
        }); } catch (...) {
            stopping_ = true;
            // Optional diagnostics must not prevent an otherwise usable stream.
            try { reporter_("observer unavailable; video stall diagnostics disabled"); } catch (...) {}
        }
    }
    void stop() noexcept {
        stopping_ = true; wake_.notify_all();
        if (worker_.joinable()) worker_.join();
    }
    // Observer-thread only; explicit time allows deterministic budget tests.
    void inspect(std::uint64_t now, const Reporter& report) {
        const std::array<const VideoFlight*, 2> lanes{&decoder, &presentation};
        for (std::size_t i = 0; i < lanes.size(); ++i) {
            VideoFlight::Snapshot state;
            if (!lanes[i]->read(state) || state.stage == VideoFlight::Stage::Idle ||
                now < state.began || now - state.began < 500) continue;
            auto& budget = budgets_[i];
            if (budget.sequence != state.sequence) budget = {state.sequence, 0, 0};
            if (total_ >= 128 || budget.count >= 4 || (budget.count && now - budget.at < 5000)) continue;
            ++budget.count; budget.at = now; ++total_;
            const auto& m = state.metadata;
            report(std::string("stall lane=") + (i ? "presentation" : "decoder") +
                " stage=" + VideoFlight::name(state.stage) + " sequence=" + std::to_string(state.sequence) +
                " monotonicMs=" + std::to_string(now) + " elapsedMs=" + std::to_string(now-state.began) +
                " width=" + std::to_string(m.width) + " height=" + std::to_string(m.height) +
                " codec=" + std::to_string(m.codec) + " format=" + std::to_string(m.format) +
                " stride=" + std::to_string(m.stride) + " bytes=" + std::to_string(m.bytes) +
                " nalTypes=" + std::to_string(m.nalTypes) + " frame=" + std::to_string(m.frame));
        }
    }
private:
    struct Budget { std::uint64_t sequence = 0, at = 0; unsigned count = 0; };
    std::array<Budget, 2> budgets_{};
    unsigned total_ = 0;
    Reporter reporter_;
    std::atomic<bool> stopping_{false};
    std::mutex mutex_;
    std::condition_variable wake_;
    std::thread worker_;
};
}
