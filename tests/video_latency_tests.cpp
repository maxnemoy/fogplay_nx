#include "features/streaming/infrastructure/video_latency.hpp"
#include "features/streaming/infrastructure/media_frame_queue.hpp"
#include "features/streaming/infrastructure/stream_queue.hpp"
#include <cassert>
#include <iostream>

int main() {
    using namespace fog::detail;
    VideoFrameOrigins origins;
    origins.remember(1, {100, 2000, true});
    origins.remember(2, {110, 3000, true});
    bool found = false;
    assert(origins.find(2, found).firstPacketMs == 110 && found);
    assert(origins.find(1, found).firstPacketMs == 100 && found); // Reordered output.
    origins.find(-1, found); assert(!found); // Decoder omitted PTS.
    for (int i = 3; i <= 65; ++i) origins.remember(i, {std::uint64_t(i), 0, false});
    origins.find(1, found); assert(!found); // Bounded retention; never guess another AU.
    assert(origins.find(65, found).firstPacketMs == 65 && found);
    origins.clear(); origins.find(65, found); assert(!found);

    stream::MediaQueue compressed;
    stream::Packet packet;
    packet.data.resize(1); packet.firstPacketMs = 10; packet.enqueueMs = 15;
    assert(compressed.push(std::move(packet), 30));
    assert(compressed.pop(packet) && packet.firstPacketMs == 10 && packet.enqueueMs == 30);
    assert(compressed.age(stream::Kind::Video, 40) == 0);

    MediaFrameQueue ready;
    ready.reset(60, true);
    MediaFrameQueue::Frame input, output;
    input.origin = {100, 2500, true};
    const auto now = MediaFrameQueue::Clock::time_point{};
    ready.push(std::move(input), 0, now);
    std::uint64_t dropped = 0;
    assert(ready.take(output, now, dropped) && output.origin.firstPacketMs == 100 &&
        output.origin.assembledQueueWaitUs == 2500 && output.origin.hasQueueWait);

    VideoLatencyMetrics metrics;
    metrics.presented({100, 2000, true}, 110000);
    metrics.presented({110, 4000, true}, 135000); // Exactly 1.5x60fps is not late.
    metrics.presented({120, 6000, true}, 165001); // 30,001 us is late.
    const auto text = metrics.diagnostics(165001, 60);
    assert(text.find("frameAge{samples=3 p50Us=25000 p95Us=45001 p99Us=45001") != std::string::npos);
    assert(text.find("assembledQueueWait{samples=3 p50Us=4000 p95Us=6000") != std::string::npos);
    assert(text.find("presentGap{samples=2 p50Us=25000 p95Us=30001") != std::string::npos);
    assert(text.find("lateCount=1 latePpm=500000") != std::string::npos);
    metrics.presented({999, 0, false}, 170000); // Future origin is unavailable.
    const auto absent = metrics.diagnostics(170000, 60);
    assert(absent.find("frameAgeUnavailableLifetime=1 queueWaitUnavailableLifetime=1") != std::string::npos);
    const auto expired = metrics.diagnostics(5170000, 60);
    assert(expired.find("frameAge{samples=0") != std::string::npos &&
        expired.find("presentGap{samples=0") != std::string::npos && expired.find("lateCount=0 latePpm=0") != std::string::npos);
    std::cout << "PASS video origins: reorder, bounded retention/reset, queue propagation, 5s age/wait/gap percentiles, strict late fraction, unavailable timestamps\n";
}
