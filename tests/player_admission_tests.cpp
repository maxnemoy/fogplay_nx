#include "player_fakes.hpp"
#include "core/constants/stage3_options.hpp"
#include <iostream>
using namespace player_test;
int main() {
    for (const bool mts:{false,true}) for (const bool accepts:{false,true}) for (const int fps:{30,60}) {
        Trace trace;TransportFactory transports(trace);MediaFactory medias(trace);Input input(trace);
        Player::Time now=Player::Time{}+1s;Player owner(transports,medias,input,[&]{return now;});
        auto client=std::make_shared<ClientState>();client->startOk=accepts;transports.next.push_back(client);
        auto media=std::make_shared<MediaState>();medias.next.push_back(media);
        media->progress.decoded=12;media->progress.uploaded=10;media->progress.presented=10;media->progress.superseded=2;
        media->progress.width=1280;media->progress.height=720;media->progress.workerGeneration=7;
        media->progress.workerDecodeUs=60000;media->progress.workerConvertUs=9000;
        media->pending=true;media->prepareAction=[&]{now+=4ms;};
        Player::Start start;start.rental={mts?fog::ProviderId::Mts:fog::ProviderId::FogHub,"fixture"};start.video.framesPerSecond=fps;
        const auto result=owner.start(start,{});
        const bool expected=accepts&&(!mts||fog::stage3::mtsMediaWorkers);
        assert(media->progress.workers==expected&&media->fps==fps);
        if(expected)assert(media->policy==(mts?Media::WorkerPolicy::Mts:Media::WorkerPolicy::FogHub));
        if(accepts) {
            assert(result==Player::StartResult::Started);owner.observe(now);assert(owner.beforeMedia({},now).media);owner.media(now);
            assert(client->observations==unsigned(mts));
            if(mts){const auto& value=client->observed;
                assert(value.workUs==4000&&value.decoderWorkUs==69000&&value.videoGeneration==7&&value.workerVideo==expected);
                assert(value.presentedFrames==10&&value.decodedFrames==12&&value.supersededFrames==2&&value.width==1280&&value.height==720);
            }
        } else assert(result==Player::StartResult::StopRequired&&client->observations==0&&client->closes==0);
        owner.closeAfterStopDispatch();
    }
    std::cout<<"Actual Player admission: both providers, accepted/rejected start, 30/60 FPS, compile-selected MTS workers, independent UI/worker AUTO costs and counters PASS\n";
}
