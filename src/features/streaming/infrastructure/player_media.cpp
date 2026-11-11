#include "features/streaming/infrastructure/player_media.hpp"
#include "features/streaming/infrastructure/media.hpp"
#include "core/diagnostics/thread_observation.hpp"
#include <utility>

namespace fog::stream {
namespace {
[[noreturn]] void translated(const VideoDecodeError& error) {
    throw PlayerVideoError(error.code(), error.recoverable(), error.what());
}
class SdlPlayerMedia final : public PlayerMedia {
public:
    SdlPlayerMedia(SDL_Renderer* renderer, std::function<void(const std::string&)> watchdog,
                   std::function<void(const ::fog::media::telemetry::StallEvent&)> typedWatchdog)
        : media_(renderer, ::stream::VideoCodec::H264, true) {
        if(typedWatchdog)media_.setVideoStallObserver(std::move(typedWatchdog));
        else media_.setVideoStallReporter(std::move(watchdog));
    }
    void setVideoCodec(::stream::VideoCodec value) override {
        try { media_.setVideoCodec(value); } catch (const VideoDecodeError& error) { translated(error); }
    }
    void setPresentationFrameRate(int fps) override { media_.setPresentationFrameRate(fps); }
    void startWorkers(::stream::CompressedMediaSource& source, int fps, WorkerPolicy policy) override {
        media_.startWorkers(source, fps, policy == WorkerPolicy::Mts ? Media::WorkerPolicy::Mts : Media::WorkerPolicy::FogHub);
    }
    void stopWorkers() noexcept override { media_.stopWorkers(); }
    bool workersRunning() const override { return media_.workersRunning(); }
    bool takeWorkerError(WorkerError& value) override {
        Media::WorkerError error;
        if (!media_.takeWorkerError(error)) return false;
        value = {error.error, std::move(error.packet), error.hasPacket};
        try { if (value.error) std::rethrow_exception(value.error); }
        catch (const VideoDecodeError& failure) {
            value.error = std::make_exception_ptr(PlayerVideoError(failure.code(),failure.recoverable(),failure.what()));
        } catch (...) {} // Audio/unexpected failures retain their original type.
        return true;
    }
    std::uint64_t workerVideoGeneration() const override { return media_.workerVideoGeneration(); }
    bool waitingForVideoKeyframe() const override { return media_.waitingForVideoKeyframe(); }
    std::uint64_t presentedFrames() const override { return media_.presentedFrames(); }
    void resetVideoDecoder() override {
        try { media_.resetVideoDecoder(); } catch (const VideoDecodeError& error) { translated(error); }
    }
    void video(const ::stream::Packet& packet) override {
        try { media_.video(packet.data.data(),packet.data.size(),packet.firstPacketMs,packet.enqueueMs); }
        catch (const VideoDecodeError& error) { translated(error); }
    }
    void audio(const ::stream::Packet& packet) override { media_.audio(packet.data.data(),packet.data.size(),packet.enqueueMs); }
    void checkAudioRecovery() const override { media_.checkAudioRecovery(); }
    bool prepare(std::chrono::steady_clock::time_point slot) override {
        try { return media_.preparePresentation(slot); } catch (const VideoDecodeError& error) { translated(error); }
    }
    Progress progress() const override {
        const auto load = media_.workerVideoLoad();
        return {media_.videoFrames(),media_.uploadedFrames(),media_.presentedFrames(),media_.supersededFrames(),media_.audioPackets(),
            media_.audioUnderruns(),media_.audioQueueResets(),load.decodeUs,load.convertUs,load.generation,
            media_.videoWidth(),media_.videoHeight(),media_.hardwareVideo(),media_.waitingForVideoKeyframe(),media_.workersRunning()};
    }
    ::fog::media::telemetry::Snapshot captureTelemetry() const noexcept override {
        try { return media_.captureTelemetry(); } catch (...) { return {}; }
    }
    bool draw() override { return media_.draw(); }
    void present() override { media_.present(); }
    std::string videoDiagnostics() const override { return media_.diagnostics(); }
    std::string audioDiagnostics() const override { return media_.audioDiagnostics(); }
    std::string imageDiagnostics() const override { return media_.imageDiagnostics(); }
    std::string latencyDiagnostics() const override { return media_.latencyDiagnostics(); }
private:
    Media media_;
};
}
SdlPlayerMediaFactory::SdlPlayerMediaFactory(SDL_Renderer* renderer, std::function<void(const std::string&)> watchdog)
    : renderer_(renderer), watchdog_(std::move(watchdog)) {}
SdlPlayerMediaFactory::SdlPlayerMediaFactory(SDL_Renderer* renderer,std::function<void(const ::fog::media::telemetry::StallEvent&)> observer)
    :renderer_(renderer),typedWatchdog_(std::move(observer)) {}
std::unique_ptr<PlayerMedia> SdlPlayerMediaFactory::create() {
    fog::diagnostics::beginThreadObservationEpoch();
    fog::diagnostics::observeThread("application");
    return std::make_unique<SdlPlayerMedia>(renderer_,watchdog_,typedWatchdog_);
}
bool SdlPlayerMediaFactory::supportsH265() const { return Media::supportsH265(); }
}
