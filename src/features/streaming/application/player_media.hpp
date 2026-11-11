#pragma once
#include "features/streaming/domain/compressed_media_source.hpp"
#include "features/streaming/domain/media_telemetry.hpp"
#include <chrono>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>

namespace fog::stream {
// Application-level codec failure. The production adapter translates Media's
// error without bringing SDL/FFmpeg types into the player owner.
class PlayerVideoError : public std::runtime_error {
public:
    PlayerVideoError(int code, bool recoverable, const std::string& message)
        : std::runtime_error(message), code_(code), recoverable_(recoverable) {}
    int code() const noexcept { return code_; }
    bool recoverable() const noexcept { return recoverable_; }
private:
    int code_;
    bool recoverable_;
};
class MediaPresentation {
public:
    virtual ~MediaPresentation() = default;
    virtual bool draw() = 0;
    // Production calls Media::present(): SDL return then its existing one-time
    // uploaded/drawn-frame acknowledgement. No player rendering takes place.
    virtual void present() = 0;
};
class PlayerMedia : public MediaPresentation {
public:
    enum class WorkerPolicy { FogHub, Mts };
    struct WorkerError { std::exception_ptr error; ::stream::Packet packet; bool hasPacket = false; };
    struct Progress {
        std::uint64_t decoded=0,uploaded=0,presented=0,superseded=0,audio=0,audioUnderruns=0,audioResets=0;
        std::uint64_t workerDecodeUs=0,workerConvertUs=0,workerGeneration=0;
        int width=0,height=0;
        bool hardware=false,waiting=false,workers=false;
    };
    virtual ~PlayerMedia() = default;
    virtual void setVideoCodec(::stream::VideoCodec) = 0;
    virtual void setPresentationFrameRate(int) = 0;
    virtual void startWorkers(::stream::CompressedMediaSource&, int, WorkerPolicy) = 0;
    virtual void stopWorkers() noexcept = 0;
    virtual bool workersRunning() const = 0;
    virtual bool takeWorkerError(WorkerError&) = 0;
    virtual std::uint64_t workerVideoGeneration() const = 0;
    virtual bool waitingForVideoKeyframe() const = 0;
    virtual std::uint64_t presentedFrames() const = 0;
    virtual void resetVideoDecoder() = 0;
    virtual void video(const ::stream::Packet&) = 0;
    virtual void audio(const ::stream::Packet&) = 0;
    virtual void checkAudioRecovery() const = 0;
    // Preserve the real Media queue/admission/generation contract. True means
    // prepare succeeded; it does not claim SDL return or physical scanout.
    virtual bool prepare(std::chrono::steady_clock::time_point refreshSlot) = 0;
    virtual Progress progress() const = 0;
    virtual ::fog::media::telemetry::Snapshot captureTelemetry() const noexcept { return {}; }
    virtual std::string videoDiagnostics() const = 0;
    virtual std::string audioDiagnostics() const = 0;
    virtual std::string imageDiagnostics() const = 0;
    virtual std::string latencyDiagnostics() const = 0;
};
class PlayerMediaFactory {
public:
    virtual ~PlayerMediaFactory() = default;
    virtual std::unique_ptr<PlayerMedia> create() = 0;
    virtual bool supportsH265() const = 0;
};
}
