#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace fog::stream {
// First failed condition in the Player admission predicate, in predicate order.
// Shell overlays and the remote host's input acknowledgement are outside scope.
enum class InputGateReason : std::uint8_t { None, Cancelled, Resources, FirstPrepared, PreparedGate,
    Assembly, Decoder, ProviderFreeze, Generation, Transport, RecoveryDeadline, PreparedAge,
    Reconnect, VideoRecovery, Loss, ControlReady, Count };
inline const char* inputGateReasonName(InputGateReason value) noexcept {
    switch(value) {
        case InputGateReason::None:return "none";
        case InputGateReason::Cancelled:return "cancelled";
        case InputGateReason::Resources:return "resources";
        case InputGateReason::FirstPrepared:return "first-prepared";
        case InputGateReason::PreparedGate:return "prepared-gate";
        case InputGateReason::Assembly:return "assembly-wait";
        case InputGateReason::Decoder:return "decoder-wait";
        case InputGateReason::ProviderFreeze:return "provider-freeze";
        case InputGateReason::Generation:return "generation";
        case InputGateReason::Transport:return "transport";
        case InputGateReason::RecoveryDeadline:return "recovery-deadline";
        case InputGateReason::PreparedAge:return "prepared-age";
        case InputGateReason::Reconnect:return "reconnect";
        case InputGateReason::VideoRecovery:return "video-recovery";
        case InputGateReason::Loss:return "loss";
        case InputGateReason::ControlReady:return "control-ready";
        case InputGateReason::Count:break;
    }
    return "unknown";
}
// Historical cause recorded when suspend() last closed the prepared gate.
// It is not a measurement that the corresponding live condition still holds.
enum class InputPreparedGateCause : std::uint8_t { None, Loss, Decoder, Generation, ProviderFreeze, Transport };
inline const char* inputPreparedGateCauseName(InputPreparedGateCause value) noexcept {
    switch(value) {
        case InputPreparedGateCause::None:return "none";
        case InputPreparedGateCause::Loss:return "loss";
        case InputPreparedGateCause::Decoder:return "decoder";
        case InputPreparedGateCause::Generation:return "generation";
        case InputPreparedGateCause::ProviderFreeze:return "provider-freeze";
        case InputPreparedGateCause::Transport:return "transport";
    }
    return "unknown";
}
inline constexpr std::size_t inputGateReasonCount=static_cast<std::size_t>(InputGateReason::Count);
struct InputGateTransition {
    std::uint64_t sequence=0,atMs=0,session=0,transport=0,generation=0,preparedAgeMs=0,previousReasonMs=0;
    InputGateReason previous=InputGateReason::None,reason=InputGateReason::None;
    InputPreparedGateCause previousPreparedGateCause=InputPreparedGateCause::None,preparedGateCause=InputPreparedGateCause::None;
    std::uint64_t preparedGateCauseAtMs=0;
};
struct InputGateSnapshot {
    static constexpr std::size_t Capacity=8;
    std::array<InputGateTransition,Capacity> transitions{};
    std::array<std::uint64_t,inputGateReasonCount> reasonMs{};
    std::uint64_t capturedAtMs=0,sequence=0,overwritten=0,episodes=0,completed=0,blockedMs=0,activeMs=0;
    std::size_t count=0;
    InputGateReason reason=InputGateReason::None;
    InputPreparedGateCause preparedGateCause=InputPreparedGateCause::None;
    std::uint64_t preparedGateCauseAtMs=0;
    bool initialized=false,final=false,providerSuspensionSupported=false;
};
// Fixed storage, owner-thread only. No log admission or SDK telemetry access.
// Durations measure observed Player gate state, not a confirmed remote input stall.
class InputGateDiagnostics {
public:
    void observe(InputGateReason reason,std::uint64_t nowMs,std::uint64_t session,
        std::uint64_t transport,std::uint64_t generation,std::uint64_t preparedAgeMs,
        InputPreparedGateCause cause=InputPreparedGateCause::None,std::uint64_t causeAtMs=0) noexcept {
        if(value_.final)return;
        if(value_.initialized&&nowMs<lastMs_)nowMs=lastMs_;
        advance(nowMs);
        if(reason!=InputGateReason::PreparedGate){cause=InputPreparedGateCause::None;causeAtMs=0;}
        if(!value_.initialized || reason!=value_.reason || transport!=transport_ ||
            cause!=value_.preparedGateCause || causeAtMs!=value_.preparedGateCauseAtMs) {
            const auto previous=value_.reason;
            if(reason!=InputGateReason::None && (!value_.initialized || previous==InputGateReason::None)) {
                ++value_.episodes;episodeAtMs_=nowMs;
            } else if(reason==InputGateReason::None && previous!=InputGateReason::None)++value_.completed;
            InputGateTransition event{++value_.sequence,nowMs,session,transport,generation,preparedAgeMs,
                value_.initialized&&nowMs>=reasonAtMs_?nowMs-reasonAtMs_:0,previous,reason,
                value_.preparedGateCause,cause,causeAtMs};
            if(value_.count==value_.transitions.size()) {
                for(std::size_t i=1;i<value_.count;++i)value_.transitions[i-1]=value_.transitions[i];
                ++value_.overwritten;
            } else ++value_.count;
            value_.transitions[value_.count-1]=event;
            value_.reason=reason;value_.preparedGateCause=cause;value_.preparedGateCauseAtMs=causeAtMs;
            reasonAtMs_=nowMs;transport_=transport;
        }
        value_.initialized=true;lastMs_=nowMs;
    }
    void finish(std::uint64_t nowMs) noexcept {advance(nowMs);value_.final=true;}
    InputGateSnapshot snapshot(std::uint64_t nowMs,bool supported) const noexcept {
        auto copy=*this;
        if(!copy.value_.final)copy.advance(nowMs);
        copy.value_.capturedAtMs=copy.lastMs_;
        copy.value_.providerSuspensionSupported=supported;
        copy.value_.activeMs=copy.value_.initialized&&copy.value_.reason!=InputGateReason::None&&copy.lastMs_>=copy.episodeAtMs_?
            copy.lastMs_-copy.episodeAtMs_:0;
        return copy.value_;
    }
private:
    void advance(std::uint64_t nowMs) noexcept {
        if(value_.final)return;
        if(value_.initialized && nowMs>=lastMs_ && value_.reason!=InputGateReason::None) {
            const auto elapsed=nowMs-lastMs_;
            value_.blockedMs+=elapsed;
            value_.reasonMs[static_cast<std::size_t>(value_.reason)]+=elapsed;
        }
        if(nowMs>=lastMs_)lastMs_=nowMs;
    }
    InputGateSnapshot value_;
    std::uint64_t lastMs_=0,reasonAtMs_=0,episodeAtMs_=0,transport_=0;
};
}
