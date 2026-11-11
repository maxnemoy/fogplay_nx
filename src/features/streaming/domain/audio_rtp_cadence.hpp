#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace stream {
// Depacketized Opus admission, serialized by the existing audio queue owner.
// RTP/local spans describe relative cadence only: neither starts at host capture.
struct AudioRtpCadenceSnapshot {
    std::uint64_t packets=0,bytes=0,firstArrivalMs=0,lastArrivalMs=0,maximumGapMs=0,burstIntervals=0;
    std::uint64_t segments=0,segmentFirstArrivalMs=0,segmentLastArrivalMs=0,segmentTicks=0;
    std::uint64_t repeatedTimestamps=0,backsteps=0,jumps=0,wraps=0,clockRegressions=0;
    // Inclusive overlapping lifetime counts: a 250 ms gap increments all three.
    std::uint64_t gapsGe40Ms=0,gapsGe100Ms=0,gapsGe250Ms=0,segmentPackets=0;
};
class AudioRtpCadence {
public:
    static constexpr std::uint32_t ClockRate=48000,MaximumStepTicks=ClockRate*10;
    static constexpr std::uint64_t Gap40Ms=40,Gap100Ms=100,Gap250Ms=250;
    void received(std::uint32_t timestamp,std::size_t bytes,std::uint64_t nowMs) noexcept {
        ++value_.packets;value_.bytes+=bytes;
        if(value_.packets==1){
            value_.firstArrivalMs=value_.lastArrivalMs=nowMs;
            begin(timestamp,nowMs);return;
        }
        const bool clockRegressed=nowMs<value_.lastArrivalMs;
        if(clockRegressed)++value_.clockRegressions;
        else {
            const auto gap=nowMs-value_.lastArrivalMs;
            value_.maximumGapMs=std::max(value_.maximumGapMs,gap);
            if(gap<=1)++value_.burstIntervals;
            if(gap>=Gap40Ms)++value_.gapsGe40Ms;
            if(gap>=Gap100Ms)++value_.gapsGe100Ms;
            if(gap>=Gap250Ms)++value_.gapsGe250Ms;
        }
        value_.lastArrivalMs=nowMs;
        const auto step=static_cast<std::uint32_t>(timestamp-lastTimestamp_);
        if(!step)++value_.repeatedTimestamps;
        const bool backwards=step>=0x80000000u;
        const bool jumped=!backwards&&step>MaximumStepTicks;
        if(backwards)++value_.backsteps;
        if(jumped)++value_.jumps;
        // A backwards timestamp may be reordering or a source reset. Start a
        // new segment rather than inventing a continuous lifetime media clock.
        // A >10 s forward step (including legitimate silence) also breaks it.
        if(clockRegressed||backwards||jumped){begin(timestamp,nowMs);return;}
        if(timestamp<lastTimestamp_)++value_.wraps;
        ++value_.segmentPackets;
        value_.segmentTicks+=step;value_.segmentLastArrivalMs=nowMs;
        lastTimestamp_=timestamp;
    }
    AudioRtpCadenceSnapshot snapshot()const noexcept{return value_;}
private:
    void begin(std::uint32_t timestamp,std::uint64_t nowMs) noexcept {
        ++value_.segments;value_.segmentFirstArrivalMs=value_.segmentLastArrivalMs=nowMs;
        value_.segmentTicks=0;value_.segmentPackets=1;lastTimestamp_=timestamp;
    }
    AudioRtpCadenceSnapshot value_{};
    std::uint32_t lastTimestamp_=0;
};
}
