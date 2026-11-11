#pragma once
#include "features/streaming/domain/auto_quality_snapshot.hpp"
#include <array>
#include <cstdio>
#include <cmath>
#include <string>
#include <string_view>
namespace stream::quality {
inline constexpr std::array<const char*,11> DiagnosticStates={"enabled","disabled","enhance-pending","degrade-pending","send-failed","downgrade-hold","profile-observed","confirmed","rejected-profile","timeout-unconfirmed","decision"};
inline unsigned diagnosticState(std::string_view v){for(unsigned i=0;i<DiagnosticStates.size();++i)if(v==DiagnosticStates[i])return i;return 255;}
struct NumericEvent {
    std::uint64_t ms=0,nextUpMs=0,goodMs=0,evidenceMs=0;
    double sampleFps=0,decodedFps=0,videoWorkMs=0,videoBusy=0,uiVideoMs=0,decoderVideoMs=0,uiVideoBusy=0,decoderVideoBusy=0;
    unsigned state=255,reason=0,profile=0,downgrades=0,queueWindows=0,deliveryWindows=0,audioWindows=0;
    bool paused=false,workerVideo=false;
};
inline const char* diagnosticProfileName(unsigned v){switch(v){case 1:return "LOW";case 2:return "MID";case 3:return "HIGH";case 4:return "ULTRA";default:return "unknown";}}
inline std::string numericEventText(const NumericEvent& e){
    // Each metric is derived from uint64 deltas and millisecond windows. Values
    // beyond this conservative bound cannot originate in the policy producer.
    for(double value:{e.sampleFps,e.decodedFps,e.videoWorkMs,e.videoBusy,e.uiVideoMs,e.decoderVideoMs,e.uiVideoBusy,e.decoderVideoBusy})if(!std::isfinite(value)||std::abs(value)>1e24)return "qualityEvent=unknown unavailable=invalid-metrics";
    char values[1400];
    std::snprintf(values,sizeof(values)," sampleFps=%.2f decodedFps=%.2f videoWorkMs=%.2f videoBusy=%.3f downgrades=%u nextUpMs=%llu queueWindows=%u deliveryWindows=%u audioWindows=%u goodMs=%llu evidenceMs=%llu paused=%d workerVideo=%d uiVideoMs=%.2f decoderVideoMs=%.2f uiVideoBusy=%.3f decoderVideoBusy=%.3f",e.sampleFps,e.decodedFps,e.videoWorkMs,e.videoBusy,e.downgrades,static_cast<unsigned long long>(e.nextUpMs),e.queueWindows,e.deliveryWindows,e.audioWindows,static_cast<unsigned long long>(e.goodMs),static_cast<unsigned long long>(e.evidenceMs),e.paused,e.workerVideo,e.uiVideoMs,e.decoderVideoMs,e.uiVideoBusy,e.decoderVideoBusy);
    return std::string("qualityEvent=")+(e.state<DiagnosticStates.size()?DiagnosticStates[e.state]:"unknown")+" ms="+std::to_string(e.ms)+" reason="+diagnosticReasonName(e.reason)+" profile="+diagnosticProfileName(e.profile)+values;
}
}
