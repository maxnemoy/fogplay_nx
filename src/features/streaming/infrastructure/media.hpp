#pragma once
#include "features/streaming/domain/video_codec.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
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
// Used only on the application thread. WebRTC callbacks queue compressed data.
class Media {
public:
    // Deferred callers explicitly prepare the latest output after feeding a batch.
    // Other callers retain automatic presentation preparation in video().
    explicit Media(SDL_Renderer* renderer, ::stream::VideoCodec codec = ::stream::VideoCodec::H264,
        bool deferredPresentation = false);
    static bool supportsH265();
    // Serialized by the application thread, before submitting the new track.
    // Reinitializes video state and parameter cache while retaining audio.
    void setVideoCodec(::stream::VideoCodec codec);
    ~Media();
    Media(const Media&) = delete;
    Media& operator=(const Media&) = delete;
    void video(const std::byte* data, std::size_t size);
    // Recreate video resources, retaining the last texture, counters and audio.
    // Following packets are ignored until an IDR with matching VPS (HEVC), SPS and PPS arrives.
    void resetVideoDecoder();
    bool waitingForVideoKeyframe() const;
    void audio(const std::byte* data, std::size_t size);
    // Transfer, convert and upload the latest decoded output once per batch.
    // Returns false without a pending output; codec transfer failures stay here.
    bool preparePresentation();
    // Draws only the last successfully uploaded texture.
    bool draw();
    std::uint64_t videoFrames() const;
    std::uint64_t audioPackets() const;
    bool hardwareVideo() const;
    int videoWidth() const;
    int videoHeight() const;
    // Numeric decoder/HW metadata and cumulative stage timings; no payload.
    std::string diagnostics() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
