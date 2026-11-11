#pragma once
#include "features/streaming/domain/compressed_media_source.hpp"
#include <atomic>
#include <deque>
#include <future>
#include <mutex>

// The real Media workers consume this synchronized compressed source. Completion
// acknowledges the production decode/convert/enqueue iteration, not just poll().
class MediaExperimentClient final : public stream::CompressedMediaSource {
public:
    explicit MediaExperimentClient(stream::VideoCodec codec) : codec_(codec) {}
    std::atomic<std::uint64_t> generation{1};
    std::future<bool> push(const std::vector<unsigned char>& bytes, std::uint64_t sourceGeneration = 0) {
        auto completion = std::make_shared<Completion>();
        auto result = completion->result.get_future();
        stream::Packet packet;
        packet.data.assign(reinterpret_cast<const std::byte*>(bytes.data()),
            reinterpret_cast<const std::byte*>(bytes.data() + bytes.size()));
        packet.videoGeneration = sourceGeneration ? sourceGeneration : generation.load();
        packet.completion = std::move(completion);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            packet.timestamp = ++timestamp_ * 1500;
            video_.push_back(std::move(packet));
        }
        mediaSignals()->video.notify();
        return result;
    }
    bool pollVideo(stream::Packet& packet) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (video_.empty()) return false;
        packet = std::move(video_.front()); video_.pop_front(); return true;
    }
    bool pollAudio(stream::Packet&) override { return false; }
    std::uint64_t currentVideoGeneration() const override { return generation.load(); }
    std::size_t queuedVideoPackets() const { std::lock_guard<std::mutex> lock(mutex_); return video_.size(); }
    stream::VideoCodec selectedVideoCodec() const override { return codec_; }
private:
    struct Completion final : stream::VideoFrameCompletion {
        std::promise<bool> result;
        void complete(bool decoded) noexcept override { try { result.set_value(decoded); } catch (...) {} }
    };
    const stream::VideoCodec codec_;
    mutable std::mutex mutex_;
    std::deque<stream::Packet> video_;
    std::uint32_t timestamp_ = 0;
};
