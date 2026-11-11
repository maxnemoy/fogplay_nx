#pragma once
#include "features/streaming/domain/media_telemetry.hpp"
#include <array>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <string>

namespace fog::media::telemetry {
// A bounded process-lifetime dictionary of SDK's fixed public format names.
// Media construction initializes it before producing records. Formatting only
// reads this owned immutable metadata; it neither queries nor includes FFmpeg.
class FormatNames {
public:
    static constexpr std::size_t PixelCapacity=512,NameCapacity=32;
    explicit FormatNames(bool published=true):published_(published){}
    void publish() noexcept { published_.store(true,std::memory_order_release); }
    bool setPixel(std::size_t id,const char* name) {
        if(id>=PixelCapacity||!name)return false;
        const auto size=std::strlen(name);
        if(size>=NameCapacity)return false;
        std::memcpy(pixels_[id].data(),name,size+1);return true;
    }
    const char* pixel(std::int32_t id) const noexcept {
        return published_.load(std::memory_order_acquire)&&id>=0&&std::size_t(id)<PixelCapacity&&pixels_[id][0]?pixels_[id].data():"none";
    }
private:
    std::array<std::array<char,NameCapacity>,PixelCapacity> pixels_{};
    std::atomic<bool> published_;
};
static_assert(sizeof(FormatNames)<=17*1024);
inline const char* codecName(Codec codec) { return codec==Codec::H264?"h264":codec==Codec::Hevc?"hevc":"none"; }
// Read-only process metadata initialized by Media before any publication.
const FormatNames& processFormatNames() noexcept;
std::string formatDecoder(const Snapshot&,const FormatNames&);
std::string formatVideo(const Snapshot&,const FormatNames&);
std::string formatAudio(const Snapshot&);
std::string formatImage(const Snapshot&);
std::string formatLatency(const Snapshot&);
std::string format(const Record&,const FormatNames&);
inline constexpr std::size_t MaximumFormattedBytes=Record::MaximumRenderedBytes;
}

