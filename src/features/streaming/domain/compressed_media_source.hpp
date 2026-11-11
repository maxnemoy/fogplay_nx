#pragma once
#include "features/streaming/domain/video_codec.hpp"
#include "features/streaming/domain/media_signal.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace stream {
enum class Kind { Video, Audio };
class VideoFrameCompletion {
public:
    virtual ~VideoFrameCompletion() = default;
    // Idempotent, bounded acknowledgement on the video consumer. Providers
    // retaining a decoder/core handle must fail closed if abandoned.
    virtual void complete(bool decoded) noexcept = 0;
};
struct Packet {
    Kind kind = Kind::Video;
    std::vector<std::byte> data;
    std::uint32_t timestamp = 0; // RTP clock: video 90 kHz, audio 48 kHz.
    std::int64_t firstSequence = 0, lastSequence = 0; // Video extended RTP range.
    std::uint64_t enqueueMs = 0; // Local steady clock, never a remote timestamp.
    std::uint64_t videoGeneration = 0; // Reset decoder when the next video AU changes generation.
    // Earliest received RTP packet of this video AU, in local steady milliseconds.
    // Distinct from assembled queue admission. Zero means unavailable.
    std::uint64_t firstPacketMs = 0;
    std::shared_ptr<VideoFrameCompletion> completion{};
};
// Worker-safe compressed media boundary. No input, rental, transport lifecycle
// or diagnostic formatting authority. Owner must join consumers before teardown.
class CompressedMediaSource {
public:
    virtual ~CompressedMediaSource() = default;
    virtual VideoCodec selectedVideoCodec() const = 0;
    // Independent consumers: pollVideo must never remove an audio packet.
    // Legacy poll remains for synchronous callers. Client lifetime/start/close
    // stay on the application owner; stop/join consumers before replacing it.
    virtual bool pollVideo(Packet& packet) = 0;
    // Blocking adapters retain a wake issued between the caller's last check
    // and entry into their pull. Nonblocking adapters use the ordinary path.
    virtual bool pollVideo(Packet& packet, std::uint64_t observedWakeSequence) {
        (void)observedWakeSequence;
        return pollVideo(packet);
    }
    virtual bool pollAudio(Packet& packet) = 0; // Audio FIFO, independent of queued compressed video.
    const std::shared_ptr<MediaSignals>& mediaSignals() const { return mediaSignals_; }
    // Stop/reset must interrupt both readiness waits and a provider's blocking
    // frame pull. This must not join workers or close the transport.
    virtual void wakeMediaWaiters() { mediaSignals_->notifyAll(); }
    // Worker/UI-safe admission snapshot; provider overrides cannot query SDKs.
    virtual bool mediaAdmissionAllowed() const { return true; }
    // Synchronized source generation, including faults already repaired into a
    // queued IDR. Assembly readiness alone does not prove decoder readiness.
    // Zero is the initial generation and the default for worker-only adapters.
    virtual std::uint64_t currentVideoGeneration() const { return 0; }
private:
    // Stable across transport retries. Producers retain a shared reference;
    // consumers are joined before client destruction.
    std::shared_ptr<MediaSignals> mediaSignals_ = std::make_shared<MediaSignals>();
};
}
