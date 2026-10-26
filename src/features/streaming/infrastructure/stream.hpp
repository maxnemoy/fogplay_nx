#pragma once
#include "features/settings/domain/video_settings.hpp"
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
// Application-thread controller. Callbacks store bounded data; a serialized worker
// sends input and pongs. Public enqueue/pump calls never send data-channel traffic.
// Call pump every frame, then drain poll. The owner chooses bounded transport retry or session finish after an error.
class Client {
public:
    Client();
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    bool start(const std::string& sessionHash, const std::string& random5suffix,
               const std::string& caPath);
    // Preferences are retained across start(); codec changes require a new connection.
    bool configureVideo(const fog::video::Settings& settings);
    // Pre-start transport preference, retained across start(). Relay never falls back to direct.
    bool configureRelay(bool relay);
    // Pre-start controller mode, retained across start(). Defaults to ordinary gamepad.
    bool configurePlayStation(bool enabled);
    bool updateVideoSettings(const fog::video::Settings& settings);
    VideoCodec selectedVideoCodec() const;
    std::string qualityStatus() const;
    void pump();
    // Application thread, once per frame after draining packets. Counters are cumulative per session.
    void observePlayback(const PlaybackObservation& observation);
    bool poll(Packet& packet);
    bool pollAudio(Packet& packet); // Audio FIFO, independent of queued compressed video.
    std::uint64_t queuedVideoAgeMs() const;
    std::size_t queuedVideoPackets() const;
    bool ready() const;
    std::string status() const;
    std::string error() const;
    bool retryableError() const; // Fixed transport failures/timeouts only; persists after teardown.
    bool disconnected() const; // Live peer or ICE Disconnected; application applies its grace period.
    bool disconnectForRetry(); // False if pending terminal evidence forbids retry; otherwise release transports.
    // Fixed operation names and counters only; no remote fields or identifiers.
    std::string diagnostics() const;
    // Application-thread lifetime stages; nested scopes are not additive.
    std::string transportTimings() const;
    std::string videoDiagnostics() const;
    std::string gapDiagnostics() const;
    std::string queueDiagnostics() const;
    std::string mutexDiagnostics() const;
    std::string dtlsDiagnostics() const;
    std::string rtcpDiagnostics() const;
    // Local queue/RTC acceptance, scoped to this transport generation. No remote input ACK exists.
    std::string inputDiagnostics() const;
    bool pollVideoDiagnostic(std::string& event);
    bool waitingForVideoKeyframe() const;
    bool requestVideoKeyframe(); // Best effort PLI on open video tracks; no transport reset.
    // Decoder recovery is independent of transport assembly and quality mode.
    // Keep requesting IDR until the owner successfully prepares decoded output.
    void setDecoderWaitingForKeyframe(bool waiting);
    bool sendGamepad(const Gamepad& state);
    bool sendControl(const std::string& json);
    void close();
    // After destroying all Clients, before shutting down the platform socket runtime.
    static void cleanupRuntime();
#ifdef FOG_STREAM_TESTING
    // Local fixture only. This method does not exist in the production build.
    bool startForTesting(const std::string& hash, const std::string& suffix,
                         const std::string& loopbackUrl);
    // Advance only local deadline ages without sleeping; no production symbol.
    void elapseForTesting(unsigned seconds);
    // Quality clock only; deterministic controller coverage over the real RTC channel.
    void advanceQualityForTesting(unsigned milliseconds);
    // Deterministic event ordering fixture; no network/service injection in production.
    void failThenReceiveForTesting(const std::string& text, bool signaling);
    void setSelectedCandidatesForTesting(const std::string& local, const std::string& remote);
    void clearSelectedCandidatesForTesting(bool invalidate = true);
    void failSelectedPairForTesting(bool fail);
    void setRouteSnapshotTimeForTesting(std::uint64_t now);
    void repeatIceStateForTesting();
    void addCandidateProofForTesting(const std::string& candidate);
    bool selectedRemoteMatchesForTesting(const std::string& candidate) const;
    void receiveControlForTesting(const std::string& text);
    void blockIceForTesting(bool blocked, unsigned stage = 0);
    void blockControlForTesting(bool blocked);
    void blockControlSendForTesting(bool blocked);
    void advanceControlForTesting(unsigned milliseconds);
#endif
private:
    bool startEndpoint(const std::string& hash, const std::string& suffix,
                       const std::string& caPath, const std::string& url);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
