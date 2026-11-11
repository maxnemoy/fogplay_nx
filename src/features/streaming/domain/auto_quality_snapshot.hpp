#pragma once
#include <array>
#include <cstdint>
#include <string_view>
namespace stream::quality {
inline constexpr std::array<const char*,23> DiagnosticReasons={"unknown","manual","startup","sample-gap","profile-stale","await-confirmation","dimensions","floor-low","down-cooldown","down-unconfirmed","above-auto-ceiling","video-work-overload","transport-decode-trouble","video-delivery-loss","audio-playback-loss","headroom-paused","insufficient-headroom","recovery-window","ceiling-high","stable-window","up-cooldown","up-unconfirmed","stable-headroom"};
inline unsigned diagnosticReason(std::string_view value) noexcept {for(unsigned i=0;i<DiagnosticReasons.size();++i)if(value==DiagnosticReasons[i])return i;return 0;}
inline const char* diagnosticReasonName(unsigned value) noexcept {return value<DiagnosticReasons.size()?DiagnosticReasons[value]:DiagnosticReasons[0];}
struct NumericSnapshot {
    bool enabled=false,pending=false,paused=false,workerVideo=false;
    unsigned reason=0,pauseCause=0,upAttempts=0,downAttempts=0,downgrades=0,queueWindows=0,deliveryWindows=0,audioWindows=0;
    int targetFps=30;
    double sampleFps=0,decodedFps=0,videoWorkMs=0,videoBusy=0,uiVideoMs=0,decoderVideoMs=0,uiVideoBusy=0,decoderVideoBusy=0;
    std::uint64_t nextUpMs=0,capCoalesced=0,goodMs=0,evidenceMs=0;
    std::array<std::uint64_t,5> pauseCounts{};
    std::uint64_t rejectLoss=0,rejectSuperseded=0,rejectAudio=0,rejectLoad=0,rejectDelivery=0;
};
}
