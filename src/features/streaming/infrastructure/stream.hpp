#pragma once
#include "features/streaming/domain/streaming_client.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace stream {
// Application-thread controller. Callbacks store bounded data; a serialized worker
// sends input and pongs. Public enqueue/pump calls never send data-channel traffic.
// Call pump every frame, then drain poll. The owner chooses bounded transport retry or session finish after an error.
class Client final : public StreamingClient {
public:
    Client();
    Client(std::string caPath, std::string random5suffix);
    ~Client() override;
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    bool start(const StreamConnection& connection) override;
    // Explicit MTS transport arguments are limited to this adapter and protocol fixtures.
    bool start(const std::string& sessionHash, const std::string& random5suffix,
               const std::string& caPath);
    // Preferences are retained across start(); codec changes require a new connection.
    bool configureVideo(const fog::video::Settings& settings) override;
    // Pre-start transport preference, retained across start(). Relay never falls back to direct.
    bool configureRelay(bool relay) override;
    // Pre-start controller preference, retained across start().
    bool configureController(ControllerProfile profile) override;
    bool updateVideoSettings(const fog::video::Settings& settings) override;
    VideoCodec selectedVideoCodec() const override;
    std::string qualityStatus() const override;
    void pump() override;
    // Application thread, once per frame after draining packets. Counters are cumulative per session.
    void observePlayback(const PlaybackObservation& observation) override;
    bool poll(Packet& packet) override;
    bool pollAudio(Packet& packet) override; // Audio FIFO, independent of queued compressed video.
    bool pollRumble(Rumble& rumble) override; // Latest slot-0 feedback, drained on the application thread.
    std::uint64_t queuedVideoAgeMs() const override;
    std::size_t queuedVideoPackets() const override;
    bool ready() const override;
    std::string status() const override;
    std::string error() const override;
    bool retryableError() const override; // Fixed transport failures/timeouts only; persists after teardown.
    bool disconnected() const override; // Live peer or ICE Disconnected; application applies its grace period.
    bool disconnectForRetry() override; // False if pending terminal evidence forbids retry; otherwise release transports.
    // Fixed operation names and counters only; no remote fields or identifiers.
    // STOMP send acceptance is local (buffered included), not a broker receipt.
    std::string diagnostics() const override;
    // Application-thread lifetime stages; nested scopes are not additive.
    std::string transportTimings() const override;
    std::string videoDiagnostics() const override;
    std::string gapDiagnostics() const override;
    std::string queueDiagnostics() const override;
    std::string mutexDiagnostics() const override;
    std::string dtlsDiagnostics() const override;
    std::string rtcpDiagnostics() const override;
    // Local queue/RTC acceptance, scoped to this transport generation. No remote input ACK exists.
    std::string inputDiagnostics() const override;
    bool pollVideoDiagnostic(std::string& event) override;
    bool pollControlDiagnostic(std::string& event, bool final = false) override;
    bool waitingForVideoKeyframe() const override;
    bool requestVideoKeyframe() override; // Best effort PLI on open video tracks; no transport reset.
    // Decoder recovery is independent of transport assembly and quality mode.
    // Keep requesting IDR until the owner successfully prepares decoded output.
    void setDecoderWaitingForKeyframe(bool waiting) override;
    bool sendGamepad(const Gamepad& state) override;
    bool sendInput(const fog::remote::Input& input) override;
    // MTS wire entry point. Kept on the concrete adapter for protocol validation tests.
    bool sendControl(const std::string& json);
    void close() override;
    // After destroying all Clients, before shutting down the platform socket runtime.
    static void cleanupRuntime();
#ifdef FOG_STREAM_TESTING
    // One-shot send fixture: reject before send, or report buffered after real
    // loopback delivery. This method does not exist in the production build.
    void overrideSignalingSendForTesting(bool enabled, bool reject);
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
    std::string caPath_, random5suffix_;
};
}
