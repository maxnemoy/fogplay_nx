#pragma once
#include "features/streaming/domain/video_codec.hpp"
#include "features/streaming/domain/streaming_client.hpp"
#include <cstddef>
#include <cstdint>
#include <chrono>
#include <memory>
#include <functional>
#include <exception>
#include <stdexcept>
#include <string>
struct SDL_Renderer;
namespace fog {
// Only codec failures are candidates for the application's bounded recovery.
// what() contains metadata and FFmpeg's fixed error text, never packet payload.
class VideoDecodeError : public std::runtime_error {
public:
    VideoDecodeError(int code, bool recoverable, const std::string& message)
        : std::runtime_error(message), code_(code), recoverable_(recoverable) {}
    int code() const noexcept { return code_; }
    bool recoverable() const noexcept { return recoverable_; }
private:
    int code_;
    bool recoverable_;
};
// Audio recovery exhausted its bounded budget. No packet/device text is logged.
class AudioPlaybackError : public std::runtime_error {
public:
    explicit AudioPlaybackError(const char* message) : std::runtime_error(message) {}
};
// Texture and presentation methods belong to the constructing application thread.
// Optional workers consume the transport's synchronized compressed queues.
class Media {
public:
    // Install before starting workers. Reporter executes on an observer thread;
    // it must only enqueue diagnostics, never call UI/media or block on storage.
    void setVideoStallReporter(std::function<void(const std::string&)> reporter);
    // Observe the blocking SDL2 presentation call. Return is not proof of display scanout.
    void present();
    struct WorkerError {
        std::exception_ptr error;
        ::stream::Packet packet;
        bool hasPacket = false;
    };
    // Deferred callers explicitly prepare the latest output after feeding a batch.
    // Other callers retain automatic presentation preparation in video().
    explicit Media(SDL_Renderer* renderer, ::stream::VideoCodec codec = ::stream::VideoCodec::H264,
        bool deferredPresentation = false);
    static bool supportsH265();
    // Application thread. Reinitializes video state/cache while retaining audio.
    // With workers this schedules a command and immediately invalidates ready output.
    void setVideoCodec(::stream::VideoCodec codec);
    // Application thread before workers: enable bounded presentation at 30/60
    // FPS. Direct callers that omit this retain the legacy unpaced behavior.
    void setPresentationFrameRate(int fps);
    ~Media();
    Media(const Media&) = delete;
    Media& operator=(const Media&) = delete;
    // Client must outlive workers. Stop/join BEFORE closing/retrying transport.
    // Video errors pause decode until the application applies its recovery policy
    // and resetVideoDecoder(). Audio recovers independently; only an exhausted
    // recovery budget or an unexpected failure pauses audio and reports an error.
    void startWorkers(::stream::StreamingClient& client, int fps);
    void stopWorkers() noexcept;
    bool workersRunning() const;
    bool takeWorkerError(WorkerError& error);
    std::uint64_t workerVideoGeneration() const;
    void video(const std::byte* data, std::size_t size,
        std::uint64_t firstPacketMs = 0, std::uint64_t enqueueMs = 0);
    // Recreate video resources, retaining the last texture, counters and audio.
    // With workers this schedules a command without waiting for codec work.
    // Following packets are ignored until an IDR with matching VPS (HEVC), SPS and PPS arrives.
    void resetVideoDecoder();
    bool waitingForVideoKeyframe() const;
    // Transport enqueue time permits stale compressed backlog to be discarded
    // after audio recovery. Zero retains compatibility with direct callers.
    void audio(const std::byte* data, std::size_t size, std::uint64_t enqueueMs = 0);
    // Application loop: enforce the synchronous recovery deadline even when
    // no new audio arrives. Workers enforce it on their own audio thread.
    void checkAudioRecovery() const;
    // Synchronous: transfer/convert/upload latest batch output. With workers:
    // take a due converted frame and upload only. Codec transfer failures arrive
    // through takeWorkerError(). Always application thread; false if none due.
    bool preparePresentation();
    // Synchronous callers use the refresh slot captured before variable decode
    // work. A successful upload advances that slot, never its completion time.
    bool preparePresentation(std::chrono::steady_clock::time_point presentationSlot);
    // Draws only the last successfully uploaded texture.
    bool draw();
    // Call immediately after SDL_RenderPresent returns. Only a new uploaded
    // generation from the last successful draw counts, once.
    void presented();
    std::uint64_t videoFrames() const;
    std::uint64_t uploadedFrames() const;
    std::uint64_t presentedFrames() const;
    std::uint64_t supersededFrames() const;
    // Successfully decoded Opus packets, including PCM suppressed in worker catchup.
    std::uint64_t audioPackets() const;
    // Samples queued audio. Poll each application loop: counts observed empty
    // episodes lasting >=40 ms after priming, not exact device underruns.
    std::uint64_t audioUnderruns() const;
    std::uint64_t audioQueueResets() const;
    std::string audioDiagnostics() const;
    bool hardwareVideo() const;
    int videoWidth() const;
    int videoHeight() const;
    // Numeric decoder/HW metadata and cumulative stage timings; no payload.
    std::string diagnostics() const;
    // Application thread only. Sparse CPU image statistics and SDL submission
    // stages; no screenshot, GPU readback or proof of physical screen contents.
    std::string imageDiagnostics() const;
    // Application thread. Last 5s of completed new-frame presentations, excluding
    // redraws; local receive-to-SDL-return age is not display scanout latency.
    std::string latencyDiagnostics() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
