#include "features/streaming/domain/playback_drain.hpp"
#include <algorithm>
#include <cstdint>
#include <deque>
#include <iostream>
#include <stdexcept>
#include <vector>

using Budget = fog::stream::PlaybackDrainBudget;
static void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct Packet { unsigned id; bool video; std::uint64_t costUs; };
struct Result { std::vector<unsigned> order; unsigned cycles=0, audio=0, maxVideo=0; };
static Result drain(std::deque<Packet> packets) {
    Result result;
    while (!packets.empty()) {
        const auto queuedVideo = std::count_if(packets.begin(), packets.end(), [](const auto& p) { return p.video; });
        Budget budget(queuedVideo);
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
int main() {
    Budget normal(1);
    check(!normal.catchup() && normal.yield(100000) == Budget::Yield::None, "one atomic operation must always progress");
    normal.consumed(true);
    check(normal.yield(1000) == Budget::Yield::Video, "normal frame must reach presentation before next decode");
    check(normal.yield(8000) == Budget::Yield::Time, "normal work exceeded its budget");
    Budget youngBurst(12,79);
    check(!youngBurst.catchup(), "young receive burst must not discard decoded frames by catching up");
    youngBurst.consumed(true);
    check(youngBurst.yield(5000) == Budget::Yield::Video, "fresh burst bypassed single frame presentation");
    Budget shallow(2,200);
    check(!shallow.catchup(), "two old frames must still get separate presentation turns");
    Budget catchup(3,80);
    catchup.consumed(true);
    check(catchup.catchup() && catchup.yield(11999) == Budget::Yield::None, "aged backlog cannot catch up");
    check(catchup.yield(12000) == Budget::Yield::Time, "catchup work exceeded its budget");
    catchup.consumed(true);
    check(catchup.yield(0) == Budget::Yield::Video, "catchup batch must yield after two frames");
    Budget audio(0);
    for (unsigned i=0;i<12;++i) audio.consumed(false);
    check(audio.yield(0) == Budget::Yield::Packets, "audio-only backlog must yield to controls");
    std::deque<Packet> mixed;
    for (unsigned i=0;i<24;++i) mixed.push_back({i,i%3 != 2,1000});
    const auto result=drain(mixed);
    check(result.audio == 8 && result.order.size() == 24, "video backlog starved audio or lost media");
    for (unsigned i=0;i<24;++i) check(result.order[i] == i, "compressed references changed order");
    check(result.maxVideo == 1 && result.cycles >= 16 && result.cycles <= 18, "normal batching replaced frames before presentation");
    const auto expensive=drain({{0,true,200000},{1,false,100},{2,true,1000},{3,false,100}});
    check(expensive.cycles == 3 && expensive.audio == 2, "slow decoder operation must yield then resume audio");
    std::cout << "Playback drain: one fresh video per presentation turn, aged bounded catchup, FIFO references and atomic overrun PASS\n";
}
