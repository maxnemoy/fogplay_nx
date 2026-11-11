#pragma once
#include "features/streaming/domain/network_telemetry.hpp"
#include <string>
namespace stream::telemetry::network {
struct RtpEvent {
    unsigned reason=0;std::uint64_t ms=0;std::int64_t sequence=0;std::uint32_t timestamp=0;
    std::uint64_t missing=0;bool waiting=false;GapExpiry gap{};
};
// Shared pure compatibility kernel; numeric producers never format this text.
inline std::string rtpEventText(const RtpEvent& e){
    std::string out="reason="+std::to_string(e.reason)+" ms="+std::to_string(e.ms)+" seq="+std::to_string(e.sequence)+
        " ts="+std::to_string(e.timestamp)+" missing="+std::to_string(e.missing)+" waitIdr="+std::to_string(e.waiting);
    if(e.reason==1){const auto& g=e.gap;out+=" gapLast="+std::to_string(g.last)+" detectedMs="+std::to_string(g.detectedMs)+
        " oldestMs="+std::to_string(g.oldestMs)+" budgetMs="+std::to_string(g.budgetMs)+" deadlineMs="+std::to_string(g.deadlineMs)+
        " overshootMs="+std::to_string(g.overshootMs)+" firstNackMs="+std::to_string(g.firstNackMs)+" lastNackMs="+std::to_string(g.lastNackMs)+
        " nackAttempts="+std::to_string(g.nackAttempts)+" pendingFirst="+std::to_string(g.pendingFirst)+" pendingLast="+std::to_string(g.pendingLast)+" pendingDepth="+std::to_string(g.pendingDepth);}
    return out;
}
}