namespace fog::media::telemetry {
namespace formatter_detail {
using Number=unsigned long long;
inline Number n(std::uint64_t value){return static_cast<Number>(value);}
inline std::uint64_t audioMs(std::uint64_t bytes){return bytes*1000/(48000*2*2);}
inline std::string percentiles(Percentiles value){
    return "samples="+std::to_string(value.samples)+" p50Us="+std::to_string(value.p50Us)+" p95Us="+std::to_string(value.p95Us)+
        " p99Us="+std::to_string(value.p99Us)+" overflowLifetime="+std::to_string(value.overflowLifetime);
}
}
using formatter_detail::n;
using formatter_detail::audioMs;
using formatter_detail::percentiles;
inline std::string formatDecoder(const Snapshot& value,const FormatNames& names){
    const auto& v=value.video;const auto& p=value.presentation;const auto& h=v.hardware;
    const auto pendingAge=v.pending&&!p.workers&&!p.retainedWorkerHistory&&value.capturedAtUs>=v.pendingSinceUs ? value.capturedAtUs-v.pendingSinceUs : v.pendingAgeUs;
    std::array<char,4096> text{};
    std::snprintf(text.data(),text.size(),
        "decoder codec=%s codec_id=%d pixfmt=%s frame_pixfmt=%s cpu_pixfmt=%s frame_colorspace=%d frame_color_range=%d frame_color_trc=%d frame_color_primaries=%d threads=%d thread_requested=%d thread_active=%d "
        "send_calls=%llu send_avg_us=%llu send_max_us=%llu receive_calls=%llu receive_avg_us=%llu receive_max_us=%llu "
        "transfer_calls=%llu transfer_avg_us=%llu transfer_max_us=%llu scale_calls=%llu scale_avg_us=%llu scale_max_us=%llu "
        "upload_calls=%llu upload_avg_us=%llu upload_max_us=%llu "
        "decoded_frames=%llu uploaded_frames=%llu presented_frames=%llu superseded_frames=%llu pending_frames=%d pending_frame_age_us=%llu "
        "nv_events=%llu nv_status=%d nv_fresh=%d nv_error=0x%08x nv_mbs_error=%u nv_mbs_ok=%u nv_cycles_raw=%u nv_header_error=0x%08x nv_wait_code=%d",
        codecName(v.codec),v.codecId,names.pixel(v.pixelFormat),names.pixel(v.framePixelFormat),names.pixel(v.cpuPixelFormat),
        v.colorspace,v.range,v.trc,v.primaries,v.threads,v.threadRequested,v.threadActive,
        n(v.stages[0].calls),n(average(v.stages[0])),n(v.stages[0].maxUs),
        n(v.stages[1].calls),n(average(v.stages[1])),n(v.stages[1].maxUs),
        n(v.stages[2].calls),n(average(v.stages[2])),n(v.stages[2].maxUs),
        n(v.stages[3].calls),n(average(v.stages[3])),n(v.stages[3].maxUs),
        n(p.upload.calls),n(average(p.upload)),n(p.upload.maxUs),n(p.decoded),n(p.uploaded),n(p.presented),n(p.superseded),v.pending?1:0,n(pendingAge),
        n(h.events),h.hasStatus?1:0,h.fresh?1:0,h.error,h.badBlocks,h.goodBlocks,h.cycles,h.headerError,h.waitCode);
    return text.data();
}
inline std::string formatImage(const Snapshot& value){
    const auto& i=value.image;
    std::array<char,4096> text{};
    std::snprintf(text.data(),text.size(),
        "image_source=%s image_grid_max=576 image_samples=%llu image_points=%u "
        "image_y_min=%u image_y_max=%u image_y_mean=%u image_dark_points=%u image_chroma_deviation=%u "
        "image_near_black=%d image_black_frames=%llu image_black_runs=%llu image_black_streak=%llu image_black_streak_max=%llu "
        "image_unchanged_streak=%llu image_unchanged_streak_max=%llu "
        "image_texture=%d image_width=%d image_height=%d image_uploaded_generation=%llu image_drawn_generation=%llu image_presented_generation=%llu "
        "image_upload_age_ms=%lld image_draw_age_ms=%lld image_new_present_age_ms=%lld "
        "image_draws=%llu image_empty_draws=%llu image_create_errors=%llu image_upload_errors=%llu image_clear_errors=%llu image_copy_errors=%llu "
        "image_presented_black=%d image_presented_black_frames=%llu image_sample_avg_us=%llu image_sample_max_us=%llu",
        i.layout==ImageLayout::Nv12?"cpu_nv12_grid":"cpu_iyuv_grid",n(i.samples),i.points,i.yMin,i.yMax,i.yMean,i.darkPoints,i.chromaDeviation,
        i.nearBlack?1:0,n(i.blackFrames),n(i.blackRuns),n(i.blackStreak),n(i.blackStreakMax),n(i.unchangedStreak),n(i.unchangedStreakMax),
        i.texture?1:0,i.width,i.height,n(i.uploadedGeneration),n(i.drawnGeneration),n(i.presentedGeneration),
        static_cast<long long>(i.uploadAgeMs),static_cast<long long>(i.drawAgeMs),static_cast<long long>(i.newPresentAgeMs),
        n(i.draws),n(i.emptyDraws),n(i.createErrors),n(i.uploadErrors),n(i.clearErrors),n(i.copyErrors),i.presentedBlack?1:0,n(i.presentedBlackFrames),
        n(average(i.sample)),n(i.sample.maxUs));
    return std::string(text.data())+" mediaNv12="+std::to_string(i.nv12)+" mediaNv12Available="+std::to_string(i.nv12Available)+
        " mediaNv12Fallbacks="+std::to_string(i.nv12Fallbacks)+" mediaCpuPool="+std::to_string(i.cpuPool)+
        " mediaTextures="+std::to_string(i.textureCount)+" mediaReadyDepth="+std::to_string(i.readyDepth);
}
inline std::string formatVideo(const Snapshot& value,const FormatNames& names){
    const auto& p=value.presentation;const auto& q=p.queue;
    std::string text=formatDecoder(value,names);
    if(!p.workers&&!p.retainedWorkerHistory)text+=" async_workers=0 presentation_queue_limit=1 presentation_fps="+std::to_string(p.configuredFps);
    else {
        text+=" async_workers="+std::to_string(p.workers)+" presentation_queue="+std::to_string(q.depth)+" presentation_queue_limit="+std::to_string(q.limit)+
            " presentation_fps="+std::to_string(p.configuredFps)+" video_worker_queue_samples="+std::to_string(p.videoQueueWait.calls)+
            " video_worker_queue_wait_avg_us="+std::to_string(average(p.videoQueueWait))+" video_worker_queue_wait_max_us="+std::to_string(p.videoQueueWait.maxUs)+
            " displayPaced="+std::to_string(q.displayPaced)+" queueFps="+std::to_string(q.framesPerSecond)+
            " readyEmpty="+std::to_string(q.readyEmpty)+" cadenceBlocked="+std::to_string(q.cadenceBlocked)+" rtpBlocked="+std::to_string(q.rtpBlocked)+
            " pushCapacityDrop="+std::to_string(q.capacityDrop)+" takeStaleDrop="+std::to_string(q.staleDrop)+
            " timedLateCoalesce="+std::to_string(q.lateCoalesce)+" taken="+std::to_string(q.taken)+" resets="+std::to_string(q.resets);
    }
    if(includes(value.captured,Capture::Image))text+=' '+formatImage(value);
    return text;
}
inline std::string formatAudio(const Snapshot& value){
    const auto& a=value.audio;
    std::array<char,4096> text{};
    std::snprintf(text.data(),text.size(),
        "audio_packets=%llu audio_queued_ms=%llu audio_max_queued_ms=%llu "
        "audio_empty_observations=%llu audio_prolonged_empty_observations=%llu audio_empty_threshold_ms=40 "
        "audio_queue_resets=%llu audio_reset_discarded_ms=%llu "
        "audio_worker_queue_samples=%llu audio_worker_queue_wait_avg_us=%llu audio_worker_queue_wait_max_us=%llu "
        "audio_pcm_discarded_packets=%llu audio_pcm_discarded_ms=%llu audio_stale_packets=%llu "
        "audio_overflow_packets=%llu audio_catchups=%llu audio_worker_refill_ms=40 audio_worker_max_ms=80 audio_worker_stale_ms=60 "
        "audio_bad_packets=%llu audio_decoder_errors=%llu audio_output_errors=%llu audio_recovery_attempts=%llu "
        "audio_recovery_failures=%llu audio_recovery_discarded_packets=%llu audio_recovery_pending=%d audio_recovery_limit=3/60s audio_recovery_deadline_ms=10000 audio_sync_bounded=%d "
        "audio_floor_last_ms=%llu audio_floor_max_ms=%llu audio_floor_windows=%llu audio_floor_elevated_windows=%llu "
        "audio_floor_window_ms=10000 audio_floor_elevated_ms=100 "
        "audio_worker_bounded=%d audio_pcm_decoded_frames=%llu audio_pcm_admitted_frames=%llu audio_pcm_failed_frames=%llu "
        "audio_pcm_suppressed_frames=%llu audio_pcm_first_at_us=%llu audio_pcm_last_at_us=%llu audio_pcm_elapsed_us=%llu "
        "audio_output_format_source=sdl-obtained-stream audio_output_rate=%u audio_output_channels=%u audio_output_bytes_per_frame=%u "
        "audio_latency_clears=%llu audio_recovery_clears=%llu audio_clear_reason=%s "
        "audio_clear_before_bytes=%llu audio_clear_after_bytes=%llu audio_clear_at_us=%llu audio_queue_observed_at_us=%llu",
        n(a.packets),n(audioMs(a.queuedBytes)),n(audioMs(a.maximumBytes)),n(a.emptyObservations),n(a.prolongedEmptyObservations),
        n(a.resets),n(audioMs(a.discardedBytes)),n(a.queueWait.calls),n(average(a.queueWait)),n(a.queueWait.maxUs),
        n(a.pcmDiscardedPackets),n(a.pcmDiscardedFrames*1000/48000),n(a.stalePackets),n(a.overflowPackets),n(a.catchups),
        n(a.badPackets),n(a.decoderErrors),n(a.outputErrors),n(a.recoveryAttempts),n(a.recoveryFailures),n(a.recoveryDiscardedPackets),
        a.recoveryPending?1:0,a.boundedSync?1:0,n(audioMs(a.lastFloorBytes)),n(audioMs(a.maximumFloorBytes)),n(a.floorWindows),n(a.elevatedFloorWindows),
        a.boundedWorker?1:0,n(a.decodedPcmFrames),n(a.admittedPcmFrames),n(a.failedPcmFrames),n(a.pcmDiscardedFrames),
        n(a.firstPcmAtUs),n(a.lastPcmAtUs),n(a.lastPcmAtUs>=a.firstPcmAtUs?a.lastPcmAtUs-a.firstPcmAtUs:0),
        a.outputSampleRate,a.outputChannels,a.outputBytesPerFrame,n(a.latencyClears),n(a.recoveryClears),
        a.lastClearReason==AudioClearReason::Latency?"latency":a.lastClearReason==AudioClearReason::Recovery?"recovery":"none",
        n(a.clearBeforeBytes),n(a.clearAfterBytes),n(a.clearAtUs),n(a.queueObservedAtUs));
    return text.data();
}
inline std::string formatLatency(const Snapshot& value){
    const auto& l=value.latency;
    return "videoLatency windowMs=5000 sample=completed-new-present timestampResolutionMs=1 "
        "frameAgeSource=local-first-packet-to-sdl-return frameAge{"+percentiles(l.stages[0])+"} assembledQueueWait{"+percentiles(l.stages[1])+"} presentGap{"+percentiles(l.stages[2])+"} lateThresholdUs="+
        std::to_string(l.lateThresholdUs)+" lateCount="+std::to_string(l.lateCount)+" latePpm="+std::to_string(l.latePpm)+
        " frameAgeUnavailableLifetime="+std::to_string(l.unavailableAge)+" queueWaitUnavailableLifetime="+std::to_string(l.unavailableWait)+
        " originMapCapacity=64 originMissesLifetime="+std::to_string(l.originMisses);
}
inline const char* stallStageName(StallStage stage) noexcept {
    switch (stage) {
        case StallStage::DecoderInit:return "decoder-init";
        case StallStage::DecodeSend:return "decode-send";
        case StallStage::DecodeReceive:return "decode-receive";
        case StallStage::FrameRelease:return "frame-release";
        case StallStage::DecoderRelease:return "decoder-release";
        case StallStage::Transfer:return "hardware-transfer";
        case StallStage::Convert:return "pixel-convert";
        case StallStage::TextureUpload:return "texture-upload";
        case StallStage::TextureRelease:return "texture-release";
        case StallStage::Draw:return "render-draw";
        case StallStage::Present:return "render-present";
        default:return "idle";
    }
}
inline std::string formatStall(const StallEvent& value) {
    if (value.kind == StallKind::ObserverUnavailable) return "observer unavailable; video stall diagnostics disabled";
    const auto& m = value.metadata;
    return std::string("stall lane=") + (value.lane == StallLane::Presentation ? "presentation" : "decoder") +
        " stage=" + stallStageName(value.stage) + " sequence=" + std::to_string(value.sequence) +
        " monotonicMs=" + std::to_string(value.monotonicMs) + " elapsedMs=" + std::to_string(value.elapsedMs) +
        " width=" + std::to_string(m.width) + " height=" + std::to_string(m.height) +
        " codec=" + std::to_string(m.codec) + " format=" + std::to_string(m.format) +
        " stride=" + std::to_string(m.stride) + " bytes=" + std::to_string(m.bytes) +
        " nalTypes=" + std::to_string(m.nalTypes) + " frame=" + std::to_string(m.frame);
}
inline std::string formatStage(const StageDistributionValue& value, bool published = true) {
    using A = StageAvailability;
    if (!published) return "availability=not-published";
    if (value.availability != A::Available)
        return value.availability == A::InstrumentationDisabled ? "availability=instrumentation-disabled" : "availability=not-collected";
    return "availability=available capturedAtSteadyUs=" + std::to_string(value.capturedAtSteadyUs) +
        " samples=" + std::to_string(value.samples) + " p50Us=" + std::to_string(value.p50Us) +
        " p95Us=" + std::to_string(value.p95Us) + " p99Us=" + std::to_string(value.p99Us) +
        " overflowLifetime=" + std::to_string(value.overflowLifetime);
}
inline std::string formatStages(const Snapshot& value) {
    std::string text = "Media stages: schema=1 clock=steady-us scope=rolling5s windowUs=5000000 capacity=512 quantile=exact-nearest-rank boundary=sdk-call-return";
    constexpr std::array<const char*,4> names{{"decodeSend", "decodeReceive", "hardwareTransfer", "pixelScale"}};
    const bool published = value.video.source.availability != Availability::NotPublished;
    text += std::string(" videoSourceAvailability=") + (!published ? "not-published" :
        value.video.source.availability == Availability::FinalRetained ? "final-retained" : "available") +
        " videoSourceRevision=" + std::to_string(value.video.source.revision) +
        " videoSourceStale=" + std::to_string(value.video.source.stale) +
        " videoSourceClockValid=" + std::to_string(value.video.source.clockValid) +
        " videoSourceAgeUs=" + (value.video.source.clockValid ? std::to_string(value.video.source.ageUs) : "-1");
    for (std::size_t i = 0; i < names.size(); ++i)
        text += std::string(" ") + names[i] + "{" + formatStage(value.video.distributions[i], published) + "}";
    text += " textureUpload{" + formatStage(value.presentation.uploadDistribution) + "}";
    return text;
}
inline std::string format(const Record& record,const FormatNames& names){
    const auto group=record.line==Line::Video?Capture::Video:record.line==Line::Audio?Capture::Audio:record.line==Line::Image?Capture::Image:record.line==Line::Stages?Capture::Video:Capture::Latency;
    const char* name=record.line==Line::Video?"video":record.line==Line::Audio?"audio":record.line==Line::Image?"image":record.line==Line::Stages?"stages":"latency";
    if(!includes(record.snapshot.captured,group))return std::string("mediaTelemetry group=")+name+" availability=not-captured";
    if((record.line==Line::Video&&record.snapshot.video.source.availability==Availability::NotPublished)||
        (record.line==Line::Audio&&record.snapshot.audio.source.availability==Availability::NotPublished))
        return std::string("mediaTelemetry group=")+name+" availability=not-published";
    std::string text;
    switch(record.line){
        case Line::Video:text=formatVideo(record.snapshot,names);break;
        case Line::Audio:text=formatAudio(record.snapshot);break;
        case Line::Image:text=formatImage(record.snapshot);break;
        case Line::Latency:text=formatLatency(record.snapshot);break;
        case Line::Stages:text=formatStages(record.snapshot);break;
    }
    const auto& value=record.snapshot;
    text+=" mediaInstance="+std::to_string(value.mediaInstance)+" mediaCaptureAtUs="+std::to_string(value.capturedAtUs)+
        " mediaCaptureCompletedAtUs="+std::to_string(value.captureCompletedAtUs);
    if(record.line==Line::Video||record.line==Line::Audio) {
        const auto& source=record.line==Line::Video?value.video.source:value.audio.source;
        text+=std::string(" mediaSourceAvailability=")+(source.availability==Availability::FinalRetained?"final-retained":"available")+
            " mediaSourceRevision="+std::to_string(source.revision)+" mediaSourceCapturedAtUs="+std::to_string(source.capturedAtUs)+
            " mediaSourcePublishedAtUs="+std::to_string(source.publishedAtUs)+" mediaSourceClockValid="+std::to_string(source.clockValid)+
            " mediaSourceAgeUs="+(source.clockValid?std::to_string(source.ageUs):"-1")+" mediaSourceStale="+std::to_string(source.stale)+
            " mediaSourceEpochKnown="+std::to_string(source.epochKnown)+" mediaSourceEpoch="+std::to_string(source.epoch)+
            " mediaSourceGenerationKnown="+std::to_string(source.sourceGenerationKnown)+" mediaSourceGeneration="+std::to_string(source.sourceGeneration)+
            " mediaSourceLoadGenerationKnown="+std::to_string(source.loadGenerationKnown)+" mediaSourceLoadGeneration="+std::to_string(source.loadGeneration)+
            " mediaSourceWorkerRun="+std::to_string(source.workerRun);
    }
    if(record.line==Line::Audio) {
        const auto& audio=value.audio;const bool clock=audio.queueKnown&&audio.queueObservedAtUs<=value.captureCompletedAtUs;
        text+=" audio_queue_known="+std::to_string(audio.queueKnown)+" audio_queue_clock_valid="+std::to_string(clock)+
            " audio_queue_observation_age_us="+(clock?std::to_string(value.captureCompletedAtUs-audio.queueObservedAtUs):"-1");
    }
    if(record.line==Line::Video) {
        const auto& presentation=value.presentation;
        text+=" mediaCurrentEpoch="+std::to_string(presentation.currentEpoch)+" mediaObservedSourceGeneration="+std::to_string(presentation.observedSourceGeneration)+
            " mediaCurrentLoadGeneration="+std::to_string(presentation.currentLoadGeneration)+" mediaCurrentWorkerRun="+std::to_string(presentation.workerRun)+
            " mediaResetRequested="+std::to_string(presentation.resetRequested)+" mediaResetInProgress="+std::to_string(presentation.resetInProgress)+
            " mediaWaitingForKeyframe="+std::to_string(presentation.waitingForKeyframe);
    }
    return text;
}
}
