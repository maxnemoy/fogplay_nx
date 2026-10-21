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
struct Gamepad {
    std::array<int, 18> buttons{};
    std::array<float, 4> axes{};
    float rightTrigger = 0;
    float leftTrigger = 0;
};
// Single application-thread controller. Network callbacks only enqueue bounded data.
// Call pump every frame, then drain poll. Errors require the owner to finish its session.
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
    bool updateVideoSettings(const fog::video::Settings& settings);
    VideoCodec selectedVideoCodec() const;
    std::string qualityStatus() const;
    void pump();
    // Application thread, once per frame after draining packets. Counters are cumulative per session.
    void observePlayback(std::uint64_t decodedFrames, std::uint64_t videoWorkUs,
                         std::uint64_t decodeErrors, bool recovering, int width, int height);
    bool poll(Packet& packet);
    std::size_t queuedVideoPackets() const;
    bool ready() const;
    std::string status() const;
    std::string error() const;
    // Fixed operation names and counters only; no remote fields or identifiers.
    std::string diagnostics() const;
    std::string videoDiagnostics() const;
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
    void blockControlForTesting(bool blocked);
    void advanceControlForTesting(unsigned milliseconds);
#endif
private:
    bool startEndpoint(const std::string& hash, const std::string& suffix,
                       const std::string& caPath, const std::string& url);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
