#pragma once
// Included by the real Media worker harness to reuse its existing decoder/SDL
// interposition. This adds acceptance checks, not a second media implementation.
#include "features/streaming/application/media_diagnostics_formatter.hpp"
#include "features/streaming/infrastructure/media_experiments.hpp"
#include <future>
#include "core/diagnostics/diagnostic_profile.hpp"

static void numericMediaTelemetry(SDL_Renderer* renderer,const std::vector<Bytes>& units) {
    namespace mt=fog::media::telemetry;
    Client client;
    auto media=std::make_unique<fog::Media>(renderer,stream::VideoCodec::H264,true);
    struct StallCapture { std::mutex mutex;std::optional<mt::StallEvent> event; };
    const auto stallCapture=std::make_shared<StallCapture>();
    media->setVideoStallObserver([stallCapture](const mt::StallEvent& event) {
        std::lock_guard<std::mutex> lock(stallCapture->mutex);
        if(!stallCapture->event)stallCapture->event=event;
    });
    struct Release {
        ~Release(){blockSend=false;blockAudioObservation=false;gate.notify_all();}
    } release;
    const auto initial=media->captureTelemetry();
    check(!media->retainedTelemetry()&&!initial.presentation.retainedWorkerHistory,
        "unstarted media invented a final retained worker snapshot/history marker");
    check(initial.video.source.availability==mt::Availability::Available&&!initial.audio.queueKnown,
        "construction did not publish initialized numeric state or invented an audio observation");
    for(int pixel=0;pixel<AV_PIX_FMT_NB;++pixel) {
        const auto* expected=av_get_pix_fmt_name(static_cast<AVPixelFormat>(pixel));
        check(std::strcmp(mt::processFormatNames().pixel(pixel),expected?expected:"none")==0,"fixed SDK pixel name dictionary changed a legacy name");
    }
    media->startWorkers(client,60);const auto baseline=media->captureTelemetry();
    blockSend=true;enteredSend=false;client.push(units[0],false,5,0);
    until([]{return enteredSend.load();});
    blockAudioObservation=true;enteredAudioObservation=false;
    until([]{return enteredAudioObservation.load();});
    const auto blocked=media->captureTelemetry();
    const auto queries=applicationQueueObservations.load(),names=pixelNameQueries.load();
    videoPublications=0;audioPublications=0;
    const auto began=std::chrono::steady_clock::now();
    for(unsigned i=0;i<100;++i) {
        const auto value=media->captureTelemetry();
        check(value.video.source.revision==blocked.video.source.revision&&value.audio.source.revision==blocked.audio.source.revision,
            "blocked producer cache read changed a publication revision");
        check(value.presentation.queue.depth==0&&value.presentation.queue.taken==0,"cache read consumed a ready queue entry");
    }
    check(std::chrono::steady_clock::now()-began<100ms,"numeric capture waited for blocked decoder/audio owner");
    check(applicationQueueObservations==queries&&pixelNameQueries==names&&videoPublications==0&&audioPublications==0,
        "numeric cache capture queried SDL/FFmpeg or formatted worker strings");
    auto legacyAudio=std::async(std::launch::async,[&]{return media->audioDiagnostics();});
    check(legacyAudio.wait_for(100ms)==std::future_status::ready&&!legacyAudio.get().empty(),"legacy worker audio facade waited on blocked SDL owner");
    std::this_thread::sleep_for(2100ms);
    {
        std::lock_guard<std::mutex> lock(stallCapture->mutex);
        check(stallCapture->event&&stallCapture->event->kind==mt::StallKind::Stall&&
            stallCapture->event->stage==mt::StallStage::DecodeSend&&stallCapture->event->elapsedMs>=500&&
            stallCapture->event->metadata.bytes==units[0].size(),"actual numeric observer did not report blocked decoder metadata");
    }
    const auto stale=media->captureTelemetry();
    check(stale.video.source.stale&&stale.audio.source.stale&&stale.video.source.clockValid&&stale.audio.source.clockValid,
        "blocked worker snapshots were presented as fresh or unclocked");
    blockAudioObservation=false;gate.notify_all();
    until([&]{return media->captureTelemetry().audio.source.revision>stale.audio.source.revision;});
    check(media->captureTelemetry().video.source.stale&&!media->captureTelemetry().audio.source.stale,
        "independent audio owner could not publish while video stayed blocked");
    media->resetVideoDecoder();const auto requested=media->captureTelemetry();
    check(requested.presentation.currentEpoch>requested.video.source.epoch&&requested.presentation.resetRequested&&requested.presentation.waitingForKeyframe&&requested.video.source.revision==stale.video.source.revision&&
        !media->preparePresentation(),"reset relabeled old decoder cache/output as the requested epoch");
    blockSend=false;gate.notify_all();
    until([&]{return media->captureTelemetry().video.source.epoch==requested.presentation.currentEpoch;});
    const auto before=media->videoFrames();client.push(units[0],false,7,9000);
    until([&]{return media->videoFrames()>before&&media->workerVideoGeneration()==7;});
    until([&]{return media->captureTelemetry().video.source.sourceGeneration==7;});
    auto ready=media->captureTelemetry();
    check(ready.video.source.sourceGenerationKnown&&ready.video.source.loadGenerationKnown&&ready.presentation.queue.depth<=FOGPLAY_MEDIA_READY_DEPTH&&
        ready.presentation.queue.limit==FOGPLAY_MEDIA_READY_DEPTH,"typed generation/queue mode did not describe the actual ready queue");
    until([&]{return media->preparePresentation();});check(media->draw(),"typed capture test failed real texture draw");media->present();
    const auto visible=media->captureTelemetry();
    check(visible.presentation.uploaded==1&&visible.presentation.presented==1&&visible.image.uploadedGeneration==1&&
        visible.image.presentedGeneration==1&&visible.latency.stages[0].samples==1,"typed snapshot conflated decode/upload/draw/completed present");
    if constexpr(fog::metrics::collects(fog::metrics::DiagnosticClass::StageDistribution)) {
        check(visible.video.distributions[0].availability==mt::StageAvailability::Available&&
            visible.video.distributions[0].samples>0&&visible.video.distributions[0].p99Us>=2000000&&
            visible.video.distributions[1].samples>0&&visible.video.distributions[3].samples>0&&
            visible.video.distributions[2].samples==0&&visible.presentation.uploadDistribution.samples==1,
            "Deep distributions did not describe actual decoder/scale/upload owners or invented hardware-transfer samples");
    } else {
        for(const auto& distribution:visible.video.distributions)
            check(distribution.availability==mt::StageAvailability::NotCollected&&distribution.samples==0,
                "non-Deep worker retained a distribution ring or reported measured zeros");
        check(visible.presentation.uploadDistribution.availability==mt::StageAvailability::NotCollected,
            "non-Deep upload distribution was not explicitly omitted");
    }
    const auto video=media->captureTelemetry(mt::Capture::Video);
    const auto facade=media->diagnostics();const auto imageAt=facade.find(" image_source=");
    check(imageAt!=std::string::npos&&facade.substr(0,imageAt)==mt::formatVideo(video,mt::processFormatNames()),
        "legacy video facade did not format the same numeric counters/queue values");
    media->stopWorkers();const auto final=media->captureTelemetry();
    check(final.video.source.availability==mt::Availability::FinalRetained&&final.audio.source.availability==mt::Availability::FinalRetained&&
        !final.video.source.stale&&!final.audio.source.stale&&final.presentation.queue.depth==0&&final.presentation.uploaded==1,
        "stop did not retain final worker telemetry and the visible texture separately");
    const auto savedFinal=media->retainedTelemetry();
    check(savedFinal&&savedFinal->presentation.workerRun==final.presentation.workerRun&&savedFinal->presentation.presented==1&&
        !savedFinal->presentation.workers&&savedFinal->presentation.retainedWorkerHistory&&savedFinal->presentation.configuredFps==60&&
        savedFinal->presentation.queue.limit==FOGPLAY_MEDIA_READY_DEPTH&&savedFinal->presentation.queue.taken==visible.presentation.queue.taken&&
        savedFinal->presentation.videoQueueWait.calls==visible.presentation.videoQueueWait.calls,
        "joined stop did not retain its complete UI/worker final handoff");
    check(!final.presentation.retainedWorkerHistory&&final.presentation.configuredFps==0,
        "current stopped synchronous snapshot inherited retained worker configuration");
    const auto finalVideo=mt::formatVideo(*savedFinal,mt::processFormatNames());
    const auto history=" async_workers=0 presentation_queue=0 presentation_queue_limit="+std::to_string(FOGPLAY_MEDIA_READY_DEPTH)+
        " presentation_fps=60 video_worker_queue_samples="+std::to_string(savedFinal->presentation.videoQueueWait.calls);
    check(finalVideo.find(history)!=std::string::npos&&finalVideo.find(" taken="+std::to_string(savedFinal->presentation.queue.taken))!=std::string::npos&&
        finalVideo.find(" video_worker_queue_wait_avg_us="+std::to_string(mt::average(savedFinal->presentation.videoQueueWait)))!=std::string::npos&&
        finalVideo.find(" video_worker_queue_wait_max_us="+std::to_string(savedFinal->presentation.videoQueueWait.maxUs))!=std::string::npos&&
        finalVideo.find(" resets="+std::to_string(savedFinal->presentation.queue.resets))!=std::string::npos,
        "retained 60fps final dropped worker queue/wait/configuration while workers were stopped");
    media->audioDiagnostics();media->audioUnderruns();
    check(media->retainedTelemetry()->capturedAtUs==savedFinal->capturedAtUs&&
        media->retainedTelemetry()->audio.source.availability==mt::Availability::FinalRetained,
        "legacy sync audio observations replaced the previous run's final record");
    const mt::Record retained(*savedFinal,mt::Line::Video);const auto finalText=mt::format(retained,mt::processFormatNames());
    media->startWorkers(client,30);const auto restarted=media->captureTelemetry();
    check(!restarted.presentation.retainedWorkerHistory&&restarted.presentation.workerRun==baseline.presentation.workerRun+1&&restarted.video.source.availability==mt::Availability::Available&&
        retained.snapshot.video.source.availability==mt::Availability::FinalRetained&&retained.snapshot.presentation.workerRun==baseline.presentation.workerRun,
        "restart changed an immutable final record or reused the old worker-run label");
    check(restarted.video.source.workerRun==baseline.presentation.workerRun&&
        restarted.video.source.loadGeneration<restarted.presentation.currentLoadGeneration,
        "new worker admission relabeled old decoder observations as newly completed work");
    check(media->retainedTelemetry()->presentation.workerRun==baseline.presentation.workerRun,"new start relabeled the retained final snapshot");
    media->resetVideoDecoder();
    until([&]{const auto reset=media->captureTelemetry();return reset.video.source.epoch==reset.presentation.currentEpoch;});
    check(!media->captureTelemetry().video.source.sourceGenerationKnown&&media->captureTelemetry().video.source.workerRun==restarted.presentation.workerRun,
        "reset before any new-run packet inherited the previous run's source generation");
    media->stopWorkers();
    const auto restartedFinal=media->retainedTelemetry();
    check(restartedFinal&&!restartedFinal->presentation.workers&&restartedFinal->presentation.retainedWorkerHistory&&
        restartedFinal->presentation.configuredFps==30&&restartedFinal->presentation.workerRun==restarted.presentation.workerRun&&
        mt::formatVideo(*restartedFinal,mt::processFormatNames()).find(" presentation_fps=30 video_worker_queue_samples=")!=std::string::npos&&
        mt::format(retained,mt::processFormatNames())==finalText,
        "30fps final/restart rewrote the frozen previous 60fps worker record or lost its own configuration");
    check(!media->captureTelemetry().presentation.retainedWorkerHistory,
        "new synchronous capture inherited the restarted run's frozen history marker");
    media.reset();const auto namesAfter=pixelNameQueries.load(),sdlAfter=applicationQueueObservations.load();
    auto next=std::make_unique<fog::Media>(renderer,stream::VideoCodec::H264,true);
    check(next->captureTelemetry().mediaInstance!=retained.snapshot.mediaInstance,"new Media object reused a retained record's instance label");
    auto formatted=std::async(std::launch::async,[retained]{return mt::format(retained,mt::processFormatNames());});
    check(formatted.get()==finalText&&pixelNameQueries==namesAfter&&applicationQueueObservations==sdlAfter,
        "formatting retained values after Media destruction queried SDK/SDL or followed owner state");
    check(finalText.size()<mt::MaximumFormattedBytes,"media record exceeded its declared rendered budget");
    std::cout<<"Media typed owners PASS: real decoder/audio blocked, cached no-query capture, stale independent groups, epoch/reset/queue depth="<<FOGPLAY_MEDIA_READY_DEPTH
        <<", upload/present, final/restart, immutable format after destruction; Snapshot="<<sizeof(mt::Snapshot)<<" Record="<<sizeof(mt::Record)<<'\n';
}
