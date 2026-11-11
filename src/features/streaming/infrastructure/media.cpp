#include "core/constants/error_text.hpp"
#include "core/constants/stage1_options.hpp"
#include "features/streaming/infrastructure/media.hpp"
#include "features/streaming/application/media_diagnostics_formatter.hpp"
#include "features/streaming/infrastructure/media_telemetry_cache.hpp"
#include "features/streaming/domain/media_annexb.hpp"
#include "features/streaming/domain/video_limits.hpp"
#include "features/streaming/domain/audio_queue_metrics.hpp"
#include "features/streaming/domain/audio_latency_policy.hpp"
#include "features/streaming/domain/audio_recovery.hpp"
#include "features/streaming/domain/frame_cadence.hpp"
#include "features/streaming/infrastructure/media_decoder_diagnostics.hpp"
#include "features/streaming/infrastructure/media_frame_queue.hpp"
#include "features/streaming/infrastructure/media_experiments.hpp"
#include "features/streaming/infrastructure/video_image_diagnostics.hpp"
#include "features/streaming/infrastructure/video_watchdog.hpp"
#include "core/diagnostics/thread_observation.hpp"
#include "core/diagnostics/stage_distribution.hpp"
#include <SDL.h>
#include <opus/opus.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}
#include <array>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <vector>
#include <chrono>
#include <mutex>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <deque>
#include <thread>

