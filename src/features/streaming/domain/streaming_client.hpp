#pragma once
#include "core/domain/identity.hpp"
#include "features/settings/domain/video_settings.hpp"
#include "features/streaming/domain/remote_input.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace stream {
enum class Kind { Video, Audio };
struct Packet {
    Kind kind = Kind::Video;
    std::vector<std::byte> data;
    std::uint32_t timestamp = 0; // RTP clock: video 90 kHz, audio 48 kHz.
    std::int64_t firstSequence = 0, lastSequence = 0; // Video extended RTP range.
    std::uint64_t enqueueMs = 0; // Local steady clock, never a remote timestamp.
    std::uint64_t videoGeneration = 0; // Reset decoder when the next video AU changes generation.
};
struct PlaybackObservation {
    std::uint64_t presentedFrames = 0, decodedFrames = 0, supersededFrames = 0;
    std::uint64_t workUs = 0, errors = 0, audioUnderruns = 0, audioResets = 0;
    bool recovering = false;
    int width = 0, height = 0;
};
struct Gamepad {
    std::array<int, 18> buttons{};
    std::array<float, 4> axes{};
    float rightTrigger = 0;
    float leftTrigger = 0;
};
struct StreamConnection {
    fog::EntityId rental;
    // Only resume the already running app on the authenticated rental host.
    bool resumeOnly = false;
};
class StreamingClient {
public:
    virtual ~StreamingClient() = default;
    virtual bool start(const StreamConnection& connection) = 0;
    // Preferences are retained across start(); codec changes require a new connection.
    virtual bool configureVideo(const fog::video::Settings& settings) = 0;
    // Pre-start transport preference, retained across start(). Relay never falls back to direct.
    virtual bool configureRelay(bool relay) = 0;
    // Pre-start controller mode, retained across start(). Defaults to ordinary gamepad.
    virtual bool configurePlayStation(bool enabled) = 0;
    virtual bool updateVideoSettings(const fog::video::Settings& settings) = 0;
    virtual VideoCodec selectedVideoCodec() const = 0;
    virtual std::string qualityStatus() const = 0;
    virtual void pump() = 0;
    // Application thread, once per frame after draining packets. Counters are cumulative per session.
    virtual void observePlayback(const PlaybackObservation& observation) = 0;
    virtual bool poll(Packet& packet) = 0;
    virtual bool pollAudio(Packet& packet) = 0; // Audio FIFO, independent of queued compressed video.
    virtual std::uint64_t queuedVideoAgeMs() const = 0;
    virtual std::size_t queuedVideoPackets() const = 0;
    virtual bool ready() const = 0;
    virtual std::string status() const = 0;
    virtual std::string error() const = 0;
    virtual bool retryableError() const = 0; // Fixed transport failures/timeouts only; persists after teardown.
    virtual bool disconnected() const = 0; // Live peer or ICE Disconnected; application applies its grace period.
    virtual bool disconnectForRetry() = 0; // False if pending terminal evidence forbids retry; otherwise release transports.
    // Fixed operation names and counters only; no remote fields or identifiers.
    virtual std::string diagnostics() const = 0;
    // Application-thread lifetime stages; nested scopes are not additive.
    virtual std::string transportTimings() const = 0;
    virtual std::string videoDiagnostics() const = 0;
    virtual std::string gapDiagnostics() const = 0;
    virtual std::string queueDiagnostics() const = 0;
    virtual std::string mutexDiagnostics() const = 0;
    virtual std::string dtlsDiagnostics() const = 0;
    virtual std::string rtcpDiagnostics() const = 0;
    // Local queue/RTC acceptance, scoped to this transport generation. No remote input ACK exists.
    virtual std::string inputDiagnostics() const = 0;
    virtual bool pollVideoDiagnostic(std::string& event) = 0;
    virtual bool waitingForVideoKeyframe() const = 0;
    virtual bool requestVideoKeyframe() = 0; // Best effort PLI on open video tracks; no transport reset.
    // Decoder recovery is independent of transport assembly and quality mode.
    // Keep requesting IDR until the owner successfully prepares decoded output.
    virtual void setDecoderWaitingForKeyframe(bool waiting) = 0;
    virtual bool sendGamepad(const Gamepad& state) = 0;
    virtual bool sendInput(const fog::remote::Input& input) = 0;
    virtual void close() = 0;
};
class StreamingClientFactory {
public:
    virtual ~StreamingClientFactory() = default;
    virtual std::unique_ptr<StreamingClient> create() = 0;
};
}
