#pragma once
#include "features/streaming/infrastructure/stream.hpp"
#include <algorithm>
#include <deque>
namespace stream {
// Independent audio/video bounds: audio may shed old Opus packets, while video
// overflow is reported to the owner, which must quarantine the prediction chain.
class MediaQueue {
public:
    static constexpr std::size_t MaxVideo = 12, MaxAudio = 12;
    static constexpr std::size_t MaxVideoBytes = 16 * 1024 * 1024, MaxAudioBytes = 256 * 1024;
    static constexpr std::uint64_t MaxVideoAgeMs = 250, MaxAudioAgeMs = 120;
    std::uint64_t audioDropped = 0, audioRejected = 0, videoAgeHighMs = 0, audioAgeHighMs = 0;
    std::size_t videoHigh = 0, audioHigh = 0, bytesHigh = 0;
    std::size_t videoCount = 0, audioCount = 0, videoBytes = 0, audioBytes = 0;
    std::deque<Packet> packets;
    std::uint64_t age(Kind kind, std::uint64_t now) const {
        for (const auto& packet : packets)
            if (packet.kind == kind) return now >= packet.enqueueMs ? now - packet.enqueueMs : 0;
        return 0;
    }
    void erase(std::deque<Packet>::iterator& it) {
        if (it->kind == Kind::Video) { --videoCount; videoBytes -= it->data.size(); }
        else { --audioCount; audioBytes -= it->data.size(); }
        it = packets.erase(it);
    }
    void clearVideo() {
        for (auto it = packets.begin(); it != packets.end();)
            if (it->kind == Kind::Video) erase(it); else ++it;
    }
    void expireAudio(std::uint64_t now) {
        videoAgeHighMs = std::max(videoAgeHighMs, age(Kind::Video, now));
        audioAgeHighMs = std::max(audioAgeHighMs, age(Kind::Audio, now));
        for (auto it = packets.begin(); it != packets.end();)
            if (it->kind == Kind::Audio && now >= it->enqueueMs && now - it->enqueueMs > MaxAudioAgeMs) {
                ++audioDropped; erase(it);
            } else ++it;
    }
    bool push(Packet packet, std::uint64_t now) {
        expireAudio(now);
        packet.enqueueMs = now;
        const auto size = packet.data.size();
        if (packet.kind == Kind::Video) {
            if (videoCount >= MaxVideo || size > MaxVideoBytes - videoBytes) return false;
            ++videoCount; videoBytes += size;
        } else {
            if (size > MaxAudioBytes) { ++audioRejected; return true; }
            while (audioCount >= MaxAudio || size > MaxAudioBytes - audioBytes) {
                auto it = std::find_if(packets.begin(), packets.end(), [](const Packet& p) { return p.kind == Kind::Audio; });
                ++audioDropped; erase(it);
            }
            ++audioCount; audioBytes += size;
        }
        packets.push_back(std::move(packet));
        videoHigh = std::max(videoHigh, videoCount); audioHigh = std::max(audioHigh, audioCount);
        bytesHigh = std::max(bytesHigh, videoBytes + audioBytes);
        return true;
    }
    bool pop(Packet& packet) {
        if (packets.empty()) return false;
        packet = std::move(packets.front());
        if (packet.kind == Kind::Video) { --videoCount; videoBytes -= packet.data.size(); }
        else { --audioCount; audioBytes -= packet.data.size(); }
        packets.pop_front(); return true;
    }
};
}
