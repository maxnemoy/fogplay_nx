#include "features/streaming/application/media_diagnostics_formatter.hpp"
#include "features/streaming/infrastructure/video_watchdog.hpp"
#include "core/diagnostics/stage_distribution.hpp"
#include <cassert>
#include <future>
#include <iostream>
#include <limits>

namespace mt=fog::media::telemetry;
using Distribution=fog::metrics::StageDistribution<>;
using Availability=fog::metrics::DistributionAvailability;
static void distributions() {
    Distribution recorder;
    const auto zero=recorder.snapshot(1000000);
    constexpr bool enabled=fog::metrics::collects(fog::metrics::DiagnosticClass::StageDistribution);
    if constexpr(enabled) {
        assert(zero.availability==Availability::Available&&zero.samples==0);
        static_assert(!enabled || sizeof(Distribution)>8000);
    } else {
        assert(zero.availability==Availability::NotCollected&&zero.samples==0);
        static_assert(sizeof(fog::metrics::StageDistribution<false>)==1);
    }
    for(unsigned n=1;n<=512;++n)recorder.record(n*10,1000000+n);
    recorder.record(10000,1000513);
    const auto current=recorder.snapshot(1000513);
    if constexpr(enabled) {
        assert(current.samples==512&&current.overflowLifetime==1);
        assert(current.p50Us==2570&&current.p95Us==4880&&current.p99Us==5080);
        assert(recorder.snapshot(6000512).samples==1);
        assert(recorder.snapshot(6000513).samples==0);
    } else assert(current.availability==Availability::NotCollected);
    assert(recorder.snapshot(1000513,false).availability==Availability::InstrumentationDisabled);
    mt::Snapshot snapshot;
    snapshot.video.source.availability=mt::Availability::Available;
    const mt::StageDistributionValue zeroValue{enabled?mt::StageAvailability::Available:mt::StageAvailability::NotCollected,1000000,0,0,0,0,0};
    snapshot.video.distributions.fill(zeroValue);snapshot.presentation.uploadDistribution=zeroValue;
    const auto text=mt::formatStages(snapshot);
    assert(text.find("scope=rolling5s windowUs=5000000 capacity=512 quantile=exact-nearest-rank boundary=sdk-call-return")!=std::string::npos);
    for(const char* name:{"decodeSend","decodeReceive","hardwareTransfer","pixelScale","textureUpload"})
        assert(text.find(std::string(name)+"{availability="+(enabled?"available":"not-collected"))!=std::string::npos);
    if constexpr(enabled) assert(text.find("samples=0 p50Us=0 p95Us=0 p99Us=0 overflowLifetime=0")!=std::string::npos);
    else assert(text.find("samples=")==std::string::npos);
    snapshot.video.source.availability=mt::Availability::NotPublished;
    assert(mt::formatStages(snapshot).find("decodeSend{availability=not-published}")!=std::string::npos);
    const auto max=std::numeric_limits<std::uint64_t>::max();
    auto full=zeroValue;full.availability=mt::StageAvailability::Available;
    full.capturedAtSteadyUs=full.samples=full.p50Us=full.p95Us=full.p99Us=full.overflowLifetime=max;
    snapshot.video.source.availability=mt::Availability::Available;
    snapshot.video.distributions.fill(full);snapshot.presentation.uploadDistribution=full;
    assert(mt::formatStages(snapshot).size()<4096);
}
static void stalls() {
    using Watchdog=fog::detail::VideoWatchdog;
    using Flight=fog::detail::VideoFlight;
    mt::StallEvent frozen;
    {
        Watchdog producer;
        producer.decoder.begin(Flight::Stage::DecodeReceive,{1280,720,1,2,1280,12345,96,321},1000);
        bool called=false;
        producer.inspectNumeric(1499,[&](const mt::StallEvent&){called=true;});
        assert(!called);
        producer.inspectNumeric(1500,[&](const mt::StallEvent& event){frozen=event;called=true;});
        assert(called&&frozen.kind==mt::StallKind::Stall&&frozen.lane==mt::StallLane::Decoder&&frozen.error==mt::StallError::None);
        assert(frozen.sequence==1&&frozen.monotonicMs==1500&&frozen.elapsedMs==500);
        producer.decoder.end();
    }
    const std::string golden="stall lane=decoder stage=decode-receive sequence=1 monotonicMs=1500 elapsedMs=500 width=1280 height=720 codec=1 format=2 stride=1280 bytes=12345 nalTypes=96 frame=321";
    auto format=std::async(std::launch::async,[frozen]{return mt::formatStall(frozen);});
    assert(format.get()==golden);
    mt::StallEvent unavailable;unavailable.kind=mt::StallKind::ObserverUnavailable;unavailable.error=mt::StallError::ObserverThreadStart;
    assert(mt::formatStall(unavailable)=="observer unavailable; video stall diagnostics disabled");
}
int main(){distributions();stalls();std::cout<<"Media stages PASS profile="<<FOGPLAY_DIAGNOSTIC_PROFILE<<" recorder="<<sizeof(Distribution)
    <<" Snapshot="<<sizeof(mt::Snapshot)<<" Record="<<sizeof(mt::Record)<<" StallEvent="<<sizeof(mt::StallEvent)<<'\n';}
