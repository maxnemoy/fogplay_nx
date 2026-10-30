#include "features/streaming/domain/playback_drain.hpp"
#include <algorithm>
#include <cstdint>
#include <deque>
#include <iostream>
#include <stdexcept>
#include <vector>

using Budget = fog::stream::PlaybackDrainBudget;
using Policy = fog::stream::PlaybackDrainPolicy;
static void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct Packet { unsigned id; bool video; std::uint64_t costUs; };
struct Result { std::vector<unsigned> order; unsigned cycles=0, audio=0, maxVideo=0; };
static Result drain(std::deque<Packet> packets, Policy policy = Policy::PresentationFirst, std::uint64_t oldestMs = 0) {
    Result result;
    while (!packets.empty()) {
        const auto queuedVideo = std::count_if(packets.begin(), packets.end(), [](const auto& p) { return p.video; });
        Budget budget(queuedVideo,oldestMs,policy);
        std::uint64_t elapsed=0;
        while (!packets.empty() && budget.yield(elapsed) == Budget::Yield::None) {
            const auto packet=packets.front(); packets.pop_front();
            budget.consumed(packet.video); elapsed += packet.costUs;
            result.order.push_back(packet.id);
            if (!packet.video) ++result.audio;
        }
        result.maxVideo=std::max(result.maxVideo,static_cast<unsigned>(budget.video()));
        ++result.cycles;
        check(result.cycles <= 100, "backlog stopped progressing");
    }
    return result;
}
// A 60-FPS source delivered in pairs on alternate UI turns. Immediate P4
// decodes both references but presents only one, then has nothing to present.
// The guarded policy must spread fresh frames across the two turns.
static unsigned pairedArrivalPresentations(Policy policy) {
    std::deque<std::uint64_t> arrivals;
    unsigned presented = 0, decoded = 0;
    for (unsigned turn = 0; turn < 120; ++turn) {
        const std::uint64_t nowMs = turn * 1000u / 60;
        if (turn % 2 == 0) { arrivals.push_back(nowMs); arrivals.push_back(nowMs); }
        Budget budget(arrivals.size(), arrivals.empty() ? 0 : nowMs - arrivals.front(), policy, 60);
        while (!arrivals.empty() && budget.yield(budget.video() * 1000) == Budget::Yield::None) {
            arrivals.pop_front(); budget.consumed(true); ++decoded;
        }
        if (budget.video()) ++presented;
        check(arrivals.size() <= 1, "paired arrivals accumulated a permanent queue");
    }
    check(arrivals.empty() && decoded == 120, "paired arrival simulation lost compressed frames");
    return presented;
}
int main() {
    Budget normal(1);
    check(!normal.catchup() && normal.yield(100000) == Budget::Yield::None, "one atomic operation must always progress");
    normal.consumed(true);
    check(normal.yield(1000) == Budget::Yield::Video, "normal frame must reach presentation before next decode");
    check(normal.yield(8000) == Budget::Yield::Time, "normal work exceeded its budget");
    Budget youngBurst(12,0);
    check(youngBurst.catchup(), "fresh backlog must catch up without an age threshold");
    youngBurst.consumed(true);
    check(youngBurst.yield(5000) == Budget::Yield::None, "fresh burst failed to drain its second reference");
    youngBurst.consumed(true);
    check(youngBurst.yield(10000) == Budget::Yield::None, "large backlog failed to drain its third reference");
    youngBurst.consumed(true);
    check(youngBurst.yield(11000) == Budget::Yield::Video, "large backlog exceeded three video frames");
    Budget shallow(2,0);
    check(!shallow.catchup(), "fresh MTS pair must retain separate presentation turns");
    shallow.consumed(true);
    check(shallow.yield(0) == Budget::Yield::Video, "fresh MTS pair bypassed presentation");
    for (int fps : {30,60}) {
        const auto threshold = fps == 30 ? 67u : 34u;
        check(Budget::pairAgeThresholdMs(fps) == threshold, "wrong frame-rate-specific age threshold");
        Budget justYoung(2,threshold-1,Policy::PresentationFirst,fps);
        check(!justYoung.catchup(), "MTS pair caught up before two frame intervals");
        Budget justAged(2,threshold,Policy::PresentationFirst,fps);
        check(justAged.catchup(), "aged MTS pair failed to catch up");
        justAged.consumed(true);
        check(justAged.yield(11999) == Budget::Yield::None, "aged pair yielded before its second reference");
        check(justAged.yield(12000) == Budget::Yield::Time, "aged pair exceeded its time budget");
        justAged.consumed(true);
        check(justAged.yield(0) == Budget::Yield::Video, "aged pair exceeded two video frames");
        check(Budget(3,0,Policy::PresentationFirst,fps).catchup(), "fresh depth-three queue failed to catch up");
        check(!Budget(1,200,Policy::PresentationFirst,fps).catchup(), "single aged MTS frame triggered catchup");
    }
    check(Budget::pairAgeThresholdMs(0) == 34 && Budget::pairAgeThresholdMs(-1) == 34,
          "invalid FPS must fall back to 60 without division by zero");
    Budget immediatePair(2,0,Policy::ImmediatePresentationFirst);
    check(immediatePair.catchup(), "legacy P4 comparison lost immediate pair catchup");
    immediatePair.consumed(true);
    check(immediatePair.yield(0) == Budget::Yield::None, "legacy P4 must decode a fresh pair together");
    immediatePair.consumed(true);
    check(immediatePair.yield(0) == Budget::Yield::Video, "legacy P4 pair exceeded two frames");
    check(pairedArrivalPresentations(Policy::PresentationFirst) == 120, "guarded P4 lost presentation turns on fresh pairs");
    check(pairedArrivalPresentations(Policy::ImmediatePresentationFirst) == 60, "reproduction no longer exercises the old P4 burst regression");
    Budget catchup(3,80);
    catchup.consumed(true);
    check(catchup.catchup() && catchup.yield(11999) == Budget::Yield::None, "aged backlog cannot catch up");
    check(catchup.yield(12000) == Budget::Yield::Time, "catchup work exceeded its budget");
    catchup.consumed(true);
    catchup.consumed(true);
    check(catchup.yield(0) == Budget::Yield::Video, "catchup batch must yield after three frames");
    Budget boundedBurst(3,0,Policy::BoundedQueue);
    check(boundedBurst.catchup() && boundedBurst.yield(100000) == Budget::Yield::None,
          "bounded queue must catch up fresh bursts and always progress one atomic operation");
    boundedBurst.consumed(true);
    check(boundedBurst.yield(11999) == Budget::Yield::None, "fresh bounded burst yielded before second frame");
    check(boundedBurst.yield(12000) == Budget::Yield::Time, "bounded catchup exceeded its time budget");
    boundedBurst.consumed(true);
    check(boundedBurst.yield(0) == Budget::Yield::Video, "bounded catchup decoded more than two frames");
    Budget boundedFresh(2,15,Policy::BoundedQueue);
    check(!boundedFresh.catchup(), "two fresh bounded frames must retain separate presentation turns");
    boundedFresh.consumed(true);
    check(boundedFresh.yield(0) == Budget::Yield::Video, "fresh bounded pair bypassed presentation");
    Budget boundedAged(2,16,Policy::BoundedQueue);
    check(boundedAged.catchup(), "bounded pair must catch up at one frame of queue age");
    Budget boundedSingle(1,200,Policy::BoundedQueue);
    check(!boundedSingle.catchup(), "a single aged bounded frame cannot catch up");
    Budget baselineShallow(2,200,Policy::LegacyPresentationFirst);
    check(!baselineShallow.catchup(), "A/B control changed the baseline two-frame policy");
    Budget baselineFresh(3,79,Policy::LegacyPresentationFirst);
    check(!baselineFresh.catchup(), "A/B control changed the baseline age threshold");
    Budget baselineAged(3,80,Policy::LegacyPresentationFirst);
    check(baselineAged.catchup(), "A/B control lost the baseline aged catchup");
    baselineAged.consumed(true); baselineAged.consumed(true);
    check(baselineAged.yield(0)==Budget::Yield::Video,"A/B control changed the baseline two-AU cap");
    Budget audio(0);
    for (unsigned i=0;i<12;++i) audio.consumed(false);
    check(audio.yield(0) == Budget::Yield::Packets, "audio-only backlog must yield to controls");
    std::deque<Packet> mixed;
    for (unsigned i=0;i<24;++i) mixed.push_back({i,i%3 != 2,1000});
    const auto result=drain(mixed);
    check(result.audio == 8 && result.order.size() == 24, "video backlog starved audio or lost media");
    for (unsigned i=0;i<24;++i) check(result.order[i] == i, "compressed references changed order");
    check(result.maxVideo == 3 && result.cycles == 7, "MTS backlog did not drain in bounded three-frame batches");
    const auto boundedResult=drain(mixed,Policy::BoundedQueue);
    check(boundedResult.audio == 8 && boundedResult.order.size() == 24, "bounded catchup starved audio or lost media");
    for (unsigned i=0;i<24;++i) check(boundedResult.order[i] == i, "bounded catchup changed compressed reference order");
    check(boundedResult.maxVideo == 2 && boundedResult.cycles == 10, "fresh bounded backlog did not catch up in bounded batches");
    const auto boundedPair=drain({{0,true,1000},{1,true,1000}},Policy::BoundedQueue,16);
    check(boundedPair.cycles == 1 && boundedPair.maxVideo == 2, "aged bounded pair did not drain together");
    const auto expensive=drain({{0,true,200000},{1,false,100},{2,true,1000},{3,false,100}});
    check(expensive.cycles == 3 && expensive.audio == 2, "slow decoder operation must yield then resume audio");
    const auto boundedExpensive=drain({{0,true,200000},{1,false,100},{2,true,1000},{3,true,1000},{4,false,100}},Policy::BoundedQueue);
    check(boundedExpensive.cycles == 4 && boundedExpensive.audio == 2,
          "bounded slow decoder operation must yield then resume FIFO audio and video");
    std::cout << "Playback drain: MTS age guard at 30/60 FPS, paired-arrival regression, legacy P4/control, FH, FIFO audio/video and atomic overrun PASS\n";
}
