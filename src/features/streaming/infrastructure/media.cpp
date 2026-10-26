#include "core/constants/error_text.hpp"
#include "features/streaming/infrastructure/media.hpp"
#include "features/streaming/domain/media_annexb.hpp"
#include "features/streaming/domain/audio_queue_metrics.hpp"
#include "features/streaming/infrastructure/media_decoder_diagnostics.hpp"
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
    std::uint64_t calls = 0, total = 0, maximum = 0;
    void record(DecoderClock::time_point start) {
        const auto us = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            DecoderClock::now() - start).count());
        ++calls; total += us; maximum = std::max(maximum, us);
    }
    std::uint64_t average() const { return calls ? total / calls : 0; }
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
    std::vector<unsigned char> pixels;
    int width = 0, height = 0;
    int textureWidth = 0, textureHeight = 0;
    bool deferredPresentation = false, pending = false;
    DecoderClock::time_point pendingSince{};
    std::uint64_t uploadedFrames = 0, supersededFrames = 0;
    std::uint64_t presentedFrames = 0, drawnGeneration = 0, presentedGeneration = 0;
    std::uint64_t frames = 0, packets = 0;
    detail::AudioQueueMetrics audioMetrics;
    bool hw = false;
    bool ownsCallback = false;
    bool waiting = false, awaitingIdr = false;
    std::size_t packetSize = 0;
    std::uint64_t packetNalTypes = 0;
    ::stream::VideoCodec videoCodec;
    detail::VideoParameters parameters;
    StageTiming sendTiming, receiveTiming, transferTiming, scaleTiming, uploadTiming;
    std::uint64_t hardwareEventsBeforePacket = 0;
    int lastFrameFormat = AV_PIX_FMT_NONE, lastCpuFormat = AV_PIX_FMT_NONE;
    int lastColorspace = AVCOL_SPC_UNSPECIFIED, lastRange = AVCOL_RANGE_UNSPECIFIED;
    int lastTrc = AVCOL_TRC_UNSPECIFIED, lastPrimaries = AVCOL_PRI_UNSPECIFIED;
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
            decoder ? decoder->width : width, decoder ? decoder->height : height,
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
        const auto bytes = SDL_GetQueuedAudioSize(audioDevice);
        audioMetrics.observe(bytes, DecoderClock::now());
        return bytes;
    }
    std::string audioDiagnostics() {
        const auto bytes = observeAudioQueue();
        std::array<char, 512> text{};
        std::snprintf(text.data(), text.size(),
            "audio_packets=%llu audio_queued_ms=%llu audio_max_queued_ms=%llu "
            "audio_empty_observations=%llu audio_prolonged_empty_observations=%llu audio_empty_threshold_ms=40 "
            "audio_queue_resets=%llu audio_reset_discarded_ms=%llu",
            static_cast<unsigned long long>(packets),
            static_cast<unsigned long long>(detail::AudioQueueMetrics::milliseconds(bytes)),
            static_cast<unsigned long long>(detail::AudioQueueMetrics::milliseconds(audioMetrics.maximumBytes)),
            static_cast<unsigned long long>(audioMetrics.emptyObservations),
            static_cast<unsigned long long>(audioMetrics.prolongedEmptyObservations),
            static_cast<unsigned long long>(audioMetrics.resets),
            static_cast<unsigned long long>(detail::AudioQueueMetrics::milliseconds(audioMetrics.discardedBytes)));
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
        std::swap(hw, candidate.hw);
    }
    void init() {
        initVideo(videoCodec == ::stream::VideoCodec::H265);
        int error = 0;
        opus = opus_decoder_create(48000, 2, &error);
        if (!opus || error != OPUS_OK) throw std::runtime_error(fog::constants::error::Stream::CannotInitializeOpusDecoder);
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
    bool preparePresentation() {
        requireVideo();
        if (!pending) return false;
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
        pixels.resize(static_cast<std::size_t>(w) * h + 2 * ((w+1)/2) * ((h+1)/2));
        scaler = sws_getCachedContext(scaler, w, h, static_cast<AVPixelFormat>(decoded->format),
            w, h, AV_PIX_FMT_YUV420P, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        if (!scaler) throw std::runtime_error(fog::constants::error::Stream::CannotConvertVideoPixelFormat);
        unsigned char* planes[4]{pixels.data(), pixels.data() + w*h,
            pixels.data() + w*h + ((w+1)/2)*((h+1)/2), nullptr};
        int strides[4]{w, (w+1)/2, (w+1)/2, 0};
        const auto scaleStart = DecoderClock::now();
        const int scaled = sws_scale(scaler, decoded->data, decoded->linesize, 0, h, planes, strides);
        scaleTiming.record(scaleStart);
        if (scaled != h) throw std::runtime_error(fog::constants::error::Stream::VideoConversionFailed);
        // Keep the last good texture until its replacement has been uploaded.
        const bool replace = !texture || w != textureWidth || h != textureHeight;
        std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)> candidate(nullptr, SDL_DestroyTexture);
        if (replace) {
            candidate.reset(SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, w, h));
            if (!candidate) throw std::runtime_error(fog::constants::error::Stream::CannotAllocateVideoTexture);
        }
        SDL_Texture* target = replace ? candidate.get() : texture;
        const auto uploadStart = DecoderClock::now();
        const int uploaded = SDL_UpdateYUVTexture(target, nullptr, planes[0], strides[0], planes[1], strides[1], planes[2], strides[2]);
        uploadTiming.record(uploadStart);
        if (uploaded != 0) throw std::runtime_error(fog::constants::error::Stream::CannotUpdateVideoTexture);
        if (replace) {
            if (texture) SDL_DestroyTexture(texture);
            texture = candidate.release();
            textureWidth = w; textureHeight = h;
        }
        // A draw of the preceding texture cannot acknowledge this upload.
        drawnGeneration = 0;
        ++uploadedFrames;
        pending = false;
        av_frame_unref(pendingFrame);
        av_frame_unref(cpuFrame);
        return true;
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
Media::~Media() = default;
void Media::video(const std::byte* data, std::size_t size) {
    if (!data || !size || size > 4*1024*1024) throw std::runtime_error(fog::constants::error::Stream::InvalidVideoAccessUnit);
    auto& m = *impl_;
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
    if (!m.deferredPresentation) m.preparePresentation();
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
void Media::setVideoCodec(::stream::VideoCodec codec) {
    auto& m = *impl_;
    if (m.videoCodec == codec) return;
    if (codec == ::stream::VideoCodec::H265 && !supportsH265()) throw std::runtime_error(fog::constants::error::Stream::H265HardwareDecoderUnavailable);
    const bool requireHardware = m.hw;
    m.releaseVideo();
    m.width = m.textureWidth; m.height = m.textureHeight;
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
void Media::resetVideoDecoder() {
    auto& m = *impl_;
    const bool requireHardware = m.hw || m.videoCodec == ::stream::VideoCodec::H265;
    // Release old frame references and device context before opening another
    // hardware decoder: Switch decoder surfaces are a constrained resource.
    m.releaseVideo();
    m.width = m.textureWidth; m.height = m.textureHeight;
    m.hardwareEventsBeforePacket = 0;
    m.lastFrameFormat = m.lastCpuFormat = AV_PIX_FMT_NONE;
    m.lastColorspace = AVCOL_SPC_UNSPECIFIED; m.lastRange = AVCOL_RANGE_UNSPECIFIED;
    m.lastTrc = AVCOL_TRC_UNSPECIFIED; m.lastPrimaries = AVCOL_PRI_UNSPECIFIED;
    m.waiting = m.awaitingIdr = true;
    m.initVideo(requireHardware);
}
bool Media::waitingForVideoKeyframe() const { return impl_->waiting; }
void Media::audio(const std::byte* data, std::size_t size) {
    if (!size || size > 65536) throw std::runtime_error(fog::constants::error::Stream::InvalidOpusPacket);
    std::array<opus_int16, 5760*2> pcm{};
    const int count = opus_decode(impl_->opus, reinterpret_cast<const unsigned char*>(data),
        static_cast<opus_int32>(size), pcm.data(), 5760, 0);
    if (count < 0) throw std::runtime_error(fog::constants::error::Stream::OpusDecodingFailed);
    // Drop stale queued audio after a scheduling stall instead of increasing latency.
    const auto queued = impl_->observeAudioQueue();
    if (queued > detail::AudioQueueMetrics::bytesPerSecond / 5) {
        SDL_ClearQueuedAudio(impl_->audioDevice);
        impl_->audioMetrics.cleared(queued);
    }
    if (SDL_QueueAudio(impl_->audioDevice, pcm.data(), static_cast<Uint32>(count*2*sizeof(opus_int16))) != 0)
        throw std::runtime_error(fog::constants::error::Stream::CannotQueueAudio);
    ++impl_->packets;
    impl_->audioMetrics.queued(SDL_GetQueuedAudioSize(impl_->audioDevice), DecoderClock::now());
}
bool Media::preparePresentation() { return impl_->preparePresentation(); }
bool Media::draw() {
    impl_->drawnGeneration = 0;
    if (!impl_->texture) return false;
    const double scale = std::min(1280.0/impl_->textureWidth, 720.0/impl_->textureHeight);
    const int w = static_cast<int>(impl_->textureWidth*scale), h = static_cast<int>(impl_->textureHeight*scale);
    SDL_Rect rect{(1280-w)/2, (720-h)/2, w, h};
    // Redraw letterbox bars too, so closed overlays leave no stale pixels.
    SDL_SetRenderDrawColor(impl_->renderer, 0, 0, 0, 255);
    if (SDL_RenderClear(impl_->renderer) != 0)
        throw std::runtime_error(fog::constants::error::Stream::VideoBackgroundRenderingFailed);
    if (SDL_RenderCopy(impl_->renderer, impl_->texture, nullptr, &rect) != 0)
        throw std::runtime_error(fog::constants::error::Stream::VideoRenderingFailed);
    impl_->drawnGeneration = impl_->uploadedFrames;
    return true;
}
void Media::presented() {
    const auto generation = impl_->drawnGeneration;
    impl_->drawnGeneration = 0;
    if (generation && generation != impl_->presentedGeneration) {
        impl_->presentedGeneration = generation;
        ++impl_->presentedFrames;
    }
}
std::uint64_t Media::videoFrames() const { return impl_->frames; }
std::uint64_t Media::uploadedFrames() const { return impl_->uploadedFrames; }
std::uint64_t Media::presentedFrames() const { return impl_->presentedFrames; }
std::uint64_t Media::supersededFrames() const { return impl_->supersededFrames; }
std::uint64_t Media::audioPackets() const { return impl_->packets; }
std::uint64_t Media::audioUnderruns() const {
    impl_->observeAudioQueue();
    return impl_->audioMetrics.prolongedEmptyObservations;
}
std::uint64_t Media::audioQueueResets() const { return impl_->audioMetrics.resets; }
std::string Media::audioDiagnostics() const { return impl_->audioDiagnostics(); }
bool Media::hardwareVideo() const { return impl_->hw; }
int Media::videoWidth() const { return impl_->width; }
int Media::videoHeight() const { return impl_->height; }
std::string Media::diagnostics() const { return impl_->diagnostics(); }
}
