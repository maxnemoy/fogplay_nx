#pragma once
#include "features/streaming/domain/stream_telemetry.hpp"
#include "features/streaming/domain/compressed_media_source.hpp"
#include "features/streaming/domain/stream_input_sink.hpp"
#include "features/streaming/domain/stream_lifecycle.hpp"
#include "features/streaming/domain/playback_observation.hpp"
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
// Compatibility aggregate. Focused consumers depend on the port they actually use.
class StreamingClient : public CompressedMediaSource, public StreamInputSink, public StreamLifecycle, public StreamTelemetrySource {
public:
    virtual ~StreamingClient() = default;
    virtual std::string qualityStatus() const = 0;
    // Application thread, once per frame after draining packets. Counters are cumulative per session.
    virtual void observePlayback(const PlaybackObservation& observation) = 0;
    virtual bool poll(Packet& packet) = 0;
    virtual std::uint64_t queuedVideoAgeMs() const = 0;
    virtual std::size_t queuedVideoPackets() const = 0;
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
    virtual bool requestVideoKeyframe() = 0; // Best effort PLI on open video tracks; no transport reset.
    // Decoder recovery is independent of transport assembly and quality mode.
    // Keep requesting IDR until the owner successfully prepares decoded output.
    virtual void setDecoderWaitingForKeyframe(bool waiting) = 0;
};
class StreamingClientFactory {
public:
    virtual ~StreamingClientFactory() = default;
    virtual std::unique_ptr<StreamingClient> create() = 0;
};
}
