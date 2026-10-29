#pragma once
#include "features/streaming/domain/streaming_client.hpp"
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
    std::uint64_t audioAgeDropped = 0, audioCapacityDropped = 0, audioPriorityPopped = 0;
    std::size_t videoHigh = 0, audioHigh = 0, bytesHigh = 0;
    std::size_t videoCount = 0, audioCount = 0, videoBytes = 0, audioBytes = 0;
    enum class PressureReason { None, Count, Bytes, Age };
    struct Pressure {
        PressureReason reason = PressureReason::None;
        std::uint64_t atMs = 0, videoAgeMs = 0, audioAgeMs = 0;
        std::size_t videoCount = 0, audioCount = 0, videoBytes = 0, audioBytes = 0, incomingBytes = 0;
    };
    Pressure lastPressure;
    std::uint64_t pressureCount = 0;
    std::deque<Packet> packets;
    void recordVideoPressure(PressureReason reason, std::uint64_t now, std::size_t incomingBytes = 0) {
        lastPressure = {reason, now, age(Kind::Video, now), age(Kind::Audio, now),
            videoCount, audioCount, videoBytes, audioBytes, incomingBytes};
        ++pressureCount;
    }
    std::string pressureText() const {
        const char* reason = lastPressure.reason == PressureReason::Count ? "count" :
            lastPressure.reason == PressureReason::Bytes ? "bytes" :
            lastPressure.reason == PressureReason::Age ? "age" : "none";
        return " queuePressureCount=" + std::to_string(pressureCount) + " pressureReason=" + reason +
            " pressureAtMs=" + std::to_string(lastPressure.atMs) +
            " pressureVideoCount=" + std::to_string(lastPressure.videoCount) +
            " pressureAudioCount=" + std::to_string(lastPressure.audioCount) +
            " pressureVideoBytes=" + std::to_string(lastPressure.videoBytes) +
            " pressureAudioBytes=" + std::to_string(lastPressure.audioBytes) +
            " pressureVideoAgeMs=" + std::to_string(lastPressure.videoAgeMs) +
            " pressureAudioAgeMs=" + std::to_string(lastPressure.audioAgeMs) +
            " pressureIncomingBytes=" + std::to_string(lastPressure.incomingBytes) +
            " audioAgeDropped=" + std::to_string(audioAgeDropped) +
            " audioCapacityDropped=" + std::to_string(audioCapacityDropped) +
            " audioPriorityPopped=" + std::to_string(audioPriorityPopped);
    }
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
                ++audioDropped; ++audioAgeDropped; erase(it);
            } else ++it;
    }
    bool push(Packet packet, std::uint64_t now) {
        expireAudio(now);
        // Queue residence starts after assembly. Preserve firstPacketMs through
        // admission so repair/reordering remains part of receive-to-present age.
        packet.enqueueMs = now;
        const auto size = packet.data.size();
        if (packet.kind == Kind::Video) {
            if (videoCount >= MaxVideo || size > MaxVideoBytes - videoBytes) {
                recordVideoPressure(videoCount >= MaxVideo ? PressureReason::Count : PressureReason::Bytes, now, size);
                return false;
            }
            ++videoCount; videoBytes += size;
        } else {
            if (size > MaxAudioBytes) { ++audioRejected; return true; }
            while (audioCount >= MaxAudio || size > MaxAudioBytes - audioBytes) {
                auto it = std::find_if(packets.begin(), packets.end(), [](const Packet& p) { return p.kind == Kind::Audio; });
                ++audioDropped; ++audioCapacityDropped; erase(it);
            }
            ++audioCount; audioBytes += size;
        }
        packets.push_back(std::move(packet));
        videoHigh = std::max(videoHigh, videoCount); audioHigh = std::max(audioHigh, audioCount);
        bytesHigh = std::max(bytesHigh, videoBytes + audioBytes);
        return true;
    }
    bool popAudio(Packet& packet) {
        auto it = std::find_if(packets.begin(), packets.end(), [](const Packet& p) { return p.kind == Kind::Audio; });
        if (it == packets.end()) return false;
        packet = std::move(*it);
        --audioCount; audioBytes -= packet.data.size();
        packets.erase(it); ++audioPriorityPopped;
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
