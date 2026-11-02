#pragma once
#include "core/domain/identity.hpp"
#include "features/settings/domain/video_settings.hpp"
#include "features/streaming/domain/controller_profile.hpp"
#include "features/streaming/domain/remote_input.hpp"
#include "features/streaming/domain/rumble.hpp"
#include "features/streaming/domain/input_suspension.hpp"
#include "features/streaming/domain/media_signal.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
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
struct PlaybackObservation {
    std::uint64_t presentedFrames = 0, decodedFrames = 0, supersededFrames = 0;
    std::uint64_t workUs = 0, errors = 0, audioUnderruns = 0, audioResets = 0;
    bool recovering = false;
    int width = 0, height = 0;
    // Cumulative video decode + conversion work on a separate worker. Never
    // add overlapping UI and worker times as utilization of one thread.
    std::uint64_t decoderWorkUs = 0, videoGeneration = 0;
    bool workerVideo = false;
};
struct Gamepad {
    std::array<int, 18> buttons{};
    std::array<float, 4> axes{};
    float rightTrigger = 0;
    float leftTrigger = 0;
    std::uint64_t sampledAtUs = 0; // Local pad poll timestamp; never serialized to the host.
};
struct StreamConnection {
    fog::EntityId rental;
    // Only resume the already running app on the authenticated rental host.
    bool resumeOnly = false;
};
enum class RecoveryState { Connected, Disconnected, RetryableFailure, TerminalFailure };
class StreamingClient {
public:
    virtual ~StreamingClient() = default;
    virtual bool start(const StreamConnection& connection) = 0;
    // Preferences are retained across start(); codec changes require a new connection.
    virtual bool configureVideo(const fog::video::Settings& settings) = 0;
    // Pre-start transport preference, retained across start(). Relay never falls back to direct.
    virtual bool configureRelay(bool relay) = 0;
    // Pre-start controller preference, retained across start(). The host chooses
    // the virtual device supported by its driver for the requested profile.
    virtual bool configureController(ControllerProfile profile) = 0;
    virtual bool updateVideoSettings(const fog::video::Settings& settings) = 0;
    virtual VideoCodec selectedVideoCodec() const = 0;
    virtual std::string qualityStatus() const = 0;
    virtual void pump() = 0;
    // Application thread, once per frame after draining packets. Counters are cumulative per session.
    virtual void observePlayback(const PlaybackObservation& observation) = 0;
    virtual bool poll(Packet& packet) = 0;
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
    virtual std::uint64_t queuedVideoAgeMs() const = 0;
    virtual std::size_t queuedVideoPackets() const = 0;
    virtual bool ready() const = 0;
    virtual std::string status() const = 0;
    virtual std::string error() const = 0;
    virtual bool retryableError() const = 0; // Fixed transport failures/timeouts only; persists after teardown.
    virtual bool disconnected() const = 0; // Live peer or ICE Disconnected; application applies its grace period.
    // A transient disconnection can recover on the current transport during
    // grace. Failed/terminated adapters override when callbacks precede error text.
    virtual RecoveryState recoveryState() const {
        if (!error().empty()) return retryableError() ? RecoveryState::RetryableFailure : RecoveryState::TerminalFailure;
        return disconnected() ? RecoveryState::Disconnected : RecoveryState::Connected;
    }
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
    virtual std::string inputLatencyDiagnostics() const { return {}; }
    // Optional bounded performance samples; never query transport SDKs on the frame thread.
    virtual std::string serverPerformanceDiagnostics() const { return {}; }
    virtual std::string controlDeliveryDiagnostics() const { return {}; }
    virtual bool pollVideoDiagnostic(std::string& event) = 0;
    // Sanitized, bounded incoming control samples. final flushes pending counts.
    virtual bool pollControlDiagnostic(std::string&, bool final = false) { (void)final; return false; }
    virtual bool waitingForVideoKeyframe() const = 0;
    virtual InputSuspension inputSuspension() const { return {}; }
    bool inputSuspended() const { return inputSuspension().suspended(); }
    // Synchronized source generation, including faults already repaired into a
    // queued IDR. Assembly readiness alone does not prove decoder readiness.
    // Zero is the initial generation and the default for worker-only adapters.
    virtual std::uint64_t currentVideoGeneration() const { return 0; }
    virtual bool requestVideoKeyframe() = 0; // Best effort PLI on open video tracks; no transport reset.
    // Decoder recovery is independent of transport assembly and quality mode.
    // Keep requesting IDR until the owner successfully prepares decoded output.
    virtual void setDecoderWaitingForKeyframe(bool waiting) = 0;
    virtual bool sendGamepad(const Gamepad& state) = 0;
    // Main-thread feedback drain. Adapters coalesce network callbacks; never
    // touch controller hardware from a transport thread. Optional for fakes.
    virtual bool pollRumble(Rumble&) { return false; }
    virtual bool sendInput(const fog::remote::Input& input) = 0;
    virtual void close() = 0;
private:
    // Stable across transport retries. Producers retain a shared reference;
    // consumers are joined before client destruction.
    std::shared_ptr<MediaSignals> mediaSignals_ = std::make_shared<MediaSignals>();
};
class StreamingClientFactory {
public:
    virtual ~StreamingClientFactory() = default;
    virtual std::unique_ptr<StreamingClient> create() = 0;
};
}