namespace fog {
namespace mt = media::telemetry;
namespace {
mt::FormatNames mediaFormatNames(false);
std::once_flag mediaFormatNamesOnce;
std::atomic<std::uint64_t> mediaDiagnosticInstances{0};
void initializeMediaFormatNames() {
    static_assert(static_cast<std::size_t>(AV_PIX_FMT_NB) <= mt::FormatNames::PixelCapacity);
    std::call_once(mediaFormatNamesOnce, [] {
        for (int format = 0; format < AV_PIX_FMT_NB; ++format)
            if (const auto* name = av_get_pix_fmt_name(static_cast<AVPixelFormat>(format)))
                mediaFormatNames.setPixel(static_cast<std::size_t>(format),name);
        mediaFormatNames.publish();
    });
}
std::uint64_t telemetryTimeUs(std::chrono::steady_clock::time_point value = std::chrono::steady_clock::now()) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(value.time_since_epoch()).count());
}
detail::DecoderDiagnosticCapture hardwareDiagnostics;
std::mutex callbackLifecycleMutex;
unsigned callbackUsers = 0;
void decoderLogCallback(void* context, int, const char* format, va_list args) {
    // No formatting of arbitrary FFmpeg messages, payloads, URLs or I/O.
    hardwareDiagnostics.capture(context, format, args);
}
void acquireDecoderLogging() {
    std::lock_guard<std::mutex> lock(callbackLifecycleMutex);
    if (callbackUsers++ == 0) av_log_set_callback(decoderLogCallback);
}
void releaseDecoderLogging() {
    std::lock_guard<std::mutex> lock(callbackLifecycleMutex);
    if (--callbackUsers == 0) av_log_set_callback(av_log_default_callback);
}
using DecoderClock = std::chrono::steady_clock;
struct StageTiming {
    std::atomic<std::uint64_t> calls{0}, total{0}, maximum{0};
    void record(DecoderClock::time_point start) {
        const auto us = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            DecoderClock::now() - start).count());
        recordValue(us);
    }
    void recordValue(std::uint64_t us) {
        ++calls; total += us;
        auto previous = maximum.load();
        while (previous < us && !maximum.compare_exchange_weak(previous, us)) {}
    }
    std::uint64_t average() const { const auto count = calls.load(); return count ? total.load() / count : 0; }
    mt::Timing snapshot() const { return {calls.load(),total.load(),maximum.load()}; }
};
mt::StageDistributionValue distributionValue(const metrics::StageDistributionSnapshot& source) noexcept {
    auto availability=mt::StageAvailability::NotCollected;
    switch(source.availability) {
        case metrics::DistributionAvailability::NotCollected:break;
        case metrics::DistributionAvailability::InstrumentationDisabled:availability=mt::StageAvailability::InstrumentationDisabled;break;
        case metrics::DistributionAvailability::Available:availability=mt::StageAvailability::Available;break;
    }
    return {availability,source.capturedAtSteadyUs,source.samples,source.p50Us,source.p95Us,source.p99Us,source.overflowLifetime};
}
// Each instance has one serialized owner: decoder/UI sync or UI upload.
// Worker readers receive only its published scalar distribution snapshot.
struct ProfileStageTiming : StageTiming {
    metrics::StageDistribution<> distribution;
    void record(DecoderClock::time_point start) {
        const auto end = DecoderClock::now();
        const auto elapsed = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
        recordValue(elapsed);
        if constexpr (metrics::collects(metrics::DiagnosticClass::StageDistribution))
            distribution.record(elapsed, telemetryTimeUs(end));
    }
};
struct WorkScope {
    StageTiming& timing;
    DecoderClock::time_point start = DecoderClock::now();
    ~WorkScope() { timing.record(start); }
};
struct FrameCompletionScope {
    std::shared_ptr<::stream::VideoFrameCompletion> completion;
    bool decoded = false;
    ~FrameCompletionScope() { if (completion) completion->complete(decoded); }
};
}
const mt::FormatNames& media::telemetry::processFormatNames() noexcept { return mediaFormatNames; }
struct Media::Impl {
    SDL_Renderer* renderer;
    detail::VideoWatchdog watchdog;
    using Flight = detail::VideoFlight;
    Flight::Metadata decoderMetadata() const {
        return {width.load(), height.load(), static_cast<int>(videoCodec.load()), lastFrameFormat,
            pendingFrame ? pendingFrame->linesize[0] : 0, packetSize, packetNalTypes, frames.load()};
    }
    Flight::Metadata presentationMetadata() const {
        return {textureWidth.load(), textureHeight.load(), static_cast<int>(videoCodec.load()),
            textureFormat == SDL_PIXELFORMAT_NV12 ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P,
            textureWidth.load(), 0, 0, uploadedFrames.load()};
    }
    SDL_Texture* texture = nullptr;
    SDL_Texture* spareTexture = nullptr;
    int spareWidth = 0, spareHeight = 0;
    Uint32 textureFormat = SDL_PIXELFORMAT_IYUV, spareFormat = SDL_PIXELFORMAT_IYUV;
    bool nv12UploadAvailable = FOGPLAY_MEDIA_NV12 != 0 && SDL_VERSION_ATLEAST(2, 0, 16);
    std::uint64_t nv12Fallbacks = 0;
    std::vector<unsigned char> fallbackPixels;
    std::shared_ptr<detail::MediaCpuBufferPool> cpuPool = FOGPLAY_MEDIA_CPU_POOL
        ? std::make_shared<detail::MediaCpuBufferPool>() : nullptr;
    SDL_AudioDeviceID audioDevice = 0;
    AVCodecContext* decoder = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* cpuFrame = nullptr;
    AVFrame* pendingFrame = nullptr;
    AVPacket* packet = nullptr;
    SwsContext* scaler = nullptr;
    OpusDecoder* opus = nullptr;
    // Reused by synchronous callers; worker outputs transfer ownership to UI.
    std::vector<unsigned char> pixels;
    std::atomic<int> width{0}, height{0};
    std::atomic<int> textureWidth{0}, textureHeight{0};
    bool deferredPresentation = false, pending = false;
    FrameCadence presentationCadence;
    DecoderClock::time_point pendingSince{};
    std::atomic<std::uint64_t> uploadedFrames{0}, supersededFrames{0};
    std::atomic<std::uint64_t> presentedFrames{0};
    std::uint64_t drawnGeneration = 0, presentedGeneration = 0;
    detail::VideoFrameOrigins frameOrigins;
    detail::VideoFrameOrigin pendingOrigin, uploadedOrigin;
    detail::VideoLatencyMetrics latency;
    std::int64_t nextFrameToken = 0;
    std::atomic<std::uint64_t> originMisses{0};
    detail::VideoImageDiagnostics image;
    detail::MediaPixelFormat imagePixelFormat = detail::MediaPixelFormat::Iyuv;
    StageTiming imageTiming;
    DecoderClock::time_point imageUploadedAt{}, imageDrawnAt{}, imagePresentedAt{};
    std::uint64_t imageDraws = 0, imageEmptyDraws = 0;
    std::uint64_t imageCreateErrors = 0, imageUploadErrors = 0, imageClearErrors = 0, imageCopyErrors = 0;
    std::uint64_t imagePresentedBlackFrames = 0, imageLastDrawnGeneration = 0;
    bool imagePresentedBlack = false;
    std::atomic<std::uint64_t> frames{0}, packets{0};
    detail::AudioQueueMetrics audioMetrics;
    std::uint64_t audioDiscardedPackets = 0, audioDiscardedFrames = 0;
    std::uint64_t audioDecodedFrames = 0, audioAdmittedFrames = 0, audioFailedFrames = 0;
    std::uint64_t audioFirstPcmAtUs = 0, audioLastPcmAtUs = 0;
    std::uint64_t audioLatencyClears = 0, audioRecoveryClears = 0;
    std::uint64_t audioClearBeforeBytes = 0, audioClearAfterBytes = 0, audioClearAtUs = 0;
    mt::AudioClearReason audioLastClearReason = mt::AudioClearReason::None;
    std::uint32_t audioOutputRate = 0, audioOutputChannels = 0, audioOutputBytesPerFrame = 0;
    std::uint64_t audioStalePackets = 0, audioOverflowPackets = 0, audioCatchups = 0;
    bool audioCatchingUp = false;
    // Experimental opt-in only until external A/V comparison is recorded.
#ifdef FOGPLAY_BOUNDED_SYNC_AUDIO
    bool boundedSyncAudio = true;
#else
    bool boundedSyncAudio = false;
#endif
    AudioRecovery audioRecovery;
    bool audioResetNeeded = false, audioOutputResetNeeded = false;
    std::uint64_t audioBadPackets = 0, audioDecoderErrors = 0, audioOutputErrors = 0;
    std::uint64_t audioRecoveryAttempts = 0, audioRecoveryFailures = 0, audioRecoveryDiscardedPackets = 0;
    std::uint64_t audioResetAtMs = 0;
    std::atomic<bool> hw{false};
    bool ownsCallback = false;
    std::atomic<bool> waiting{false};
    bool awaitingIdr = false;
    std::size_t packetSize = 0;
    std::uint64_t packetNalTypes = 0;
    std::atomic<::stream::VideoCodec> videoCodec;
    detail::VideoParameters parameters;
    ProfileStageTiming sendTiming, receiveTiming, transferTiming, scaleTiming, uploadTiming;
    StageTiming videoQueueWait, audioQueueWait;
    StageTiming workerDecodeTiming, workerConvertTiming;
    std::atomic<std::uint64_t> loadGeneration{0};
    std::uint64_t hardwareEventsBeforePacket = 0;
    int lastFrameFormat = AV_PIX_FMT_NONE, lastCpuFormat = AV_PIX_FMT_NONE;
    int lastColorspace = AVCOL_SPC_UNSPECIFIED, lastRange = AVCOL_RANGE_UNSPECIFIED;
    int lastTrc = AVCOL_TRC_UNSPECIFIED, lastPrimaries = AVCOL_PRI_UNSPECIFIED;
    const std::thread::id applicationThread = std::this_thread::get_id();
    std::atomic<bool> workers{false}, stopping{false};
    std::atomic<bool> resetRequested{false}, codecRequested{false};
    std::atomic<bool> resetInProgress{false};
    std::atomic<::stream::VideoCodec> requestedCodec{::stream::VideoCodec::H264};
    std::atomic<std::uint64_t> epoch{0}, transportGeneration{0};
    std::atomic<std::uint64_t> underruns{0}, audioResets{0};
    std::thread videoWorker, audioWorker;
    std::mutex readyMutex, errorMutex;
    std::shared_ptr<::stream::MediaSignals> signals;
    ::stream::CompressedMediaSource* workerClient = nullptr; // Joined before client teardown.
    bool boundedWorkerAudio = true;
    detail::MediaFrameQueue readyFrames{FOGPLAY_MEDIA_READY_DEPTH == 1
        ? detail::MediaFrameQueue::Mode::Latest : detail::MediaFrameQueue::Mode::Smooth};
    std::deque<Media::WorkerError> errors;
    detail::MediaTelemetryCache diagnosticCache;
    std::uint64_t diagnosticInstance=0;
    std::uint64_t videoDiagnosticRevision=0,audioDiagnosticRevision=0,diagnosticEpoch=0;
    std::uint64_t diagnosticLoadGeneration=0,diagnosticWorkerRun=0,diagnosticSourceGeneration=0;
    std::atomic<std::uint64_t> workerRun{0};
    bool diagnosticSourceGenerationKnown=false;
    std::uint32_t observedAudioBytes=0;
    std::uint64_t audioObservedAtUs=0;
    int workerFps = 30;
    // Switch's display is 60 Hz. Enable only for its active GLES VSYNC path;
    // software/unpaced renderers and 30 FPS retain the timed queue policy.
    bool displayPaced = false, refreshPrepared = false;
    static void recordQueueWait(const ::stream::Packet& incoming, StageTiming& timing) {
        const auto now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            DecoderClock::now().time_since_epoch()).count());
        if (incoming.enqueueMs && incoming.enqueueMs <= now)
            timing.recordValue((now - incoming.enqueueMs) * 1000);
    }
    void requireApplicationThread() const {
        if (std::this_thread::get_id() != applicationThread)
            throw std::logic_error("Media presentation/lifecycle requires application thread");
    }
    DecoderClock::time_point nextVideoSnapshot{}, nextAudioSnapshot{};
    mt::VideoOwner videoOwnerSnapshot(mt::Availability availability = mt::Availability::Available) const {
        mt::VideoOwner value;
        value.source.capturedAtUs=telemetryTimeUs();
        value.source.revision=videoDiagnosticRevision; value.source.epoch=diagnosticEpoch;
        value.source.epochKnown=true;
        value.source.sourceGeneration=diagnosticSourceGeneration; value.source.loadGeneration=diagnosticLoadGeneration;
        value.source.workerRun=diagnosticWorkerRun; value.source.availability=availability;
        value.source.sourceGenerationKnown=diagnosticSourceGenerationKnown;
        value.source.loadGenerationKnown=diagnosticWorkerRun!=0;
        value.workerOwned=workers.load();
        value.codec=decoder&&decoder->codec ? (videoCodec==::stream::VideoCodec::H265?mt::Codec::Hevc:mt::Codec::H264) : mt::Codec::None;
        value.codecId=decoder?decoder->codec_id:0;
        value.pixelFormat=decoder?decoder->pix_fmt:AV_PIX_FMT_NONE;
        value.framePixelFormat=lastFrameFormat;value.cpuPixelFormat=lastCpuFormat;
        value.colorspace=lastColorspace;value.range=lastRange;value.trc=lastTrc;value.primaries=lastPrimaries;
        value.threads=decoder?decoder->thread_count:0;value.threadRequested=decoder?decoder->thread_type:0;
        value.threadActive=decoder?decoder->active_thread_type:0;
        value.stages={sendTiming.snapshot(),receiveTiming.snapshot(),transferTiming.snapshot(),scaleTiming.snapshot()};
        value.distributions={distributionValue(sendTiming.distribution.snapshot(value.source.capturedAtUs)), distributionValue(receiveTiming.distribution.snapshot(value.source.capturedAtUs)),
            distributionValue(transferTiming.distribution.snapshot(value.source.capturedAtUs)), distributionValue(scaleTiming.distribution.snapshot(value.source.capturedAtUs))};
        const auto hardware=hardwareDiagnostics.snapshot(decoder);
        value.hardware={hardware.events,hardware.error,hardware.badBlocks,hardware.goodBlocks,hardware.cycles,hardware.headerError,
            hardware.waitCode,hardware.hasStatus,hardware.events>hardwareEventsBeforePacket};
        value.pending=pending;
        value.awaitingIdr=awaitingIdr;
        value.pendingSinceUs=pending?telemetryTimeUs(pendingSince):0;
        value.pendingAgeUs=pending?telemetryTimeUs()-value.pendingSinceUs:0;
        value.source.publishedAtUs=telemetryTimeUs();
        return value;
    }
    mt::AudioOwner audioOwnerSnapshot(mt::Availability availability = mt::Availability::Available) const {
        mt::AudioOwner value;
        value.source.capturedAtUs=telemetryTimeUs();value.source.revision=audioDiagnosticRevision;
        value.source.workerRun=workerRun.load();value.source.availability=availability;
        value.packets=packets.load();value.queuedBytes=observedAudioBytes;value.maximumBytes=audioMetrics.maximumBytes;
        value.emptyObservations=audioMetrics.emptyObservations;value.prolongedEmptyObservations=audioMetrics.prolongedEmptyObservations;
        value.resets=audioMetrics.resets;value.discardedBytes=audioMetrics.discardedBytes;
        value.queueWait=audioQueueWait.snapshot();
        value.pcmDiscardedPackets=audioDiscardedPackets;value.pcmDiscardedFrames=audioDiscardedFrames;
        value.stalePackets=audioStalePackets;value.overflowPackets=audioOverflowPackets;value.catchups=audioCatchups;
        value.badPackets=audioBadPackets;value.decoderErrors=audioDecoderErrors;value.outputErrors=audioOutputErrors;
        value.recoveryAttempts=audioRecoveryAttempts;value.recoveryFailures=audioRecoveryFailures;
        value.recoveryDiscardedPackets=audioRecoveryDiscardedPackets;value.recoveryPending=audioResetNeeded;
        value.boundedSync=boundedSyncAudio;value.lastFloorBytes=audioMetrics.lastFloorBytes;
        value.maximumFloorBytes=audioMetrics.maximumFloorBytes;value.floorWindows=audioMetrics.floorWindows;
        value.elevatedFloorWindows=audioMetrics.elevatedFloorWindows;
        value.decodedPcmFrames=audioDecodedFrames;value.admittedPcmFrames=audioAdmittedFrames;value.failedPcmFrames=audioFailedFrames;
        value.firstPcmAtUs=audioFirstPcmAtUs;value.lastPcmAtUs=audioLastPcmAtUs;
        value.latencyClears=audioLatencyClears;value.recoveryClears=audioRecoveryClears;
        value.clearBeforeBytes=audioClearBeforeBytes;value.clearAfterBytes=audioClearAfterBytes;value.clearAtUs=audioClearAtUs;
        value.lastClearReason=audioLastClearReason;value.outputSampleRate=audioOutputRate;
        value.outputChannels=audioOutputChannels;value.outputBytesPerFrame=audioOutputBytesPerFrame;
        value.boundedWorker=workers.load() && boundedWorkerAudio;
        value.queueObservedAtUs=audioObservedAtUs;value.queueKnown=audioObservedAtUs!=0;
        value.source.publishedAtUs=telemetryTimeUs();
        return value;
    }
    void publishVideo(bool force = false,mt::Availability availability = mt::Availability::Available) {
        const auto now = DecoderClock::now();
        if (fog::stage1::sparseDiagnostics && !force && now < nextVideoSnapshot) return;
        nextVideoSnapshot = now + std::chrono::seconds(1);
        ++videoDiagnosticRevision;
        diagnosticCache.publish(videoOwnerSnapshot(availability));
    }
    void publishAudio(bool force = false,bool observe = true,mt::Availability availability = mt::Availability::Available) {
        const auto now = DecoderClock::now();
        if (fog::stage1::sparseDiagnostics && !force && now < nextAudioSnapshot) return;
        nextAudioSnapshot = now + std::chrono::seconds(1);
        if(observe)observeAudioQueue();
        ++audioDiagnosticRevision;
        diagnosticCache.publish(audioOwnerSnapshot(availability));
        underruns = audioMetrics.prolongedEmptyObservations;
        audioResets = audioMetrics.resets;
    }
    void syncDiagnosticOperation() {
        diagnosticEpoch=epoch.load();diagnosticLoadGeneration=diagnosticWorkerRun=diagnosticSourceGeneration=0;
        diagnosticSourceGenerationKnown=false;
    }
    void discardReady() {
        std::lock_guard<std::mutex> lock(readyMutex);
        supersededFrames += readyFrames.size();
        readyFrames.reset(workerFps, displayPaced);
    }
    void requestReset() {
        ++epoch;
        waiting = true;
        resetRequested = true;
        discardReady();
        if (workerClient) workerClient->wakeMediaWaiters();
    }
    void reportError(const ::stream::Packet* failed) noexcept {
        try {
            Media::WorkerError event;
            event.error = std::current_exception();
            try { if (event.error) std::rethrow_exception(event.error); }
            catch (const VideoDecodeError& error) { event.category=mt::ErrorCategory::VideoDecode; event.code=error.code(); event.recoverable=error.recoverable(); }
            catch (const AudioPlaybackError&) { event.category=mt::ErrorCategory::AudioPlayback; }
            catch (const std::bad_alloc&) { event.category=mt::ErrorCategory::Allocation; }
            catch (const std::logic_error&) { event.category=mt::ErrorCategory::Logic; }
            catch (...) {}
            if (failed) { event.packet = *failed; event.hasPacket = true; }
            std::lock_guard<std::mutex> lock(errorMutex);
            errors.push_back(std::move(event));
        } catch (...) {
            // No allocations are needed to retain the exception itself.
            std::lock_guard<std::mutex> lock(errorMutex);
            emergencyError = std::current_exception();
        }
    }
    std::exception_ptr emergencyError;
    void decodeVideo(const std::byte* data, std::size_t size,
        std::uint64_t firstPacketMs = 0, std::uint64_t enqueueMs = 0);
    void decodeAudio(const std::byte* data, std::size_t size, std::uint64_t enqueueMs = 0);
    bool recoverAudio(DecoderClock::time_point now);
    void clearAudioQueue(mt::AudioClearReason reason);
    void resetVideo();
    void changeCodec(::stream::VideoCodec codec);
    void runVideo(::stream::CompressedMediaSource& client) noexcept;
    void runAudio(::stream::CompressedMediaSource& client) noexcept;
    explicit Impl(SDL_Renderer* value, ::stream::VideoCodec codec = ::stream::VideoCodec::H264,
        bool deferred = false)
        : renderer(value), deferredPresentation(deferred), videoCodec(codec), parameters(codec) {}
    void releaseVideo() {
        Flight::Scope flight(watchdog.decoder, Flight::Stage::DecoderRelease, decoderMetadata());
        // Pending hardware surfaces must outlive neither decoder nor device.
        av_frame_free(&pendingFrame);
        pending = false;
        frameOrigins.clear();
        pendingOrigin = {};
        sws_freeContext(scaler);
        scaler = nullptr;
        av_packet_free(&packet);
        av_frame_free(&cpuFrame);
        av_frame_free(&frame);
        hardwareDiagnostics.unregisterContext(decoder);
        avcodec_free_context(&decoder);
        hw = false;
    }
    ~Impl() {
        if (audioDevice) SDL_CloseAudioDevice(audioDevice);
        if (texture) {
            Flight::Scope scope(watchdog.presentation, Flight::Stage::TextureRelease, presentationMetadata());
            SDL_DestroyTexture(texture);
        }
        if (spareTexture) SDL_DestroyTexture(spareTexture);
        if (opus) opus_decoder_destroy(opus);
        releaseVideo();
        if (ownsCallback) releaseDecoderLogging();
    }
    static AVPixelFormat format(AVCodecContext* context, const AVPixelFormat* formats) {
        (void)context;
#ifdef __SWITCH__
        for (auto p = formats; *p != AV_PIX_FMT_NONE; ++p)
            if (*p == AV_PIX_FMT_NVTEGRA) return *p;
        // HEVC is experimental and enabled on Switch only with NVTEGRA.
        // FFmpeg may retry get_format after a HW failure: never quietly turn
        // a high-bitrate HEVC stream into software decoding in that retry.
        if (context->codec_id == AV_CODEC_ID_HEVC) return AV_PIX_FMT_NONE;
#endif
        for (auto p = formats; *p != AV_PIX_FMT_NONE; ++p)
            if (*p == AV_PIX_FMT_YUV420P) return *p;
        return formats[0];
    }
    [[noreturn]] void codecFailure(const char* operation, int code, bool allowRecovery = true) const {
        std::array<char, AV_ERROR_MAX_STRING_SIZE> error{};
        av_strerror(code, error.data(), error.size());
        std::array<char, 2048> message{};
        const auto details = diagnostics();
        // Numeric bit mask: bit N represents NAL type N of the selected codec. No payload.
        std::snprintf(message.data(), message.size(),
            "%s %s failed: code=%d (%s), packet_bytes=%zu nal_types=0x%08llx width=%d height=%d profile=%d hw=%d waiting_idr=%d %s",
            ::stream::videoCodecName(videoCodec), operation, code, error.data(), packetSize, static_cast<unsigned long long>(packetNalTypes),
            decoder ? decoder->width : width.load(), decoder ? decoder->height : height.load(),
            decoder ? decoder->profile : AV_PROFILE_UNKNOWN, hw ? 1 : 0, awaitingIdr ? 1 : 0, details.c_str());
        const bool recoverable = allowRecovery && code != AVERROR(ENOMEM) &&
            code != AVERROR(ENOSYS) &&
            code != AVERROR_DECODER_NOT_FOUND;
        throw VideoDecodeError(code, recoverable, message.data());
    }
    std::string diagnostics() const {
        mt::Snapshot value;value.captured=mt::Capture::Video;value.capturedAtUs=telemetryTimeUs();
        value.video=videoOwnerSnapshot();
        value.presentation.decoded=frames.load();value.presentation.uploaded=uploadedFrames.load();
        value.presentation.presented=presentedFrames.load();value.presentation.superseded=supersededFrames.load();
        value.presentation.upload=uploadTiming.snapshot();value.presentation.workers=workers.load();
        return mt::formatDecoder(value,mediaFormatNames);
    }
    std::uint32_t observeAudioQueue() {
        const auto bytes = audioDevice ? SDL_GetQueuedAudioSize(audioDevice) : 0;
        const auto now=DecoderClock::now();
        audioMetrics.observe(bytes,now);observedAudioBytes=bytes;audioObservedAtUs=telemetryTimeUs(now);
        return bytes;
    }
    int sendPacket() {
        const auto start = DecoderClock::now();
        Flight::Scope flight(watchdog.decoder, Flight::Stage::DecodeSend, decoderMetadata());
        const int rc = avcodec_send_packet(decoder, packet);
        sendTiming.record(start);
        return rc;
    }
    void initVideo(bool requireHardware = false) {
        Flight::Scope flight(watchdog.decoder, Flight::Stage::DecoderInit, decoderMetadata());
        // The temporary owner also cleans up partially initialized resources.
#ifndef __SWITCH__
        (void)requireHardware;
#endif
        Impl candidate(renderer, videoCodec);
        const auto* codec = avcodec_find_decoder(videoCodec == ::stream::VideoCodec::H265 ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264);
        if (!codec) throw std::runtime_error(fog::constants::error::Stream::SelectedVideoDecoderUnavailable);
        candidate.decoder = avcodec_alloc_context3(codec);
        candidate.frame = av_frame_alloc(); candidate.cpuFrame = av_frame_alloc(); candidate.packet = av_packet_alloc();
        candidate.pendingFrame = av_frame_alloc();
        if (!candidate.decoder || !candidate.frame || !candidate.cpuFrame || !candidate.packet || !candidate.pendingFrame)
            codecFailure("allocation", AVERROR(ENOMEM), false);
        hardwareDiagnostics.registerContext(candidate.decoder);
        candidate.decoder->thread_count = 2;
        candidate.decoder->thread_type = FF_THREAD_SLICE;
        candidate.decoder->flags |= AV_CODEC_FLAG_LOW_DELAY;
        candidate.decoder->max_pixels = 1920 * 1080;
#ifdef __SWITCH__
        const auto device = av_hwdevice_find_type_by_name("nvtegra");
        if (device != AV_HWDEVICE_TYPE_NONE) {
            const int rc = av_hwdevice_ctx_create(&candidate.decoder->hw_device_ctx, device, nullptr, nullptr, 0);
            if (rc < 0 && requireHardware) candidate.codecFailure("hardware device creation", rc, false);
            if (rc >= 0) {
                candidate.decoder->get_format = &format;
                candidate.hw = true;
            }
        }
        if (requireHardware && !candidate.hw) candidate.codecFailure("hardware device unavailable", AVERROR(ENOSYS), false);
#endif
        const int rc = avcodec_open2(candidate.decoder, codec, nullptr);
        if (rc < 0) candidate.codecFailure("open", rc, false);
        std::swap(decoder, candidate.decoder);
        std::swap(frame, candidate.frame);
        std::swap(cpuFrame, candidate.cpuFrame);
        std::swap(pendingFrame, candidate.pendingFrame);
        std::swap(packet, candidate.packet);
        std::swap(scaler, candidate.scaler);
        const bool previousHardware = hw.load(); hw = candidate.hw.load(); candidate.hw = previousHardware;
    }
    void init() {
        initVideo(videoCodec == ::stream::VideoCodec::H265);
        int error = 0;
        opus = opus_decoder_create(48000, 2, &error);
        if (!opus || error != OPUS_OK) throw std::runtime_error(fog::constants::error::Stream::CannotInitializeOpusDecoder);
        openAudioOutput();
    }
    void openAudioOutput() {
        SDL_AudioSpec wanted{}, obtained{};
        wanted.freq = 48000; wanted.format = AUDIO_S16SYS; wanted.channels = 2; wanted.samples = 960;
        audioDevice = SDL_OpenAudioDevice(nullptr, 0, &wanted, &obtained, 0);
        if (!audioDevice) throw std::runtime_error(fog::constants::error::Stream::CannotOpenAudioOutput);
        audioOutputRate = static_cast<std::uint32_t>(obtained.freq);
        audioOutputChannels = obtained.channels;
        audioOutputBytesPerFrame = obtained.channels * SDL_AUDIO_BITSIZE(obtained.format) / 8;
        SDL_PauseAudioDevice(audioDevice, 0);
    }
    void receive() {
        while (true) {
            const auto receiveStart = DecoderClock::now();
            Flight::Scope receiveFlight(watchdog.decoder, Flight::Stage::DecodeReceive, decoderMetadata());
            const int rc = avcodec_receive_frame(decoder, frame);
            receiveFlight.finish();
            receiveTiming.record(receiveStart);
            if (rc == AVERROR(EAGAIN)) return;
            // This client never sends the drain marker; EOF is unexpected.
            if (rc < 0) codecFailure("receive_frame", rc);
            if (frame->width < 1 || frame->height < 1 || frame->width > 1920 || frame->height > 1080)
                throw std::runtime_error(fog::constants::error::Stream::VideoExceedsPrototypeLimit1920x1080);
            lastFrameFormat = frame->format;
            lastColorspace = frame->colorspace; lastRange = frame->color_range;
            lastTrc = frame->color_trc; lastPrimaries = frame->color_primaries;
            hw = frame->hw_frames_ctx != nullptr;
            width = frame->width; height = frame->height;
            if (pending) ++supersededFrames;
            Flight::Scope releaseFlight(watchdog.decoder, Flight::Stage::FrameRelease, decoderMetadata());
            av_frame_unref(pendingFrame);
            av_frame_move_ref(pendingFrame, frame);
            releaseFlight.finish();
            pending = true;
            pendingSince = DecoderClock::now();
            bool found = false;
            pendingOrigin = frameOrigins.find(pendingFrame->pts, found);
            if (!found) ++originMisses;
            ++frames;
            waiting = false;
        }
    }
    void requireVideo() const {
        if (!decoder || !packet || !frame || !cpuFrame || !pendingFrame)
            codecFailure("decoder unavailable", AVERROR(EINVAL), false);
    }
    detail::MediaFrameQueue::Frame convertPending() {
        requireVideo();
        detail::MediaFrameQueue::Frame output;
        output.origin = pendingOrigin;
        if (workers && cpuPool) {
            output.pool = cpuPool;
            output.pixels = cpuPool->acquire();
        } else output.pixels.swap(pixels);
        AVFrame* decoded = pendingFrame;
        if (pendingFrame->hw_frames_ctx) {
            Flight::Scope releaseFlight(watchdog.decoder, Flight::Stage::FrameRelease, decoderMetadata());
            av_frame_unref(cpuFrame);
            releaseFlight.finish();
            const auto transferStart = DecoderClock::now();
            Flight::Scope transferFlight(watchdog.decoder, Flight::Stage::Transfer, decoderMetadata());
            const int transfer = av_hwframe_transfer_data(cpuFrame, pendingFrame, 0);
            transferFlight.finish();
            transferTiming.record(transferStart);
            if (transfer < 0) codecFailure("hardware frame transfer", transfer);
            decoded = cpuFrame;
        }
        lastCpuFormat = decoded->format;
        const int w = decoded->width, h = decoded->height;
        if (w < 1 || h < 1 || w > 1920 || h > 1080)
            throw std::runtime_error(fog::constants::error::Stream::VideoExceedsPrototypeLimit1920x1080);
        const auto layout = detail::MediaPixelLayout::make(w, h);
        // Prevent vector growth doubling from retaining an oversized pooled
        // allocation after a resolution increase. Each pooled frame stays at
        // most one prototype-sized image; only two released spares are cached.
        if (output.pool && output.pixels.capacity() < layout.size)
            std::vector<unsigned char>(layout.size).swap(output.pixels);
        else output.pixels.resize(layout.size);
        output.width = w; output.height = h;
#if FOGPLAY_MEDIA_NV12
        if (decoded->format == AV_PIX_FMT_NV12) {
            // CPU storage owns both planes before the decoder/transfer frame is
            // released. Never enqueue AVFrame pointers or hardware surfaces.
            detail::copyMediaPlane(output.pixels.data(), w, decoded->data[0], decoded->linesize[0], w, h);
            detail::copyMediaPlane(output.pixels.data() + layout.ySize, 2 * layout.chromaWidth,
                decoded->data[1], decoded->linesize[1], 2 * layout.chromaWidth, layout.chromaHeight);
            output.format = detail::MediaPixelFormat::Nv12;
            return output;
        }
#endif
        scaler = sws_getCachedContext(scaler, w, h, static_cast<AVPixelFormat>(decoded->format),
            w, h, AV_PIX_FMT_YUV420P, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        if (!scaler) throw std::runtime_error(fog::constants::error::Stream::CannotConvertVideoPixelFormat);
        unsigned char* planes[4]{output.pixels.data(), output.pixels.data() + w*h,
            output.pixels.data() + w*h + ((w+1)/2)*((h+1)/2), nullptr};
        int strides[4]{w, (w+1)/2, (w+1)/2, 0};
        const auto scaleStart = DecoderClock::now();
        Flight::Scope scaleFlight(watchdog.decoder, Flight::Stage::Convert, decoderMetadata());
        const int scaled = sws_scale(scaler, decoded->data, decoded->linesize, 0, h, planes, strides);
        scaleFlight.finish();
        scaleTiming.record(scaleStart);
        if (scaled != h) throw std::runtime_error(fog::constants::error::Stream::VideoConversionFailed);
        return output;
    }
    void clearPending() {
        Flight::Scope flight(watchdog.decoder, Flight::Stage::FrameRelease, decoderMetadata());
        pending = false;
        av_frame_unref(pendingFrame);
        av_frame_unref(cpuFrame);
    }
    bool upload(const detail::MediaFrameQueue::Frame& output) {
        requireApplicationThread();
        auto metadata = presentationMetadata();
        metadata.width = output.width; metadata.height = output.height; metadata.stride = output.width;
        Flight::Scope flight(watchdog.presentation, Flight::Stage::TextureUpload, metadata);
        const auto w = output.width, h = output.height;
        const auto layout = detail::MediaPixelLayout::make(w, h);
        if (output.pixels.size() < layout.size) throw std::invalid_argument("Incomplete media CPU pixels");
        bool useNv12 = output.format == detail::MediaPixelFormat::Nv12 && nv12UploadAvailable;
#if !SDL_VERSION_ATLEAST(2, 0, 16)
        useNv12 = false;
#endif
        if (useNv12 && uploadPixels(output, layout, true)) return uploaded(output);
        if (useNv12) { nv12UploadAvailable = false; ++nv12Fallbacks; }
        if (!uploadPixels(output, layout, false))
            throw std::runtime_error(fog::constants::error::Stream::CannotUpdateVideoTexture);
        return uploaded(output);
    }
    bool uploadPixels(const detail::MediaFrameQueue::Frame& output, const detail::MediaPixelLayout& layout, bool nv12) {
        const int w = output.width, h = output.height;
        const unsigned char* y = output.pixels.data();
        if (!nv12 && output.format == detail::MediaPixelFormat::Nv12) {
            fallbackPixels.resize(layout.size);
            detail::mediaNv12ToIyuv(y, fallbackPixels.data(), layout);
            y = fallbackPixels.data();
        }
        const Uint32 format = nv12 ? SDL_PIXELFORMAT_NV12 : SDL_PIXELFORMAT_IYUV;
        SDL_Texture* previous = FOGPLAY_MEDIA_TEXTURES == 2 ? spareTexture : texture;
        const int previousWidth = FOGPLAY_MEDIA_TEXTURES == 2 ? spareWidth : textureWidth.load();
        const int previousHeight = FOGPLAY_MEDIA_TEXTURES == 2 ? spareHeight : textureHeight.load();
        const Uint32 previousFormat = FOGPLAY_MEDIA_TEXTURES == 2 ? spareFormat : textureFormat;
        // Keep the last good texture until its replacement has been uploaded.
        const bool replace = !previous || w != previousWidth || h != previousHeight || format != previousFormat;
        std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)> candidate(nullptr, SDL_DestroyTexture);
        if (replace) {
            candidate.reset(SDL_CreateTexture(renderer, format, SDL_TEXTUREACCESS_STREAMING, w, h));
            if (!candidate) {
                ++imageCreateErrors;
                if (nv12) return false;
                throw std::runtime_error(fog::constants::error::Stream::CannotAllocateVideoTexture);
            }
        }
        SDL_Texture* target = replace ? candidate.get() : previous;
        const auto uploadStart = DecoderClock::now();
        int result = -1;
#if SDL_VERSION_ATLEAST(2, 0, 16)
        if (nv12) result = SDL_UpdateNVTexture(target, nullptr, y, w, y + layout.ySize, 2 * layout.chromaWidth);
        else
#endif
        result = SDL_UpdateYUVTexture(target, nullptr, y, w, y + layout.ySize, layout.chromaWidth,
            y + layout.ySize + layout.chromaSize, layout.chromaWidth);
        uploadTiming.record(uploadStart);
        if (result != 0) {
            ++imageUploadErrors;
            return false;
        }
        if (replace) {
            if (previous) SDL_DestroyTexture(previous);
            target = candidate.release();
        }
        if (FOGPLAY_MEDIA_TEXTURES == 2) {
            spareTexture = texture; spareWidth = textureWidth; spareHeight = textureHeight; spareFormat = textureFormat;
        }
        texture = target; textureWidth = w; textureHeight = h; textureFormat = format;
        return true;
    }
    bool uploaded(const detail::MediaFrameQueue::Frame& output) {
        const int w = output.width, h = output.height;
        imagePixelFormat = output.format;
        // A draw of the preceding texture cannot acknowledge this upload.
        drawnGeneration = 0;
        ++uploadedFrames;
        uploadedOrigin = output.origin;
        imageUploadedAt = DecoderClock::now();
        const auto sampleStart = imageUploadedAt;
        image.uploaded(detail::VideoImageSample::inspect(output.pixels.data(), output.pixels.size(), w, h,
            output.format == detail::MediaPixelFormat::Nv12), w, h);
        imageTiming.record(sampleStart);
        return true;
    }
    bool preparePresentation(DecoderClock::time_point presentationSlot = DecoderClock::now()) {
        requireApplicationThread();
        requireVideo();
        if (!pending) return false;
        if (!presentationCadence.due(presentationSlot)) return false;
        auto output = convertPending();
        const bool result = upload(output);
        if (result) presentationCadence.uploaded(presentationSlot);
        pixels = std::move(output.pixels);
        clearPending();
        return result;
    }

};
Media::Media(SDL_Renderer* renderer, ::stream::VideoCodec codec, bool deferredPresentation)
    : impl_(std::make_unique<Impl>(renderer, codec, deferredPresentation)) {
    impl_->diagnosticInstance=++mediaDiagnosticInstances;
    if (codec == ::stream::VideoCodec::H265 && !supportsH265()) throw std::runtime_error(fog::constants::error::Stream::H265HardwareDecoderUnavailable);
    impl_->waiting = impl_->awaitingIdr = codec == ::stream::VideoCodec::H265;
    acquireDecoderLogging();
    impl_->ownsCallback = true;
    initializeMediaFormatNames();
    impl_->init();
    impl_->publishVideo(true);impl_->publishAudio(true,false);
}
Media::~Media() { stopWorkers(); }
void Media::setVideoStallReporter(std::function<void(const std::string&)> reporter) {
    impl_->requireApplicationThread();
    if (impl_->workers) throw std::logic_error("Configure video observer before starting workers");
    impl_->watchdog.start(std::move(reporter));
}
void Media::setVideoStallObserver(std::function<void(const mt::StallEvent&)> observer) {
    impl_->requireApplicationThread();
    if (impl_->workers) throw std::logic_error("Configure video observer before starting workers");
    impl_->watchdog.startNumeric(std::move(observer));
}
void Media::Impl::decodeVideo(const std::byte* data, std::size_t size,
    std::uint64_t firstPacketMs, std::uint64_t enqueueMs) {
    const auto admittedUs = metrics::steadyNowUs();
    if (!data || !size || size > ::stream::MaxVideoAccessUnitBytes) throw std::runtime_error(fog::constants::error::Stream::InvalidVideoAccessUnit);
    auto& m = *this;
    m.requireVideo();
    const auto* bytes = reinterpret_cast<const unsigned char*>(data);
    const auto unit = detail::inspectVideo(bytes, size, m.videoCodec);
    m.hardwareEventsBeforePacket = hardwareDiagnostics.snapshot(m.decoder).events;
    m.packetSize = size;
    m.packetNalTypes = unit.types;
    if (m.videoCodec == ::stream::VideoCodec::H265 && !unit.complete)
        m.codecFailure("invalid access unit", AVERROR_INVALIDDATA);
    m.parameters.observe(unit);
    std::vector<unsigned char> prefix;
    if (m.awaitingIdr && !m.parameters.recoveryPrefix(unit, prefix)) return;
    av_packet_unref(m.packet);
    const int allocated = av_new_packet(m.packet, static_cast<int>(prefix.size() + size));
    if (allocated < 0) m.codecFailure("packet allocation", allocated, false);
    if (!prefix.empty()) std::memcpy(m.packet->data, prefix.data(), prefix.size());
    std::memcpy(m.packet->data + prefix.size(), data, size);
    // PTS is used only as an opaque identity here; display cadence continues to
    // use the original RTP timestamp. A missing decoder PTS yields no age sample.
    m.packet->pts = ++m.nextFrameToken;
    const bool hasQueueWait = enqueueMs && enqueueMs <= admittedUs / 1000;
    m.frameOrigins.remember(m.packet->pts, {firstPacketMs,
        hasQueueWait ? admittedUs - enqueueMs * 1000 : 0, hasQueueWait});
    int rc = m.sendPacket();
    if (rc == AVERROR(EAGAIN)) { m.receive(); rc = m.sendPacket(); }
    av_packet_unref(m.packet);
    if (rc < 0) m.codecFailure("send_packet", rc);
    m.awaitingIdr = false;
    m.receive();
    if (!m.deferredPresentation && !m.workers) m.preparePresentation();
}
void Media::video(const std::byte* data, std::size_t size,
    std::uint64_t firstPacketMs, std::uint64_t enqueueMs) {
    impl_->requireApplicationThread();
    if (impl_->workers) throw std::logic_error("Media video is owned by worker");
    impl_->syncDiagnosticOperation();
    try { impl_->decodeVideo(data, size, firstPacketMs, enqueueMs); }
    catch (...) { impl_->publishVideo(true);throw; }
    impl_->publishVideo(true);
}
void Media::Impl::runVideo(::stream::CompressedMediaSource& client) noexcept {
    diagnostics::observeThread("media-video");
    bool paused = false, haveGeneration = false;
    std::uint64_t lastGeneration = 0;
    while (!stopping) {
        const auto wakeSequence = signals->video.sequence();
        ::stream::Packet incoming;
        bool hasPacket = false;
        try {
            if (resetRequested.load()) {
                diagnosticEpoch=epoch.load();
                ++loadGeneration;
                diagnosticLoadGeneration=loadGeneration.load();diagnosticWorkerRun=workerRun.load();
                diagnosticSourceGenerationKnown=haveGeneration;diagnosticSourceGeneration=haveGeneration?lastGeneration:0;
                WorkScope work{workerDecodeTiming};
                resetInProgress = true;
                resetRequested.exchange(false);
                const bool change = codecRequested.exchange(false);
                if (change && requestedCodec.load() != videoCodec) changeCodec(requestedCodec.load());
                else resetVideo();
                discardReady();
                paused = false;
                publishVideo(true);
                resetInProgress = false;
            }
            if (paused) {
                if (!stopping && !resetRequested) signals->video.wait(wakeSequence, std::chrono::seconds(1));
                continue;
            }
            if (!client.pollVideo(incoming, wakeSequence)) {
                // Sparse snapshots need an idle refresh after the last frame.
                // The control profile retains the original frame/reset/error
                // publication sites instead of formatting on every empty poll.
                if (fog::stage1::sparseDiagnostics) publishVideo();
                if (!stopping && !resetRequested) signals->video.wait(wakeSequence, std::chrono::seconds(1));
                continue;
            }
            hasPacket = true;
            FrameCompletionScope completion{incoming.completion};
            if (incoming.kind != ::stream::Kind::Video)
                throw std::logic_error("Video consumer received non-video packet");
            recordQueueWait(incoming, videoQueueWait);
            const auto decodeEpoch = epoch.load();
            diagnosticEpoch=decodeEpoch;
            const auto sourceGeneration = client.currentVideoGeneration();
            if (!client.mediaAdmissionAllowed() || (sourceGeneration && incoming.videoGeneration != sourceGeneration)) {
                discardReady();
                continue;
            }
            const auto selected = client.selectedVideoCodec();
            const bool codecChanged = selected != videoCodec;
            {
                WorkScope work{workerDecodeTiming};
                if (!haveGeneration || codecChanged || incoming.videoGeneration != lastGeneration) ++loadGeneration;
                diagnosticLoadGeneration=loadGeneration.load();diagnosticWorkerRun=workerRun.load();
                diagnosticSourceGeneration=incoming.videoGeneration;diagnosticSourceGenerationKnown=true;
                if (codecChanged) { changeCodec(selected); discardReady(); }
                if (haveGeneration && incoming.videoGeneration != lastGeneration) {
                    if (!codecChanged) resetVideo();
                    discardReady();
                }
                lastGeneration = incoming.videoGeneration;
                haveGeneration = true;
                diagnosticSourceGenerationKnown=true;
                transportGeneration = lastGeneration;
                decodeVideo(incoming.data.data(), incoming.data.size(), incoming.firstPacketMs, incoming.enqueueMs);
            }
            if (pending) {
                WorkScope work{workerConvertTiming};
                auto output = convertPending();
                output.transportGeneration = incoming.videoGeneration;
                clearPending();
                std::lock_guard<std::mutex> lock(readyMutex);
                const auto currentGeneration = client.currentVideoGeneration();
                if (decodeEpoch == epoch.load() && !resetRequested && !stopping && client.mediaAdmissionAllowed() &&
                    (!currentGeneration || incoming.videoGeneration == currentGeneration))
                    supersededFrames += readyFrames.push(std::move(output), incoming.timestamp, DecoderClock::now());
                else ++supersededFrames;
            }
            const auto completedGeneration = client.currentVideoGeneration();
            completion.decoded = !awaitingIdr && decodeEpoch == epoch.load() && !resetRequested && !stopping &&
                client.mediaAdmissionAllowed() && (!completedGeneration || incoming.videoGeneration == completedGeneration);
            publishVideo();
        } catch (...) {
            resetInProgress = false;
            paused = true;
            waiting = true;
            reportError(hasPacket ? &incoming : nullptr);
            try { publishVideo(true); } catch (...) {}
        }
    }
    try { publishVideo(true,mt::Availability::FinalRetained); } catch (...) {}
}
void Media::Impl::runAudio(::stream::CompressedMediaSource& client) noexcept {
    diagnostics::observeThread("media-audio");
    bool paused = false;
    auto nextQueueObservation = DecoderClock::now();
    while (!stopping) {
        const auto wakeSequence = signals->audio.sequence();
        ::stream::Packet incoming;
        bool hasPacket = false;
        try {
            if (paused) {
                if (!stopping) signals->audio.wait(wakeSequence, std::chrono::seconds(1));
                continue;
            }
            if (audioRecovery.expired(DecoderClock::now()))
                throw AudioPlaybackError("Audio recovery deadline exhausted; stopping rental");
            if (!fog::stage1::sparseDiagnostics) observeAudioQueue();
            const auto now = DecoderClock::now();
            if (now >= nextQueueObservation) {
                if (fog::stage1::sparseDiagnostics) observeAudioQueue();
                else publishAudio();
                nextQueueObservation = now + std::chrono::milliseconds(20);
            }
            if (fog::stage1::sparseDiagnostics) publishAudio();
            // Never backpressure the compressed FIFO from SDL: doing so keeps
            // a permanent backlog after a stall. Decode every packet and admit
            // only useful PCM, retaining Opus state while catching up.
            if (!client.pollAudio(incoming)) {
                // SDL queue observations and recovery deadlines still need a
                // bounded maintenance wake, independent of packet arrival.
                if (!stopping) signals->audio.wait(wakeSequence, std::chrono::milliseconds(20));
                continue;
            }
            hasPacket = true;
            if (incoming.kind != ::stream::Kind::Audio)
                throw std::logic_error("Audio consumer received non-audio packet");
            recordQueueWait(incoming, audioQueueWait);
            decodeAudio(incoming.data.data(), incoming.data.size(), incoming.enqueueMs);
            publishAudio();
            nextQueueObservation = DecoderClock::now() + std::chrono::milliseconds(20);
        } catch (...) {
            paused = true;
            reportError(hasPacket ? &incoming : nullptr);
            try { publishAudio(true); } catch (...) {}
        }
    }
    try { publishAudio(true,false,mt::Availability::FinalRetained); } catch (...) {}
}
void Media::startWorkers(::stream::CompressedMediaSource& client, int fps, WorkerPolicy policy) {
    auto& m = *impl_;
    m.requireApplicationThread();
    if (m.workers) throw std::logic_error("Media workers already running");
    if (fps < 1 || fps > 120) throw std::invalid_argument("Invalid media worker frame rate");
    m.workerFps = fps;
    ++m.workerRun;
    m.signals = client.mediaSignals();
    m.workerClient = &client;
    // Both transports drain compressed audio promptly and shed obsolete PCM
    // using the same bounded output policy. Synchronous callers retain opt-in.
    m.boundedWorkerAudio = policy == WorkerPolicy::FogHub || policy == WorkerPolicy::Mts || m.boundedSyncAudio;
    ++m.loadGeneration;
    m.displayPaced = false;
#ifdef __SWITCH__
    SDL_RendererInfo rendererInfo{};
    if (fps == 60 && SDL_GetRendererInfo(m.renderer, &rendererInfo) == 0 &&
        rendererInfo.name && std::strcmp(rendererInfo.name, "opengles2") == 0 &&
        (rendererInfo.flags & SDL_RENDERER_PRESENTVSYNC) && SDL_GL_GetSwapInterval() == 1)
        m.displayPaced = true;
#endif
    m.refreshPrepared = false;
    m.discardReady();
    m.publishVideo(true); m.publishAudio(true);
    m.stopping = false;
    m.resetRequested = false; m.codecRequested = false; m.resetInProgress = false;
    m.transportGeneration = 0;
    {
        std::lock_guard<std::mutex> lock(m.errorMutex);
        m.errors.clear(); m.emergencyError = nullptr;
    }
    m.workers = true;
    try {
        m.videoWorker = std::thread([&m, &client] { m.runVideo(client); });
        m.audioWorker = std::thread([&m, &client] { m.runAudio(client); });
    } catch (...) {
        stopWorkers();
        throw;
    }
}
void Media::stopWorkers() noexcept {
    auto& m = *impl_;
    assert(std::this_thread::get_id() == m.applicationThread);
    const bool hadWorkers=m.workers.load();
    m.stopping = true;
    if (m.workerClient) m.workerClient->wakeMediaWaiters();
    if (m.videoWorker.joinable()) m.videoWorker.join();
    if (m.audioWorker.joinable()) m.audioWorker.join();
    if(hadWorkers) {
        const auto last=m.diagnosticCache.snapshot();
        // Thread creation may have failed before one producer ran. Join before
        // filling only the missing final cache from the now-exclusive owner.
        if(last.video.source.availability!=mt::Availability::FinalRetained)m.publishVideo(true,mt::Availability::FinalRetained);
        if(last.audio.source.availability!=mt::Availability::FinalRetained)m.publishAudio(true,false,mt::Availability::FinalRetained);
    }
    m.workers = false;
    m.signals.reset();
    m.workerClient = nullptr;
    m.discardReady();
    if (m.cpuPool) m.cpuPool->clear();
    // Retain the visible texture as before; only the experimental back texture
    // can be retired at worker stop. Destruction always stays on the UI thread.
    if (m.spareTexture) SDL_DestroyTexture(m.spareTexture);
    m.spareTexture = nullptr; m.spareWidth = m.spareHeight = 0;
    if(hadWorkers) {
        try {
            auto final=captureTelemetry();
            final.presentation.retainedWorkerHistory=true;
            final.presentation.configuredFps=m.workerFps;
            m.diagnosticCache.retainFinal(final);
        } catch (...) {}
    }
}
bool Media::workersRunning() const { return impl_->workers; }
bool Media::takeWorkerError(WorkerError& output) {
    impl_->requireApplicationThread();
    std::lock_guard<std::mutex> lock(impl_->errorMutex);
    if (impl_->emergencyError) {
        output = WorkerError{};
        output.error = std::move(impl_->emergencyError);
        impl_->emergencyError = nullptr;
        return true;
    }
    if (impl_->errors.empty()) return false;
    output = std::move(impl_->errors.front()); impl_->errors.pop_front();
    return true;
}
std::uint64_t Media::workerVideoGeneration() const { return impl_->transportGeneration; }
Media::WorkerVideoLoad Media::workerVideoLoad() const {
    return {impl_->workerDecodeTiming.total.load(), impl_->workerConvertTiming.total.load(), impl_->loadGeneration.load()};
}
bool Media::supportsH265() {
    const auto* codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
    if (!codec) return false;
#ifdef __SWITCH__
    for (int i = 0; const auto* config = avcodec_get_hw_config(codec, i); ++i)
        if (config->pix_fmt == AV_PIX_FMT_NVTEGRA &&
            (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) return true;
    return false;
#else
    return true;
#endif
}
void Media::Impl::changeCodec(::stream::VideoCodec codec) {
    auto& m = *this;
    if (m.videoCodec == codec) return;
    if (!m.workers) m.presentationCadence.reset();
    if (codec == ::stream::VideoCodec::H265 && !supportsH265()) throw std::runtime_error(fog::constants::error::Stream::H265HardwareDecoderUnavailable);
    const bool requireHardware = m.hw;
    m.releaseVideo();
    m.width = m.textureWidth.load(); m.height = m.textureHeight.load();
    m.videoCodec = codec;
    m.parameters = detail::VideoParameters(codec);
    m.packetSize = 0; m.packetNalTypes = 0;
    m.hardwareEventsBeforePacket = 0;
    m.lastFrameFormat = m.lastCpuFormat = AV_PIX_FMT_NONE;
    m.lastColorspace = AVCOL_SPC_UNSPECIFIED; m.lastRange = AVCOL_RANGE_UNSPECIFIED;
    m.lastTrc = AVCOL_TRC_UNSPECIFIED; m.lastPrimaries = AVCOL_PRI_UNSPECIFIED;
    m.waiting = m.awaitingIdr = true;
    m.initVideo(requireHardware || codec == ::stream::VideoCodec::H265);
}
void Media::Impl::resetVideo() {
    auto& m = *this;
    if (!m.workers) m.presentationCadence.reset();
    const bool requireHardware = m.hw || m.videoCodec == ::stream::VideoCodec::H265;
    // Release old frame references and device context before opening another
    // hardware decoder: Switch decoder surfaces are a constrained resource.
    m.releaseVideo();
    m.width = m.textureWidth.load(); m.height = m.textureHeight.load();
    m.hardwareEventsBeforePacket = 0;
    m.lastFrameFormat = m.lastCpuFormat = AV_PIX_FMT_NONE;
    m.lastColorspace = AVCOL_SPC_UNSPECIFIED; m.lastRange = AVCOL_RANGE_UNSPECIFIED;
    m.lastTrc = AVCOL_TRC_UNSPECIFIED; m.lastPrimaries = AVCOL_PRI_UNSPECIFIED;
    m.waiting = m.awaitingIdr = true;
    m.initVideo(requireHardware);
}
void Media::setPresentationFrameRate(int fps) {
    auto& m = *impl_;
    m.requireApplicationThread();
    if (m.workers) throw std::logic_error("Cannot change presentation frame rate while media workers run");
    if (fps != 30 && fps != 60) throw std::invalid_argument("Presentation frame rate must be 30 or 60");
    m.presentationCadence.configure(fps);
    m.workerFps = fps;
}
void Media::setVideoCodec(::stream::VideoCodec codec) {
    auto& m = *impl_;
    m.requireApplicationThread();
    if (!m.workers) {
        if(codec==m.videoCodec.load())return;
        m.syncDiagnosticOperation();
        try {m.changeCodec(codec);}catch(...){m.publishVideo(true);throw;}
        m.publishVideo(true);return;
    }
    if (!m.codecRequested && !m.resetRequested && !m.resetInProgress && codec == m.videoCodec.load()) return;
    m.requestedCodec = codec;
    m.codecRequested = true;
    m.requestReset();
}
void Media::resetVideoDecoder() {
    impl_->requireApplicationThread();
    if (impl_->workers) impl_->requestReset();
    else {
        impl_->syncDiagnosticOperation();
        try {impl_->resetVideo();}catch(...){impl_->publishVideo(true);throw;}
        impl_->publishVideo(true);
    }
}
bool Media::waitingForVideoKeyframe() const {
    return impl_->waiting || (impl_->workers && (impl_->resetRequested || impl_->resetInProgress));
}
void Media::Impl::clearAudioQueue(mt::AudioClearReason reason) {
    const auto before = observeAudioQueue();
    if (audioDevice) SDL_ClearQueuedAudio(audioDevice);
    const auto after = audioDevice ? SDL_GetQueuedAudioSize(audioDevice) : 0;
    const auto observedAt = DecoderClock::now();
    audioMetrics.cleared(before);
    audioClearBeforeBytes = before; audioClearAfterBytes = after;
    audioClearAtUs = telemetryTimeUs(observedAt); audioLastClearReason = reason;
    if (reason == mt::AudioClearReason::Latency) ++audioLatencyClears;
    else if (reason == mt::AudioClearReason::Recovery) ++audioRecoveryClears;
    observedAudioBytes = after; audioObservedAtUs = audioClearAtUs;
    // before/after are SDL software queue observations around clear, not an
    // exact played-sample count: the device may drain concurrently.
}
bool Media::Impl::recoverAudio(DecoderClock::time_point now) {
    if (audioRecovery.expired(now))
        throw AudioPlaybackError("Audio recovery deadline exhausted; stopping rental");
    if (!audioResetNeeded) return true;
    const auto decision = audioRecovery.attempt(now);
    if (decision == AudioRecovery::Attempt::Stop)
        throw AudioPlaybackError("Audio recovery reset budget exhausted; stopping rental");
    if (decision == AudioRecovery::Attempt::Wait) return false;
    ++audioRecoveryAttempts;
    // Clear queued PCM before reopening output or changing predictive state.
    clearAudioQueue(mt::AudioClearReason::Recovery);
    audioCatchingUp = false;
    audioResetAtMs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
    int error = 0;
    auto* replacement = opus_decoder_create(48000, 2, &error);
    if (!replacement || error != OPUS_OK) {
        if (replacement) opus_decoder_destroy(replacement);
        ++audioRecoveryFailures;
        return false;
    }
    if (opus) opus_decoder_destroy(opus);
    opus = replacement;
    if (audioOutputResetNeeded) {
        if (audioDevice) SDL_CloseAudioDevice(audioDevice);
        audioDevice = 0;
        try { openAudioOutput(); }
        catch (...) { ++audioRecoveryFailures; return false; }
    }
    audioResetNeeded = audioOutputResetNeeded = false;
    audioRecovery.resetCompleted();
    return true;
}
void Media::Impl::decodeAudio(const std::byte* data, std::size_t size, std::uint64_t enqueueMs) {
    const auto now = DecoderClock::now();
    if (!recoverAudio(now)) { ++audioRecoveryDiscardedPackets; return; }
    // After a reset discard compressed packets captured before it. A worker
    // drains these without reintroducing the failed episode's audio backlog.
    if (enqueueMs && enqueueMs <= audioResetAtMs) { ++audioRecoveryDiscardedPackets; return; }
    if (!data || !size || size > 65536) {
        ++audioBadPackets;
        if (audioRecovery.badPacket(now)) { audioResetNeeded = true; recoverAudio(now); }
        return;
    }
    std::array<opus_int16, 5760*2> pcm{};
    const int count = opus_decode(opus, reinterpret_cast<const unsigned char*>(data),
        static_cast<opus_int32>(size), pcm.data(), 5760, 0);
    if (count < 0) {
        if (count == OPUS_INVALID_PACKET) {
            ++audioBadPackets;
            if (audioRecovery.badPacket(now)) audioResetNeeded = true;
        } else {
            ++audioDecoderErrors;
            audioRecovery.failed(now); audioResetNeeded = true;
        }
        if (audioResetNeeded) recoverAudio(now);
        return;
    }
    const auto decodedAtUs = telemetryTimeUs(DecoderClock::now());
    if (!audioDecodedFrames) audioFirstPcmAtUs = decodedAtUs;
    audioLastPcmAtUs = decodedAtUs;
    audioDecodedFrames += static_cast<std::uint32_t>(count);
    // Drop stale queued audio after a scheduling stall instead of increasing latency.
    const auto queued = observeAudioQueue();
    const bool boundedAudio = workers ? boundedWorkerAudio : boundedSyncAudio;
    if (!boundedAudio && queued > detail::AudioQueueMetrics::bytesPerSecond / 5) {
        clearAudioQueue(mt::AudioClearReason::Latency);
    }
    auto selected = detail::AudioLatencyPolicy::Selection{0, static_cast<std::uint32_t>(count), false, false};
    if (boundedAudio) {
        const auto now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            DecoderClock::now().time_since_epoch()).count());
        const auto residence = enqueueMs && enqueueMs <= now ? now - enqueueMs : 0;
        selected = detail::AudioLatencyPolicy::select(count, queued, residence);
        if (selected.skip) {
            ++audioDiscardedPackets;
            audioDiscardedFrames += selected.skip;
            if (selected.stale) ++audioStalePackets;
            if (selected.overflow) ++audioOverflowPackets;
            if (!audioCatchingUp) { ++audioCatchups; audioCatchingUp = true; }
        } else if (residence <= detail::AudioLatencyPolicy::staleMs) audioCatchingUp = false;
    }
    // Count decoded packets, including suppressed PCM. Never skip opus_decode
    // above: later packets may depend on its predictor/overlap history.
    ++packets;
    if (!selected.frames) return;
    if (SDL_QueueAudio(audioDevice, pcm.data() + selected.skip * 2,
        static_cast<Uint32>(selected.frames * 2 * sizeof(opus_int16))) != 0) {
        ++audioOutputErrors;
        audioFailedFrames += selected.frames;
        audioRecovery.failed(now); audioResetNeeded = audioOutputResetNeeded = true;
        recoverAudio(now);
        return;
    }
    audioAdmittedFrames += selected.frames;
    observedAudioBytes=SDL_GetQueuedAudioSize(audioDevice);
    const auto queuedAt=DecoderClock::now();audioObservedAtUs=telemetryTimeUs(queuedAt);
    audioMetrics.queued(observedAudioBytes,queuedAt);
    audioRecovery.queued();
}
void Media::audio(const std::byte* data, std::size_t size, std::uint64_t enqueueMs) {
    impl_->requireApplicationThread();
    if (impl_->workers) throw std::logic_error("Media audio is owned by worker");
    try {impl_->decodeAudio(data, size, enqueueMs);}catch(...){impl_->publishAudio(true,false);throw;}
    impl_->publishAudio(true,false);
}
void Media::checkAudioRecovery() const {
    impl_->requireApplicationThread();
    if (!impl_->workers && impl_->audioRecovery.expired(DecoderClock::now()))
        throw AudioPlaybackError("Audio recovery deadline exhausted; stopping rental");
}
bool Media::preparePresentation() {
    return preparePresentation(DecoderClock::now());
}
bool Media::preparePresentation(DecoderClock::time_point presentationSlot) {
    auto& m = *impl_;
    m.requireApplicationThread();
    if (!m.workers) {
        try {
            const bool prepared=m.preparePresentation(presentationSlot);
            m.publishVideo(true);return prepared;
        } catch (...) {m.publishVideo(true);throw;}
    }
    // At most one replacement texture per completed display cycle, even when
    // a caller probes twice. SDL_RenderPresent supplies the active 60 Hz pace.
    if (m.displayPaced && m.refreshPrepared) return false;
    detail::MediaFrameQueue::Frame output;
    std::uint64_t dropped = 0;
    {
        std::lock_guard<std::mutex> lock(m.readyMutex);
        if (!m.readyFrames.take(output, m.displayPaced ? DecoderClock::now() : presentationSlot, dropped)) return false;
    }
    m.supersededFrames += dropped;
    const auto sourceGeneration = m.workerClient->currentVideoGeneration();
    if (!m.workerClient->mediaAdmissionAllowed() ||
        (sourceGeneration && output.transportGeneration != sourceGeneration)) {
        ++m.supersededFrames;
        return false;
    }
    const bool uploaded = m.upload(output);
    if (uploaded && m.displayPaced) m.refreshPrepared = true;
    return uploaded;
}
bool Media::draw() {
    impl_->requireApplicationThread();
    Impl::Flight::Scope flight(impl_->watchdog.presentation, Impl::Flight::Stage::Draw, impl_->presentationMetadata());
    impl_->drawnGeneration = 0;
    if (!impl_->texture) { ++impl_->imageEmptyDraws; return false; }
    const double scale = std::min(1280.0/impl_->textureWidth, 720.0/impl_->textureHeight);
    const int w = static_cast<int>(impl_->textureWidth*scale), h = static_cast<int>(impl_->textureHeight*scale);
    SDL_Rect rect{(1280-w)/2, (720-h)/2, w, h};
    // Redraw letterbox bars too, so closed overlays leave no stale pixels.
    SDL_SetRenderDrawColor(impl_->renderer, 0, 0, 0, 255);
    if (SDL_RenderClear(impl_->renderer) != 0) {
        ++impl_->imageClearErrors;
        throw std::runtime_error(fog::constants::error::Stream::VideoBackgroundRenderingFailed);
    }
    if (SDL_RenderCopy(impl_->renderer, impl_->texture, nullptr, &rect) != 0) {
        ++impl_->imageCopyErrors;
        throw std::runtime_error(fog::constants::error::Stream::VideoRenderingFailed);
    }
    impl_->drawnGeneration = impl_->uploadedFrames;
    impl_->imageLastDrawnGeneration = impl_->drawnGeneration;
    impl_->imageDrawnAt = DecoderClock::now();
    ++impl_->imageDraws;
    return true;
}
void Media::present() {
    impl_->requireApplicationThread();
    Impl::Flight::Scope scope(impl_->watchdog.presentation, Impl::Flight::Stage::Present, impl_->presentationMetadata());
    SDL_RenderPresent(impl_->renderer);
    presented();
}

