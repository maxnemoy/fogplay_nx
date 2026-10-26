#pragma once
#include <array>
#include <cstdint>

namespace fog::playstation {
struct PsTouchContact {
    bool active = false;
    std::uint32_t fingerId = 0;
    std::uint16_t screenX = 0, screenY = 0;
    std::uint16_t x = 0, y = 0;
};

struct PsMotionSample {
    bool valid = false, fresh = false;
    std::uint64_t sampleNumber = 0, deltaTime = 0, readAtMs = 0;
    int sourceIndex = -1;
    std::array<float,3> gyroTurnsPerSecond{};
    std::array<float,3> accelerationG{};
    std::array<float,9> direction{};
    std::array<float,3> psGyroRadiansPerSecond{};
    std::array<float,3> psAccelerationG{};
};

struct PsSensorCounters {
    std::uint64_t updates = 0;
    std::uint64_t touchSamples = 0, touchDuplicates = 0, touchMissing = 0;
    std::uint64_t touchStaleReleases = 0;
    std::uint64_t motionSamples = 0, motionDuplicates = 0, motionMissing = 0;
    std::uint64_t motionInvalid = 0, motionStaleReleases = 0;
    std::uint64_t handleRefreshes = 0, startFailures = 0, stopFailures = 0;
};

struct PsSensorSnapshot {
    bool enabled = false;
    bool touchFresh = false, touchValid = false, touchStale = false;
    std::uint64_t touchSampleNumber = 0, touchReadAtMs = 0;
    unsigned touchCount = 0, hidTouchCount = 0;
    std::array<PsTouchContact,2> touches{};
    PsMotionSample motion;
    PsSensorCounters counters;
    int npadId = -1;
    std::uint32_t style = 0;
    unsigned handleCount = 0;
    std::uint32_t lastGetHandlesResult = 0, lastStartResult = 0, lastStopResult = 0;
};
}
