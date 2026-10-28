#pragma once
#include "features/streaming/domain/video_codec.hpp"
#include "features/streaming/domain/streaming_client.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
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
// Texture and presentation methods belong to the constructing application thread.
// Optional workers consume the transport's synchronized compressed queues.
class Media {
public:
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
    ~Media();
    Media(const Media&) = delete;
    Media& operator=(const Media&) = delete;
    // Client must outlive workers. Stop/join BEFORE closing/retrying transport.
    // Video errors pause decode until the application applies its recovery policy
    // and resetVideoDecoder(); audio errors pause audio until teardown.
    void startWorkers(::stream::StreamingClient& client, int fps);
    void stopWorkers() noexcept;
    bool workersRunning() const;
    bool takeWorkerError(WorkerError& error);
    std::uint64_t workerVideoGeneration() const;
    void video(const std::byte* data, std::size_t size);
    // Recreate video resources, retaining the last texture, counters and audio.
    // With workers this schedules a command without waiting for codec work.
    // Following packets are ignored until an IDR with matching VPS (HEVC), SPS and PPS arrives.
    void resetVideoDecoder();
    bool waitingForVideoKeyframe() const;
    void audio(const std::byte* data, std::size_t size);
    // Synchronous: transfer/convert/upload latest batch output. With workers:
    // take a due converted frame and upload only. Codec transfer failures arrive
    // through takeWorkerError(). Always application thread; false if none due.
    bool preparePresentation();
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
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