void Media::presented() {
    impl_->requireApplicationThread();
    impl_->refreshPrepared = false;
    const auto generation = impl_->drawnGeneration;
    impl_->drawnGeneration = 0;
    if (generation && generation != impl_->presentedGeneration) {
        impl_->presentedGeneration = generation;
        ++impl_->presentedFrames;
        impl_->imagePresentedAt = DecoderClock::now();
        impl_->latency.presented(impl_->uploadedOrigin, metrics::steadyNowUs());
        impl_->imagePresentedBlack = impl_->image.last.nearBlack();
        if (impl_->imagePresentedBlack) ++impl_->imagePresentedBlackFrames;
    }
}
std::uint64_t Media::videoFrames() const { return impl_->frames; }
std::uint64_t Media::uploadedFrames() const { return impl_->uploadedFrames; }
std::uint64_t Media::presentedFrames() const { return impl_->presentedFrames; }
std::uint64_t Media::supersededFrames() const { return impl_->supersededFrames; }
std::uint64_t Media::audioPackets() const { return impl_->packets; }
std::uint64_t Media::audioUnderruns() const {
    if (impl_->workers) return impl_->underruns;
    impl_->observeAudioQueue();
    impl_->publishAudio(true,false);
    return impl_->audioMetrics.prolongedEmptyObservations;
}
std::uint64_t Media::audioQueueResets() const { return impl_->workers ? impl_->audioResets.load() : impl_->audioMetrics.resets; }
std::string Media::audioDiagnostics() const {
    if(impl_->workers) {
        mt::Snapshot value;value.captured=mt::Capture::Audio;value.audio=impl_->diagnosticCache.snapshot().audio;
        return mt::formatAudio(value);
    }
    impl_->requireApplicationThread();
    impl_->observeAudioQueue();impl_->publishAudio(true,false);
    return mt::formatAudio(captureTelemetry(mt::Capture::Audio));
}
bool Media::hardwareVideo() const { return impl_->hw; }
int Media::videoWidth() const { return impl_->width; }
int Media::videoHeight() const { return impl_->height; }
std::string Media::diagnostics() const {
    return mt::formatVideo(captureTelemetry(mt::Capture::Video|mt::Capture::Image),mediaFormatNames);
}
std::string Media::imageDiagnostics() const {
    return mt::formatImage(captureTelemetry(mt::Capture::Image));
}
std::string Media::latencyDiagnostics() const {
    return mt::formatLatency(captureTelemetry(mt::Capture::Latency));
}
mt::Snapshot Media::captureTelemetry(mt::Capture mask) const {
    impl_->requireApplicationThread();
    const auto& m=*impl_;
    mt::Snapshot value;value.captured=mask;value.capturedAtUs=telemetryTimeUs();value.mediaInstance=m.diagnosticInstance;
    const auto owners=m.diagnosticCache.snapshot();value.video=owners.video;value.audio=owners.audio;
    auto& p=value.presentation;
    p.decoded=m.frames.load();p.uploaded=m.uploadedFrames.load();p.presented=m.presentedFrames.load();p.superseded=m.supersededFrames.load();
    p.upload=m.uploadTiming.snapshot();p.videoQueueWait=m.videoQueueWait.snapshot();
    p.uploadDistribution=distributionValue(m.uploadTiming.distribution.snapshot(value.capturedAtUs));
    p.workerDecode=m.workerDecodeTiming.snapshot();p.workerConvert=m.workerConvertTiming.snapshot();
    p.currentEpoch=m.epoch.load();p.observedSourceGeneration=m.transportGeneration.load();p.currentLoadGeneration=m.loadGeneration.load();
    p.workerRun=m.workerRun.load();p.workers=m.workers.load();
    p.waitingForKeyframe=m.waiting.load();p.resetRequested=m.resetRequested.load();p.resetInProgress=m.resetInProgress.load();
    p.configuredFps=p.workers||m.presentationCadence.configured()?m.workerFps:0;
    if(mt::includes(mask,mt::Capture::Video)) {
        detail::MediaFrameQueue::Stats stats;
        {
            std::lock_guard<std::mutex> lock(impl_->readyMutex);
            p.queue.depth=m.readyFrames.size();p.queue.limit=m.readyFrames.limit();stats=m.readyFrames.stats();
        }
        p.queue.readyEmpty=stats.readyEmpty;p.queue.cadenceBlocked=stats.timingCadenceBlocked;p.queue.rtpBlocked=stats.timingRtpBlocked;
        p.queue.capacityDrop=stats.pushCapacityDrop;p.queue.staleDrop=stats.takeStaleDrop;p.queue.lateCoalesce=stats.timedLateCoalesce;
        p.queue.taken=stats.taken;p.queue.resets=stats.resets;p.queue.framesPerSecond=stats.framesPerSecond;p.queue.displayPaced=stats.displayPaced;
    }
    if(mt::includes(mask,mt::Capture::Image)) {
        auto& i=value.image;const auto& image=m.image;const auto now=DecoderClock::now();
        i.samples=image.samples;i.points=image.last.points;i.yMin=image.last.yMin;i.yMax=image.last.yMax;i.yMean=image.last.yMean;
        i.darkPoints=image.last.darkPoints;i.chromaDeviation=image.last.chromaDeviation;i.nearBlack=image.last.nearBlack();
        i.blackFrames=image.blackFrames;i.blackRuns=image.blackRuns;i.blackStreak=image.blackStreak;i.blackStreakMax=image.blackStreakMax;
        i.unchangedStreak=image.unchangedStreak;i.unchangedStreakMax=image.unchangedStreakMax;
        i.texture=m.texture!=nullptr;i.width=m.textureWidth.load();i.height=m.textureHeight.load();
        i.uploadedGeneration=m.uploadedFrames.load();i.drawnGeneration=m.imageLastDrawnGeneration;i.presentedGeneration=m.presentedGeneration;
        i.uploadAgeMs=image.samples?std::chrono::duration_cast<std::chrono::milliseconds>(now-m.imageUploadedAt).count():-1;
        i.drawAgeMs=m.imageDraws?std::chrono::duration_cast<std::chrono::milliseconds>(now-m.imageDrawnAt).count():-1;
        i.newPresentAgeMs=m.presentedGeneration?std::chrono::duration_cast<std::chrono::milliseconds>(now-m.imagePresentedAt).count():-1;
        i.draws=m.imageDraws;i.emptyDraws=m.imageEmptyDraws;i.createErrors=m.imageCreateErrors;i.uploadErrors=m.imageUploadErrors;
        i.clearErrors=m.imageClearErrors;i.copyErrors=m.imageCopyErrors;i.presentedBlack=m.imagePresentedBlack;i.presentedBlackFrames=m.imagePresentedBlackFrames;
        i.sample=m.imageTiming.snapshot();i.layout=m.imagePixelFormat==detail::MediaPixelFormat::Nv12?mt::ImageLayout::Nv12:mt::ImageLayout::Iyuv;
        i.nv12=FOGPLAY_MEDIA_NV12;i.nv12Available=m.nv12UploadAvailable;i.nv12Fallbacks=m.nv12Fallbacks;i.cpuPool=FOGPLAY_MEDIA_CPU_POOL;
        i.textureCount=FOGPLAY_MEDIA_TEXTURES;i.readyDepth=FOGPLAY_MEDIA_READY_DEPTH;
    }
    if(mt::includes(mask,mt::Capture::Latency)) {
        const auto last=m.latency.snapshot(value.capturedAtUs,m.workerFps);auto& l=value.latency;
        for(std::size_t stage=0;stage<l.stages.size();++stage) {
            const auto& sample=last.stages[stage];l.stages[stage]={sample.samples,sample.p50Us,sample.p95Us,sample.p99Us,sample.overflow};
        }
        l.lateThresholdUs=last.lateThresholdUs;l.lateCount=last.lateCount;l.latePpm=last.latePpm;
        l.unavailableAge=last.unavailableAge;l.unavailableWait=last.unavailableWait;l.originMisses=m.originMisses.load();
    }
    value.captureCompletedAtUs=telemetryTimeUs();
    mt::sourceAge(value.video.source,value.captureCompletedAtUs);mt::sourceAge(value.audio.source,value.captureCompletedAtUs);
    return value;
}
std::optional<mt::Snapshot> Media::retainedTelemetry() const {return impl_->diagnosticCache.retainedFinal();}
}
