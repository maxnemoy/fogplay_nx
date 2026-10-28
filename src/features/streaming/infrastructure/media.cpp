#include "core/constants/error_text.hpp"
#include "features/streaming/infrastructure/media.hpp"
#include "features/streaming/domain/media_annexb.hpp"
#include "features/streaming/domain/audio_queue_metrics.hpp"
#include "features/streaming/domain/audio_latency_policy.hpp"
#include "features/streaming/domain/audio_recovery.hpp"
#include "features/streaming/infrastructure/media_decoder_diagnostics.hpp"
#include "features/streaming/infrastructure/media_frame_queue.hpp"
#include "features/streaming/infrastructure/video_image_diagnostics.hpp"
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
namespace {
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
};
}
struct Media::Impl {
    SDL_Renderer* renderer;
    SDL_Texture* texture = nullptr;
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
    DecoderClock::time_point pendingSince{};
    std::atomic<std::uint64_t> uploadedFrames{0}, supersededFrames{0};
    std::atomic<std::uint64_t> presentedFrames{0};
    std::uint64_t drawnGeneration = 0, presentedGeneration = 0;
    detail::VideoImageDiagnostics image;
    StageTiming imageTiming;
    DecoderClock::time_point imageUploadedAt{}, imageDrawnAt{}, imagePresentedAt{};
    std::uint64_t imageDraws = 0, imageEmptyDraws = 0;
    std::uint64_t imageCreateErrors = 0, imageUploadErrors = 0, imageClearErrors = 0, imageCopyErrors = 0;
    std::uint64_t imagePresentedBlackFrames = 0, imageLastDrawnGeneration = 0;
    bool imagePresentedBlack = false;
    std::atomic<std::uint64_t> frames{0}, packets{0};
    detail::AudioQueueMetrics audioMetrics;
    std::uint64_t audioDiscardedPackets = 0, audioDiscardedFrames = 0;
    std::uint64_t audioStalePackets = 0, audioOverflowPackets = 0, audioCatchups = 0;
    bool audioCatchingUp = false;
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
    StageTiming sendTiming, receiveTiming, transferTiming, scaleTiming, uploadTiming;
    StageTiming videoQueueWait, audioQueueWait;
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
    std::mutex wakeMutex, readyMutex, errorMutex, snapshotMutex;
    std::condition_variable wake;
    detail::MediaFrameQueue readyFrames;
    std::deque<Media::WorkerError> errors;
    std::string videoSnapshot, audioSnapshot;
    int workerFps = 30;
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
    void publishVideo() {
        auto text = diagnostics();
        std::lock_guard<std::mutex> lock(snapshotMutex);
        videoSnapshot = std::move(text);
    }
    void publishAudio() {
        auto text = audioDiagnostics();
        underruns = audioMetrics.prolongedEmptyObservations;
        audioResets = audioMetrics.resets;
        std::lock_guard<std::mutex> lock(snapshotMutex);
        audioSnapshot = std::move(text);
    }
    void discardReady() {
        std::lock_guard<std::mutex> lock(readyMutex);
        supersededFrames += readyFrames.size();
        readyFrames.reset(workerFps);
    }
    void requestReset() {
        ++epoch;
        waiting = true;
        resetRequested = true;
        discardReady();
        wake.notify_all();
    }
    void reportError(const ::stream::Packet* failed) noexcept {
        try {
            Media::WorkerError event;
            event.error = std::current_exception();
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
    void idle(bool commands = false) {
        std::unique_lock<std::mutex> lock(wakeMutex);
        wake.wait_for(lock, std::chrono::milliseconds(2), [this, commands] {
            return stopping.load() || (commands && resetRequested.load());
        });
    }
    void decodeVideo(const std::byte* data, std::size_t size);
    void decodeAudio(const std::byte* data, std::size_t size, std::uint64_t enqueueMs = 0);
    bool recoverAudio(DecoderClock::time_point now);
    void resetVideo();
    void changeCodec(::stream::VideoCodec codec);
    void runVideo(::stream::StreamingClient& client) noexcept;
    void runAudio(::stream::StreamingClient& client) noexcept;
    explicit Impl(SDL_Renderer* value, ::stream::VideoCodec codec = ::stream::VideoCodec::H264,
        bool deferred = false)
        : renderer(value), deferredPresentation(deferred), videoCodec(codec), parameters(codec) {}
    void releaseVideo() {
        // Pending hardware surfaces must outlive neither decoder nor device.
        av_frame_free(&pendingFrame);
        pending = false;
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
        if (texture) SDL_DestroyTexture(texture);
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
        const auto status = hardwareDiagnostics.snapshot(decoder);
        const char* pixel = decoder ? av_get_pix_fmt_name(decoder->pix_fmt) : nullptr;
        const char* actual = av_get_pix_fmt_name(static_cast<AVPixelFormat>(lastFrameFormat));
        const char* cpu = av_get_pix_fmt_name(static_cast<AVPixelFormat>(lastCpuFormat));
        std::array<char, 1792> text{};
        std::snprintf(text.data(), text.size(),
            "decoder codec=%s codec_id=%d pixfmt=%s frame_pixfmt=%s cpu_pixfmt=%s frame_colorspace=%d frame_color_range=%d frame_color_trc=%d frame_color_primaries=%d threads=%d thread_requested=%d thread_active=%d "
            "send_calls=%llu send_avg_us=%llu send_max_us=%llu receive_calls=%llu receive_avg_us=%llu receive_max_us=%llu "
            "transfer_calls=%llu transfer_avg_us=%llu transfer_max_us=%llu scale_calls=%llu scale_avg_us=%llu scale_max_us=%llu "
            "upload_calls=%llu upload_avg_us=%llu upload_max_us=%llu "
            "decoded_frames=%llu uploaded_frames=%llu presented_frames=%llu superseded_frames=%llu pending_frames=%d pending_frame_age_us=%llu "
            "nv_events=%llu nv_status=%d nv_fresh=%d nv_error=0x%08x nv_mbs_error=%u nv_mbs_ok=%u nv_cycles_raw=%u nv_header_error=0x%08x nv_wait_code=%d",
            decoder && decoder->codec ? decoder->codec->name : "none", decoder ? decoder->codec_id : 0,
            pixel ? pixel : "none", actual ? actual : "none", cpu ? cpu : "none",
            lastColorspace, lastRange, lastTrc, lastPrimaries,
            decoder ? decoder->thread_count : 0, decoder ? decoder->thread_type : 0, decoder ? decoder->active_thread_type : 0,
            static_cast<unsigned long long>(sendTiming.calls), static_cast<unsigned long long>(sendTiming.average()), static_cast<unsigned long long>(sendTiming.maximum),
            static_cast<unsigned long long>(receiveTiming.calls), static_cast<unsigned long long>(receiveTiming.average()), static_cast<unsigned long long>(receiveTiming.maximum),
            static_cast<unsigned long long>(transferTiming.calls), static_cast<unsigned long long>(transferTiming.average()), static_cast<unsigned long long>(transferTiming.maximum),
            static_cast<unsigned long long>(scaleTiming.calls), static_cast<unsigned long long>(scaleTiming.average()), static_cast<unsigned long long>(scaleTiming.maximum),
            static_cast<unsigned long long>(uploadTiming.calls), static_cast<unsigned long long>(uploadTiming.average()), static_cast<unsigned long long>(uploadTiming.maximum),
            static_cast<unsigned long long>(frames), static_cast<unsigned long long>(uploadedFrames),
            static_cast<unsigned long long>(presentedFrames), static_cast<unsigned long long>(supersededFrames), pending ? 1 : 0,
            pending ? static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::microseconds>(
                DecoderClock::now() - pendingSince).count()) : 0ULL,
            static_cast<unsigned long long>(status.events), status.hasStatus ? 1 : 0,
            status.events > hardwareEventsBeforePacket ? 1 : 0, status.error, status.badBlocks, status.goodBlocks,
            status.cycles, status.headerError, status.waitCode);
        return text.data();
    }
    std::uint32_t observeAudioQueue() {
        const auto bytes = audioDevice ? SDL_GetQueuedAudioSize(audioDevice) : 0;
        audioMetrics.observe(bytes, DecoderClock::now());
        return bytes;
    }
    std::string audioDiagnostics() {
        const auto bytes = observeAudioQueue();
        std::array<char, 1792> text{};
        std::snprintf(text.data(), text.size(),
            "audio_packets=%llu audio_queued_ms=%llu audio_max_queued_ms=%llu "
            "audio_empty_observations=%llu audio_prolonged_empty_observations=%llu audio_empty_threshold_ms=40 "
            "audio_queue_resets=%llu audio_reset_discarded_ms=%llu "
            "audio_worker_queue_samples=%llu audio_worker_queue_wait_avg_us=%llu audio_worker_queue_wait_max_us=%llu "
            "audio_pcm_discarded_packets=%llu audio_pcm_discarded_ms=%llu audio_stale_packets=%llu "
            "audio_overflow_packets=%llu audio_catchups=%llu audio_worker_refill_ms=40 audio_worker_max_ms=80 audio_worker_stale_ms=60 "
            "audio_bad_packets=%llu audio_decoder_errors=%llu audio_output_errors=%llu audio_recovery_attempts=%llu "
            "audio_recovery_failures=%llu audio_recovery_discarded_packets=%llu audio_recovery_pending=%d audio_recovery_limit=3/60s audio_recovery_deadline_ms=10000",
            static_cast<unsigned long long>(packets),
            static_cast<unsigned long long>(detail::AudioQueueMetrics::milliseconds(bytes)),
            static_cast<unsigned long long>(detail::AudioQueueMetrics::milliseconds(audioMetrics.maximumBytes)),
            static_cast<unsigned long long>(audioMetrics.emptyObservations),
            static_cast<unsigned long long>(audioMetrics.prolongedEmptyObservations),
            static_cast<unsigned long long>(audioMetrics.resets),
            static_cast<unsigned long long>(detail::AudioQueueMetrics::milliseconds(audioMetrics.discardedBytes)),
            static_cast<unsigned long long>(audioQueueWait.calls),
            static_cast<unsigned long long>(audioQueueWait.average()),
            static_cast<unsigned long long>(audioQueueWait.maximum),
            static_cast<unsigned long long>(audioDiscardedPackets),
            static_cast<unsigned long long>(audioDiscardedFrames * 1000 / detail::AudioLatencyPolicy::sampleRate),
            static_cast<unsigned long long>(audioStalePackets),
            static_cast<unsigned long long>(audioOverflowPackets),
            static_cast<unsigned long long>(audioCatchups),
            static_cast<unsigned long long>(audioBadPackets),
            static_cast<unsigned long long>(audioDecoderErrors),
            static_cast<unsigned long long>(audioOutputErrors),
            static_cast<unsigned long long>(audioRecoveryAttempts),
            static_cast<unsigned long long>(audioRecoveryFailures),
            static_cast<unsigned long long>(audioRecoveryDiscardedPackets), audioResetNeeded ? 1 : 0);
        return text.data();
    }
    int sendPacket() {
        const auto start = DecoderClock::now();
        const int rc = avcodec_send_packet(decoder, packet);
        sendTiming.record(start);
        return rc;
    }
    void initVideo(bool requireHardware = false) {
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
        SDL_PauseAudioDevice(audioDevice, 0);
    }
    void receive() {
        while (true) {
            const auto receiveStart = DecoderClock::now();
            const int rc = avcodec_receive_frame(decoder, frame);
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
            av_frame_unref(pendingFrame);
            av_frame_move_ref(pendingFrame, frame);
            pending = true;
            pendingSince = DecoderClock::now();
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
        output.pixels.swap(pixels);
        AVFrame* decoded = pendingFrame;
        if (pendingFrame->hw_frames_ctx) {
            av_frame_unref(cpuFrame);
            const auto transferStart = DecoderClock::now();
            const int transfer = av_hwframe_transfer_data(cpuFrame, pendingFrame, 0);
            transferTiming.record(transferStart);
            if (transfer < 0) codecFailure("hardware frame transfer", transfer);
            decoded = cpuFrame;
        }
        lastCpuFormat = decoded->format;
        const int w = decoded->width, h = decoded->height;
        if (w < 1 || h < 1 || w > 1920 || h > 1080)
            throw std::runtime_error(fog::constants::error::Stream::VideoExceedsPrototypeLimit1920x1080);
        output.pixels.resize(static_cast<std::size_t>(w) * h + 2 * ((w+1)/2) * ((h+1)/2));
        scaler = sws_getCachedContext(scaler, w, h, static_cast<AVPixelFormat>(decoded->format),
            w, h, AV_PIX_FMT_YUV420P, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        if (!scaler) throw std::runtime_error(fog::constants::error::Stream::CannotConvertVideoPixelFormat);
        unsigned char* planes[4]{output.pixels.data(), output.pixels.data() + w*h,
            output.pixels.data() + w*h + ((w+1)/2)*((h+1)/2), nullptr};
        int strides[4]{w, (w+1)/2, (w+1)/2, 0};
        const auto scaleStart = DecoderClock::now();
        const int scaled = sws_scale(scaler, decoded->data, decoded->linesize, 0, h, planes, strides);
        scaleTiming.record(scaleStart);
        if (scaled != h) throw std::runtime_error(fog::constants::error::Stream::VideoConversionFailed);
        output.width = w; output.height = h;
        return output;
    }
    void clearPending() {
        pending = false;
        av_frame_unref(pendingFrame);
        av_frame_unref(cpuFrame);
    }
    bool upload(const detail::MediaFrameQueue::Frame& output) {
        requireApplicationThread();
        const auto w = output.width, h = output.height;
        const auto* y = output.pixels.data();
        const auto* u = y + w*h;
        const auto* v = u + ((w+1)/2)*((h+1)/2);
        // Keep the last good texture until its replacement has been uploaded.
        const bool replace = !texture || w != textureWidth || h != textureHeight;
        std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)> candidate(nullptr, SDL_DestroyTexture);
        if (replace) {
            candidate.reset(SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, w, h));
            if (!candidate) {
                ++imageCreateErrors;
                throw std::runtime_error(fog::constants::error::Stream::CannotAllocateVideoTexture);
            }
        }
        SDL_Texture* target = replace ? candidate.get() : texture;
        const auto uploadStart = DecoderClock::now();
        const int uploaded = SDL_UpdateYUVTexture(target, nullptr, y, w, u, (w+1)/2, v, (w+1)/2);
        uploadTiming.record(uploadStart);
        if (uploaded != 0) {
            ++imageUploadErrors;
            throw std::runtime_error(fog::constants::error::Stream::CannotUpdateVideoTexture);
        }
        if (replace) {
            if (texture) SDL_DestroyTexture(texture);
            texture = candidate.release();
            textureWidth = w; textureHeight = h;
        }
        // A draw of the preceding texture cannot acknowledge this upload.
        drawnGeneration = 0;
        ++uploadedFrames;
        imageUploadedAt = DecoderClock::now();
        const auto sampleStart = imageUploadedAt;
        image.uploaded(detail::VideoImageSample::inspect(output.pixels.data(), output.pixels.size(), w, h), w, h);
        imageTiming.record(sampleStart);
        return true;
    }
    bool preparePresentation() {
        requireApplicationThread();
        requireVideo();
        if (!pending) return false;
        auto output = convertPending();
        const bool result = upload(output);
        pixels = std::move(output.pixels);
        clearPending();
        return result;
    }

};
Media::Media(SDL_Renderer* renderer, ::stream::VideoCodec codec, bool deferredPresentation)
    : impl_(std::make_unique<Impl>(renderer, codec, deferredPresentation)) {
    if (codec == ::stream::VideoCodec::H265 && !supportsH265()) throw std::runtime_error(fog::constants::error::Stream::H265HardwareDecoderUnavailable);
    impl_->waiting = impl_->awaitingIdr = codec == ::stream::VideoCodec::H265;
    acquireDecoderLogging();
    impl_->ownsCallback = true;
    impl_->init();
}
Media::~Media() { stopWorkers(); }
void Media::Impl::decodeVideo(const std::byte* data, std::size_t size) {
    if (!data || !size || size > 4*1024*1024) throw std::runtime_error(fog::constants::error::Stream::InvalidVideoAccessUnit);
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
    int rc = m.sendPacket();
    if (rc == AVERROR(EAGAIN)) { m.receive(); rc = m.sendPacket(); }
    av_packet_unref(m.packet);
    if (rc < 0) m.codecFailure("send_packet", rc);
    m.awaitingIdr = false;
    m.receive();
    if (!m.deferredPresentation && !m.workers) m.preparePresentation();
}
void Media::video(const std::byte* data, std::size_t size) {
    impl_->requireApplicationThread();
    if (impl_->workers) throw std::logic_error("Media video is owned by worker");
    impl_->decodeVideo(data, size);
}
void Media::Impl::runVideo(::stream::StreamingClient& client) noexcept {
    bool paused = false, haveGeneration = false;
    std::uint64_t lastGeneration = 0;
    while (!stopping) {
        ::stream::Packet incoming;
        bool hasPacket = false;
        try {
            if (resetRequested.load()) {
                resetInProgress = true;
                resetRequested.exchange(false);
                const bool change = codecRequested.exchange(false);
                if (change && requestedCodec.load() != videoCodec) changeCodec(requestedCodec.load());
                else resetVideo();
                discardReady();
                paused = false;
                publishVideo();
                resetInProgress = false;
            }
            if (paused) { idle(true); continue; }
            if (!client.poll(incoming)) { idle(true); continue; }
            hasPacket = true;
            recordQueueWait(incoming, videoQueueWait);
            const auto decodeEpoch = epoch.load();
            const auto selected = client.selectedVideoCodec();
            const bool codecChanged = selected != videoCodec;
            if (codecChanged) { changeCodec(selected); discardReady(); }
            if (haveGeneration && incoming.videoGeneration != lastGeneration) {
                if (!codecChanged) resetVideo();
                discardReady();
            }
            lastGeneration = incoming.videoGeneration;
            haveGeneration = true;
            transportGeneration = lastGeneration;
            decodeVideo(incoming.data.data(), incoming.data.size());
            if (pending) {
                auto output = convertPending();
                clearPending();
                std::lock_guard<std::mutex> lock(readyMutex);
                if (decodeEpoch == epoch.load() && !resetRequested && !stopping)
                    supersededFrames += readyFrames.push(std::move(output), incoming.timestamp, DecoderClock::now());
                else ++supersededFrames;
            }
            publishVideo();
        } catch (...) {
            resetInProgress = false;
            paused = true;
            waiting = true;
            reportError(hasPacket ? &incoming : nullptr);
            try { publishVideo(); } catch (...) {}
        }
    }
}
void Media::Impl::runAudio(::stream::StreamingClient& client) noexcept {
    bool paused = false;
    auto nextSnapshot = DecoderClock::now();
    while (!stopping) {
        ::stream::Packet incoming;
        bool hasPacket = false;
        try {
            if (paused) { idle(); continue; }
            if (audioRecovery.expired(DecoderClock::now()))
                throw AudioPlaybackError("Audio recovery deadline exhausted; stopping rental");
            observeAudioQueue();
            const auto now = DecoderClock::now();
            if (now >= nextSnapshot) {
                publishAudio();
                nextSnapshot = now + std::chrono::milliseconds(20);
            }
            // Never backpressure the compressed FIFO from SDL: doing so keeps
            // a permanent backlog after a stall. Decode every packet and admit
            // only useful PCM, retaining Opus state while catching up.
            if (!client.pollAudio(incoming)) { idle(); continue; }
            hasPacket = true;
            recordQueueWait(incoming, audioQueueWait);
            decodeAudio(incoming.data.data(), incoming.data.size(), incoming.enqueueMs);
            publishAudio();
            nextSnapshot = DecoderClock::now() + std::chrono::milliseconds(20);
        } catch (...) {
            paused = true;
            reportError(hasPacket ? &incoming : nullptr);
            try { publishAudio(); } catch (...) {}
        }
    }
}
void Media::startWorkers(::stream::StreamingClient& client, int fps) {
    auto& m = *impl_;
    m.requireApplicationThread();
    if (m.workers) throw std::logic_error("Media workers already running");
    if (fps < 1 || fps > 120) throw std::invalid_argument("Invalid media worker frame rate");
    m.workerFps = fps;
    m.discardReady();
    m.publishVideo(); m.publishAudio();
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
    m.stopping = true;
    m.wake.notify_all();
    if (m.videoWorker.joinable()) m.videoWorker.join();
    if (m.audioWorker.joinable()) m.audioWorker.join();
    m.workers = false;
    m.discardReady();
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
void Media::setVideoCodec(::stream::VideoCodec codec) {
    auto& m = *impl_;
    m.requireApplicationThread();
    if (!m.workers) { m.changeCodec(codec); return; }
    if (!m.codecRequested && !m.resetRequested && !m.resetInProgress && codec == m.videoCodec.load()) return;
    m.requestedCodec = codec;
    m.codecRequested = true;
    m.requestReset();
}
void Media::resetVideoDecoder() {
    impl_->requireApplicationThread();
    if (impl_->workers) impl_->requestReset();
    else impl_->resetVideo();
}
bool Media::waitingForVideoKeyframe() const {
    return impl_->waiting || (impl_->workers && (impl_->resetRequested || impl_->resetInProgress));
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
    const auto queued = observeAudioQueue();
    if (audioDevice) SDL_ClearQueuedAudio(audioDevice);
    audioMetrics.cleared(queued);
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
    // Drop stale queued audio after a scheduling stall instead of increasing latency.
    const auto queued = observeAudioQueue();
    if (!workers && queued > detail::AudioQueueMetrics::bytesPerSecond / 5) {
        SDL_ClearQueuedAudio(audioDevice);
        audioMetrics.cleared(queued);
    }
    auto selected = detail::AudioLatencyPolicy::Selection{0, static_cast<std::uint32_t>(count), false, false};
    if (workers) {
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
        audioRecovery.failed(now); audioResetNeeded = audioOutputResetNeeded = true;
        recoverAudio(now);
        return;
    }
    audioMetrics.queued(SDL_GetQueuedAudioSize(audioDevice), DecoderClock::now());
    audioRecovery.queued();
}
void Media::audio(const std::byte* data, std::size_t size, std::uint64_t enqueueMs) {
    impl_->requireApplicationThread();
    if (impl_->workers) throw std::logic_error("Media audio is owned by worker");
    impl_->decodeAudio(data, size, enqueueMs);
}
void Media::checkAudioRecovery() const {
    impl_->requireApplicationThread();
    if (!impl_->workers && impl_->audioRecovery.expired(DecoderClock::now()))
        throw AudioPlaybackError("Audio recovery deadline exhausted; stopping rental");
}
bool Media::preparePresentation() {
    auto& m = *impl_;
    m.requireApplicationThread();
    if (!m.workers) return m.preparePresentation();
    detail::MediaFrameQueue::Frame output;
    std::uint64_t dropped = 0;
    {
        std::lock_guard<std::mutex> lock(m.readyMutex);
        if (!m.readyFrames.take(output, DecoderClock::now(), dropped)) return false;
    }
    m.supersededFrames += dropped;
    return m.upload(output);
}
bool Media::draw() {
    impl_->requireApplicationThread();
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
void Media::presented() {
    impl_->requireApplicationThread();
    const auto generation = impl_->drawnGeneration;
    impl_->drawnGeneration = 0;
    if (generation && generation != impl_->presentedGeneration) {
        impl_->presentedGeneration = generation;
        ++impl_->presentedFrames;
        impl_->imagePresentedAt = DecoderClock::now();
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
    return impl_->audioMetrics.prolongedEmptyObservations;
}
std::uint64_t Media::audioQueueResets() const { return impl_->workers ? impl_->audioResets.load() : impl_->audioMetrics.resets; }
std::string Media::audioDiagnostics() const {
    if (!impl_->workers) return impl_->audioDiagnostics();
    std::lock_guard<std::mutex> lock(impl_->snapshotMutex);
    return impl_->audioSnapshot;
}
bool Media::hardwareVideo() const { return impl_->hw; }
int Media::videoWidth() const { return impl_->width; }
int Media::videoHeight() const { return impl_->height; }
std::string Media::diagnostics() const {
    impl_->requireApplicationThread();
    if (!impl_->workers) return impl_->diagnostics() + " " + imageDiagnostics();
    std::string snapshot;
    {
        std::lock_guard<std::mutex> lock(impl_->snapshotMutex);
        snapshot = impl_->videoSnapshot;
    }
    // Decoder fields describe the last completed worker operation. Presentation
    // counters belong to UI and can advance after the worker's latest snapshot.
    for (const auto& entry : std::array<std::pair<const char*, std::uint64_t>, 7>{{
        {"decoded_frames=", impl_->frames.load()},
        {"uploaded_frames=", impl_->uploadedFrames.load()},
        {"presented_frames=", impl_->presentedFrames.load()},
        {"superseded_frames=", impl_->supersededFrames.load()},
        {"upload_calls=", impl_->uploadTiming.calls.load()},
        {"upload_avg_us=", impl_->uploadTiming.average()},
        {"upload_max_us=", impl_->uploadTiming.maximum.load()}}}) {
        const auto start = snapshot.find(entry.first);
        if (start == std::string::npos) continue;
        const auto value = start + std::strlen(entry.first);
        const auto end = snapshot.find(' ', value);
        snapshot.replace(value, end == std::string::npos ? snapshot.size() - value : end - value,
            std::to_string(entry.second));
    }
    std::size_t depth;
    {
        std::lock_guard<std::mutex> lock(impl_->readyMutex);
        depth = impl_->readyFrames.size();
    }
    return snapshot + " async_workers=1 presentation_queue=" + std::to_string(depth) +
        " presentation_queue_limit=3 presentation_fps=" + std::to_string(impl_->workerFps) +
        " video_worker_queue_samples=" + std::to_string(impl_->videoQueueWait.calls.load()) +
        " video_worker_queue_wait_avg_us=" + std::to_string(impl_->videoQueueWait.average()) +
        " video_worker_queue_wait_max_us=" + std::to_string(impl_->videoQueueWait.maximum.load()) + " " + imageDiagnostics();
}
std::string Media::imageDiagnostics() const {
    impl_->requireApplicationThread();
    const auto& m = *impl_;
    const auto& image = m.image;
    const auto now = DecoderClock::now();
    std::array<char, 1536> text{};
    std::snprintf(text.data(), text.size(),
        "image_source=cpu_iyuv_grid image_grid_max=576 image_samples=%llu image_points=%u "
        "image_y_min=%u image_y_max=%u image_y_mean=%u image_dark_points=%u image_chroma_deviation=%u "
        "image_near_black=%d image_black_frames=%llu image_black_runs=%llu image_black_streak=%llu image_black_streak_max=%llu "
        "image_unchanged_streak=%llu image_unchanged_streak_max=%llu "
        "image_texture=%d image_width=%d image_height=%d image_uploaded_generation=%llu image_drawn_generation=%llu image_presented_generation=%llu "
        "image_upload_age_ms=%lld image_draw_age_ms=%lld image_new_present_age_ms=%lld "
        "image_draws=%llu image_empty_draws=%llu image_create_errors=%llu image_upload_errors=%llu image_clear_errors=%llu image_copy_errors=%llu "
        "image_presented_black=%d image_presented_black_frames=%llu image_sample_avg_us=%llu image_sample_max_us=%llu",
        static_cast<unsigned long long>(image.samples), image.last.points,
        image.last.yMin, image.last.yMax, image.last.yMean, image.last.darkPoints, image.last.chromaDeviation,
        image.last.nearBlack() ? 1 : 0,
        static_cast<unsigned long long>(image.blackFrames), static_cast<unsigned long long>(image.blackRuns),
        static_cast<unsigned long long>(image.blackStreak), static_cast<unsigned long long>(image.blackStreakMax),
        static_cast<unsigned long long>(image.unchangedStreak), static_cast<unsigned long long>(image.unchangedStreakMax),
        m.texture ? 1 : 0, m.textureWidth.load(), m.textureHeight.load(),
        static_cast<unsigned long long>(m.uploadedFrames.load()), static_cast<unsigned long long>(m.imageLastDrawnGeneration),
        static_cast<unsigned long long>(m.presentedGeneration),
        image.samples ? static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(now - m.imageUploadedAt).count()) : -1LL,
        m.imageDraws ? static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(now - m.imageDrawnAt).count()) : -1LL,
        m.presentedGeneration ? static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(now - m.imagePresentedAt).count()) : -1LL,
        static_cast<unsigned long long>(m.imageDraws), static_cast<unsigned long long>(m.imageEmptyDraws),
        static_cast<unsigned long long>(m.imageCreateErrors), static_cast<unsigned long long>(m.imageUploadErrors),
        static_cast<unsigned long long>(m.imageClearErrors), static_cast<unsigned long long>(m.imageCopyErrors),
        m.imagePresentedBlack ? 1 : 0, static_cast<unsigned long long>(m.imagePresentedBlackFrames),
        static_cast<unsigned long long>(m.imageTiming.average()), static_cast<unsigned long long>(m.imageTiming.maximum.load()));
    return text.data();
}
}
