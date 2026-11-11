#pragma once
#include <cstdint>

namespace stream {
struct PlaybackObservation {
    std::uint64_t presentedFrames = 0, decodedFrames = 0, supersededFrames = 0;
    std::uint64_t workUs = 0, errors = 0, audioUnderruns = 0, audioResets = 0;
    bool recovering = false;
    int width = 0, height = 0;
    // Cumulative video decode + conversion work on a separate worker. Never
    // add overlapping UI and worker times as utilization of one thread.
    std::uint64_t decoderWorkUs = 0, videoGeneration = 0;
    bool workerVideo = false;
};
}
