#include "features/streaming/application/media_diagnostics_formatter.hpp"
#include "features/streaming/infrastructure/media_telemetry_cache.hpp"
#include "features/streaming/infrastructure/video_latency.hpp"
#include <atomic>
#include <cassert>
#include <iostream>
#include <limits>
#include <thread>

namespace mt=fog::media::telemetry;
static void videoAndImage(){
    mt::FormatNames names;assert(names.setPixel(42,"pix")&&names.setPixel(43,"frame")&&names.setPixel(44,"cpu"));
    assert(!names.setPixel(512,"outside")&&!names.setPixel(41,"012345678901234567890123456789012")&&std::string(names.pixel(-1))=="none"&&std::string(names.pixel(512))=="none");
    mt::Snapshot value;value.captured=mt::Capture::Video;value.capturedAtUs=1000000;
    auto& v=value.video;v.codec=mt::Codec::H264;v.codecId=27;v.pixelFormat=42;v.framePixelFormat=43;v.cpuPixelFormat=44;
    v.source.availability=mt::Availability::Available;
    v.colorspace=2;v.range=1;v.trc=3;v.primaries=4;v.threads=v.threadRequested=v.threadActive=2;
    v.stages={mt::Timing{3,30,11},mt::Timing{2,51,30},mt::Timing{1,4,4},mt::Timing{2,20,15}};
    v.hardware={7,0xa,2,3,4,0xb,-5,true,true};v.pending=true;v.pendingSinceUs=990000;v.pendingAgeUs=123;
    auto& p=value.presentation;p.upload={5,255,100};p.decoded=20;p.uploaded=10;p.presented=9;p.superseded=4;
    const std::string expected="decoder codec=h264 codec_id=27 pixfmt=pix frame_pixfmt=frame cpu_pixfmt=cpu frame_colorspace=2 frame_color_range=1 frame_color_trc=3 frame_color_primaries=4 threads=2 thread_requested=2 thread_active=2 "
        "send_calls=3 send_avg_us=10 send_max_us=11 receive_calls=2 receive_avg_us=25 receive_max_us=30 transfer_calls=1 transfer_avg_us=4 transfer_max_us=4 scale_calls=2 scale_avg_us=10 scale_max_us=15 upload_calls=5 upload_avg_us=51 upload_max_us=100 "
        "decoded_frames=20 uploaded_frames=10 presented_frames=9 superseded_frames=4 pending_frames=1 pending_frame_age_us=10000 nv_events=7 nv_status=1 nv_fresh=1 nv_error=0x0000000a nv_mbs_error=2 nv_mbs_ok=3 nv_cycles_raw=4 nv_header_error=0x0000000b nv_wait_code=-5";
    assert(mt::formatDecoder(value,names)==expected);
    assert(mt::formatVideo(value,names)==expected+" async_workers=0 presentation_queue_limit=1 presentation_fps=0");
    p.workers=true;p.configuredFps=60;p.videoQueueWait={3,30,20};p.queue.depth=2;p.queue.limit=3;p.queue.framesPerSecond=60;
    auto workerExpected=expected;const auto age=workerExpected.find("pending_frame_age_us=10000");workerExpected.replace(age,std::strlen("pending_frame_age_us=10000"),"pending_frame_age_us=123");
    assert(mt::formatVideo(value,names)==workerExpected+" async_workers=1 presentation_queue=2 presentation_queue_limit=3 presentation_fps=60 video_worker_queue_samples=3 video_worker_queue_wait_avg_us=10 video_worker_queue_wait_max_us=20 displayPaced=0 queueFps=60 readyEmpty=0 cadenceBlocked=0 rtpBlocked=0 pushCapacityDrop=0 takeStaleDrop=0 timedLateCoalesce=0 taken=0 resets=0");
    p.workers=false;p.retainedWorkerHistory=true;
    const auto retainedExpected=workerExpected+" async_workers=0 presentation_queue=2 presentation_queue_limit=3 presentation_fps=60 video_worker_queue_samples=3 video_worker_queue_wait_avg_us=10 video_worker_queue_wait_max_us=20 displayPaced=0 queueFps=60 readyEmpty=0 cadenceBlocked=0 rtpBlocked=0 pushCapacityDrop=0 takeStaleDrop=0 timedLateCoalesce=0 taken=0 resets=0";
    assert(mt::formatVideo(value,names)==retainedExpected);
    p.retainedWorkerHistory=false;p.workers=true;
    const std::string image="image_source=cpu_iyuv_grid image_grid_max=576 image_samples=0 image_points=0 image_y_min=255 image_y_max=0 image_y_mean=0 image_dark_points=0 image_chroma_deviation=0 image_near_black=0 image_black_frames=0 image_black_runs=0 image_black_streak=0 image_black_streak_max=0 image_unchanged_streak=0 image_unchanged_streak_max=0 image_texture=0 image_width=0 image_height=0 image_uploaded_generation=0 image_drawn_generation=0 image_presented_generation=0 image_upload_age_ms=-1 image_draw_age_ms=-1 image_new_present_age_ms=-1 image_draws=0 image_empty_draws=0 image_create_errors=0 image_upload_errors=0 image_clear_errors=0 image_copy_errors=0 image_presented_black=0 image_presented_black_frames=0 image_sample_avg_us=0 image_sample_max_us=0 mediaNv12=0 mediaNv12Available=0 mediaNv12Fallbacks=0 mediaCpuPool=0 mediaTextures=1 mediaReadyDepth=3";
    assert(mt::formatImage(value)==image);
    const mt::Record owned(value,mt::Line::Video);value.presentation.uploaded=999;assert(mt::format(owned,names).find("uploaded_frames=10")!=std::string::npos);
    assert(mt::format(owned,names).find("mediaSourceAgeUs=-1")!=std::string::npos);
    assert(mt::format(owned,names).size()<mt::MaximumFormattedBytes);
}
static void audio(){
    mt::Snapshot value;auto& a=value.audio;
    a.packets=3;a.queuedBytes=3840;a.maximumBytes=38400;a.emptyObservations=1;a.prolongedEmptyObservations=2;
    a.resets=4;a.discardedBytes=1920;a.queueWait={2,5000,4000};a.pcmDiscardedPackets=5;a.pcmDiscardedFrames=960;
    a.stalePackets=6;a.overflowPackets=7;a.catchups=8;a.badPackets=9;a.decoderErrors=10;a.outputErrors=11;
    a.recoveryAttempts=12;a.recoveryFailures=13;a.recoveryDiscardedPackets=14;a.recoveryPending=true;
    a.lastFloorBytes=3840;a.maximumFloorBytes=7680;a.floorWindows=15;a.elevatedFloorWindows=16;
    assert(mt::formatAudio(value)=="audio_packets=3 audio_queued_ms=20 audio_max_queued_ms=200 audio_empty_observations=1 audio_prolonged_empty_observations=2 audio_empty_threshold_ms=40 audio_queue_resets=4 audio_reset_discarded_ms=10 audio_worker_queue_samples=2 audio_worker_queue_wait_avg_us=2500 audio_worker_queue_wait_max_us=4000 audio_pcm_discarded_packets=5 audio_pcm_discarded_ms=20 audio_stale_packets=6 audio_overflow_packets=7 audio_catchups=8 audio_worker_refill_ms=40 audio_worker_max_ms=80 audio_worker_stale_ms=60 audio_bad_packets=9 audio_decoder_errors=10 audio_output_errors=11 audio_recovery_attempts=12 audio_recovery_failures=13 audio_recovery_discarded_packets=14 audio_recovery_pending=1 audio_recovery_limit=3/60s audio_recovery_deadline_ms=10000 audio_sync_bounded=0 audio_floor_last_ms=20 audio_floor_max_ms=40 audio_floor_windows=15 audio_floor_elevated_windows=16 audio_floor_window_ms=10000 audio_floor_elevated_ms=100 audio_worker_bounded=0 audio_pcm_decoded_frames=0 audio_pcm_admitted_frames=0 audio_pcm_failed_frames=0 audio_pcm_suppressed_frames=960 audio_pcm_first_at_us=0 audio_pcm_last_at_us=0 audio_pcm_elapsed_us=0 audio_output_format_source=sdl-obtained-stream audio_output_rate=0 audio_output_channels=0 audio_output_bytes_per_frame=0 audio_latency_clears=0 audio_recovery_clears=0 audio_clear_reason=none audio_clear_before_bytes=0 audio_clear_after_bytes=0 audio_clear_at_us=0 audio_queue_observed_at_us=0");
}
static void latencyAndAvailability(){
    fog::detail::VideoLatencyMetrics metrics;metrics.presented({10,5000,true},100000);metrics.presented({10,9000,true},200000);
    const auto source=metrics.snapshot(200000,60);mt::Snapshot value;auto& l=value.latency;
    for(std::size_t i=0;i<3;++i){const auto& h=source.stages[i];l.stages[i]={h.samples,h.p50Us,h.p95Us,h.p99Us,h.overflow};}
    l.lateThresholdUs=source.lateThresholdUs;l.lateCount=source.lateCount;l.latePpm=source.latePpm;l.unavailableAge=source.unavailableAge;l.unavailableWait=source.unavailableWait;
    assert(mt::formatLatency(value)==metrics.diagnostics(200000,60)+" originMapCapacity=64 originMissesLifetime=0");
    assert(source.stages[0].samples==2&&source.stages[0].p50Us==90000&&source.stages[0].p95Us==190000&&source.lateCount==1&&source.latePpm==1000000);
    assert(metrics.snapshot(5200000,60).stages[0].samples==0);
    mt::Source age;mt::sourceAge(age,100);assert(!age.clockValid&&!age.stale);
    age.availability=mt::Availability::Available;age.capturedAtUs=100;age.publishedAtUs=110;
    mt::sourceAge(age,99);assert(!age.clockValid&&age.ageUs==0);
    mt::sourceAge(age,2000099);assert(age.clockValid&&!age.stale);
    mt::sourceAge(age,2000100);assert(age.stale&&age.ageUs==2000000);
    age.availability=mt::Availability::FinalRetained;mt::sourceAge(age,9000000);assert(!age.stale&&age.clockValid);
    mt::FormatNames names;mt::Snapshot missing;
    assert(mt::format(mt::Record(missing,mt::Line::Audio),names)=="mediaTelemetry group=audio availability=not-published");
    missing.captured=mt::Capture::Video;
    assert(mt::format(mt::Record(missing,mt::Line::Latency),names)=="mediaTelemetry group=latency availability=not-captured");
}
static void cacheCopies(){
    fog::detail::MediaTelemetryCache cache;std::atomic<unsigned> done{0};
    std::thread video([&]{for(std::uint64_t i=1;i<=30000;++i){mt::VideoOwner value;value.source.revision=i;value.source.epoch=i^0x55;value.stages[0]={i,i*3,i*5};cache.publish(value);}++done;});
    std::thread audio([&]{for(std::uint64_t i=1;i<=30000;++i){mt::AudioOwner value;value.source.revision=i;value.packets=i*7;value.lastFloorBytes=i*11;cache.publish(value);}++done;});
    std::thread final([&]{for(std::uint64_t i=1;i<=30000;++i){mt::Snapshot value;value.mediaInstance=i;value.capturedAtUs=i*17;value.presentation.uploaded=i*13;cache.retainFinal(value);}++done;});
    do{const auto value=cache.snapshot();const auto v=value.video.source.revision,a=value.audio.source.revision;
        if(v)assert(value.video.source.epoch==(v^0x55)&&value.video.stages[0].calls==v&&value.video.stages[0].totalUs==v*3&&value.video.stages[0].maxUs==v*5);
        if(a)assert(value.audio.packets==a*7&&value.audio.lastFloorBytes==a*11);
        if(const auto retained=cache.retainedFinal())assert(retained->capturedAtUs==retained->mediaInstance*17&&retained->presentation.uploaded==retained->mediaInstance*13);
    }while(done.load()!=3);
    video.join();audio.join();final.join();const auto owned=cache.snapshot();cache.publish(mt::VideoOwner{});assert(owned.video.source.revision==30000&&owned.audio.packets==210000);
    assert(cache.retainedFinal()->mediaInstance==30000);
}
static void namePublication(){
    mt::FormatNames names(false);std::atomic<bool> done{false};
    std::thread publisher([&]{for(std::size_t i=0;i<mt::FormatNames::PixelCapacity;++i)assert(names.setPixel(i,"fixed_sdk_name"));names.publish();done=true;});
    do{if(std::strcmp(names.pixel(0),"none")!=0)for(std::size_t i=0;i<mt::FormatNames::PixelCapacity;++i)assert(std::strcmp(names.pixel(static_cast<int>(i)),"fixed_sdk_name")==0);}while(!done);
    publisher.join();assert(std::strcmp(names.pixel(511),"fixed_sdk_name")==0);
}
static void boundedRendering(){
    mt::Snapshot value;mt::FormatNames names;names.setPixel(0,"0123456789012345678901234567890");
    const auto maximum=std::numeric_limits<std::uint64_t>::max();auto& v=value.video;auto& p=value.presentation;auto& i=value.image;auto& a=value.audio;
    v.codec=mt::Codec::Hevc;v.pixelFormat=v.framePixelFormat=v.cpuPixelFormat=0;v.pending=true;v.pendingAgeUs=maximum;
    for(auto& stage:v.stages)stage={1,maximum,maximum};
    p.upload=p.videoQueueWait={1,maximum,maximum};p.workers=true;
    for(auto* counter:{&p.decoded,&p.uploaded,&p.presented,&p.superseded,&p.queue.depth,&p.queue.limit,&p.queue.readyEmpty,&p.queue.cadenceBlocked,
        &p.queue.rtpBlocked,&p.queue.capacityDrop,&p.queue.staleDrop,&p.queue.lateCoalesce,&p.queue.taken,&p.queue.resets})*counter=maximum;
    for(auto* counter:{&i.samples,&i.blackFrames,&i.blackRuns,&i.blackStreak,&i.blackStreakMax,&i.unchangedStreak,&i.unchangedStreakMax,
        &i.uploadedGeneration,&i.drawnGeneration,&i.presentedGeneration,&i.draws,&i.emptyDraws,&i.createErrors,&i.uploadErrors,&i.clearErrors,
        &i.copyErrors,&i.presentedBlackFrames,&i.nv12Fallbacks})*counter=maximum;
    i.sample={1,maximum,maximum};i.uploadAgeMs=i.drawAgeMs=i.newPresentAgeMs=std::numeric_limits<std::int64_t>::min();
    for(auto* counter:{&a.packets,&a.queuedBytes,&a.maximumBytes,&a.emptyObservations,&a.prolongedEmptyObservations,&a.resets,&a.discardedBytes,
        &a.pcmDiscardedPackets,&a.pcmDiscardedFrames,&a.stalePackets,&a.overflowPackets,&a.catchups,&a.badPackets,&a.decoderErrors,&a.outputErrors,
        &a.recoveryAttempts,&a.recoveryFailures,&a.recoveryDiscardedPackets,&a.lastFloorBytes,&a.maximumFloorBytes,&a.floorWindows,&a.elevatedFloorWindows})*counter=maximum;
    for(auto* counter:{&a.decodedPcmFrames,&a.admittedPcmFrames,&a.failedPcmFrames,&a.firstPcmAtUs,&a.lastPcmAtUs,
        &a.latencyClears,&a.recoveryClears,&a.clearBeforeBytes,&a.clearAfterBytes,&a.clearAtUs,&a.queueObservedAtUs})*counter=maximum;
    a.outputSampleRate=a.outputChannels=a.outputBytesPerFrame=std::numeric_limits<std::uint32_t>::max();
    a.lastClearReason=mt::AudioClearReason::Recovery;
    a.queueWait={1,maximum,maximum};
    for(auto& stage:value.latency.stages)stage={maximum,maximum,maximum,maximum,maximum};
    value.latency.lateThresholdUs=value.latency.lateCount=value.latency.latePpm=value.latency.unavailableAge=value.latency.unavailableWait=value.latency.originMisses=maximum;
    const auto video=mt::formatVideo(value,names),audio=mt::formatAudio(value),latency=mt::formatLatency(value);
    assert(video.size()<mt::MaximumFormattedBytes&&audio.size()<mt::MaximumFormattedBytes&&latency.size()<mt::MaximumFormattedBytes);
    assert(video.find("mediaReadyDepth=3")!=std::string::npos&&audio.find("audio_floor_elevated_ms=100")!=std::string::npos&&latency.find("originMissesLifetime=")!=std::string::npos);
    assert(mt::RecordQueueCharge==sizeof(mt::Record)+mt::MaximumFormattedBytes&&mt::RecordQueueCharge<=16*1024);
    value.video.source.availability=value.audio.source.availability=mt::Availability::Available;
    value.capturedAtUs=value.captureCompletedAtUs=value.mediaInstance=maximum;
    for(auto* source:{&value.video.source,&value.audio.source}) {
        source->revision=source->capturedAtUs=source->publishedAtUs=source->epoch=source->sourceGeneration=source->loadGeneration=source->workerRun=source->ageUs=maximum;
        source->clockValid=source->epochKnown=source->sourceGenerationKnown=source->loadGenerationKnown=true;
    }
    assert(mt::format(mt::Record(value,mt::Line::Video),names).size()<mt::MaximumFormattedBytes);
    assert(mt::format(mt::Record(value,mt::Line::Audio),names).size()<mt::MaximumFormattedBytes);
}
int main(){videoAndImage();audio();latencyAndAvailability();cacheCopies();namePublication();boundedRendering();
    std::cout<<"Media numeric format/cache: exact legacy video/audio/image/latency keys+units, rolling cutoff, stale/final/clock flags, bounded names, immutable record and concurrent value-copy publication PASS; Snapshot="<<sizeof(mt::Snapshot)<<" Record="<<sizeof(mt::Record)<<'\n';
}
