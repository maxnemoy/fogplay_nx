#include "features/streaming/infrastructure/player_media.hpp"
#include "features/streaming/infrastructure/media.hpp"
#include "player_fakes.hpp"
#include <iostream>
using fog::playerMediaAdapter;
int main(){
    SDL_Renderer renderer;std::string watchdog;
    fog::stream::SdlPlayerMediaFactory factory(&renderer,[&](const std::string& text){watchdog=text;});
    auto media=factory.create();assert(playerMediaAdapter.renderer==&renderer&&playerMediaAdapter.deferred&&playerMediaAdapter.codec==::stream::VideoCodec::H264);
    playerMediaAdapter.watchdog("stall");assert(watchdog=="stall");playerMediaAdapter.hevc=false;assert(!factory.supportsH265());
    player_test::Trace trace;auto state=std::make_shared<player_test::ClientState>();state->trace=&trace;player_test::Client client(state);
    media->startWorkers(client,30,fog::stream::PlayerMedia::WorkerPolicy::Mts);
    assert(media->workersRunning()&&playerMediaAdapter.mts&&playerMediaAdapter.fps==30&&playerMediaAdapter.source==&client);
    media->stopWorkers();assert(!media->workersRunning()&&!playerMediaAdapter.source);
    media->startWorkers(client,60,fog::stream::PlayerMedia::WorkerPolicy::FogHub);assert(!playerMediaAdapter.mts&&playerMediaAdapter.fps==60);
    ::stream::Packet packet;packet.firstPacketMs=123;packet.enqueueMs=456;packet.timestamp=789;
    media->video(packet);assert(playerMediaAdapter.firstMs==123&&playerMediaAdapter.enqueueMs==456);media->audio(packet);assert(playerMediaAdapter.audios==1);
    const auto slot=std::chrono::steady_clock::time_point{};assert(media->prepare(slot)&&playerMediaAdapter.slot==slot);
    playerMediaAdapter.prepare=false;assert(!media->prepare(slot));
    assert(media->draw());media->present();assert(playerMediaAdapter.draws==1&&playerMediaAdapter.presents==1&&media->presentedFrames()==9);
    const auto progress=media->progress();assert(progress.decoded==11&&progress.uploaded==10&&progress.presented==9&&progress.superseded==2&&progress.audio==20);
    assert(progress.audioUnderruns==3&&progress.audioResets==4&&progress.workerDecodeUs==100&&progress.workerConvertUs==200&&progress.workerGeneration==7);
    assert(progress.width==1280&&progress.height==720&&progress.hardware&&!progress.waiting&&progress.workers);
    assert(media->videoDiagnostics()=="video"&&media->audioDiagnostics()=="audio"&&media->imageDiagnostics()=="image"&&media->latencyDiagnostics()=="latency");
    for(int operation=0;operation<4;++operation){
        playerMediaAdapter.codecFailure=operation==0;playerMediaAdapter.resetFailure=operation==1;
        playerMediaAdapter.videoFailure=operation==2;playerMediaAdapter.prepareFailure=operation==3;
        bool translated=false;
        try{if(operation==0)media->setVideoCodec(::stream::VideoCodec::H265);if(operation==1)media->resetVideoDecoder();if(operation==2)media->video(packet);if(operation==3)media->prepare(slot);}
        catch(const fog::stream::PlayerVideoError& error){translated=true;assert(error.code()==-10-operation&&error.recoverable()==(operation!=1));}
        assert(translated);
    }
    playerMediaAdapter.error=std::make_exception_ptr(fog::VideoDecodeError(-22,true,"worker video"));playerMediaAdapter.packet=packet;playerMediaAdapter.hasPacket=true;
    fog::stream::PlayerMedia::WorkerError failure;assert(media->takeWorkerError(failure)&&failure.hasPacket&&failure.packet.timestamp==789);
    bool translated=false;try{std::rethrow_exception(failure.error);}catch(const fog::stream::PlayerVideoError& error){translated=true;assert(error.code()==-22&&error.recoverable());}assert(translated&&!media->takeWorkerError(failure));
    playerMediaAdapter.error=std::make_exception_ptr(std::logic_error("audio unexpected"));assert(media->takeWorkerError(failure));
    bool retained=false;try{std::rethrow_exception(failure.error);}catch(const std::logic_error&){retained=true;}assert(retained);
    media->stopWorkers();const auto owner=std::this_thread::get_id();media.reset();assert(playerMediaAdapter.destroyed==1&&playerMediaAdapter.destroyedOn==owner);
    unsigned typedEvents=0;
    fog::stream::SdlPlayerMediaFactory numericFactory(&renderer,[&](const fog::media::telemetry::StallEvent& event){typedEvents+=static_cast<unsigned>(event.sequence);});
    auto numeric=numericFactory.create();assert(playerMediaAdapter.typedWatchdog);
    fog::media::telemetry::StallEvent stall;stall.sequence=3;playerMediaAdapter.typedWatchdog(stall);assert(typedEvents==3);
    assert(numeric->captureTelemetry().mediaInstance==0);numeric.reset();
    std::cout<<"PASS production Media factory adapter: real bool/slot API, policy/source/lifetime, all progress/presentation mappings, sync/worker codec translation and unexpected error retention\n";
}
