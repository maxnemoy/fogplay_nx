#pragma once
#include "features/streaming/domain/protocol_telemetry.hpp"
#include "features/streaming/domain/network_diagnostics_text.hpp"
#include <cstdio>
#include <string>
namespace stream::telemetry::protocol {
inline const char* operationName(Operation value) {switch(value){
case Operation::Op0:return "idle";
case Operation::Op1:return "init.ice-servers";
case Operation::Op2:return "init.peer-create";
case Operation::Op3:return "init.send-request";
case Operation::Op4:return "candidate.validate";
case Operation::Op5:return "candidate.apply";
case Operation::Op6:return "signal.envelope";
case Operation::Op7:return "offer.envelope";
case Operation::Op8:return "offer.base64";
case Operation::Op9:return "offer.json";
case Operation::Op10:return "offer.filter";
case Operation::Op11:return "offer.mids";
case Operation::Op12:return "offer.parse-sdp";
case Operation::Op13:return "offer.candidates";
case Operation::Op14:return "offer.set-remote";
case Operation::Op15:return "offer.create-answer";
case Operation::Op16:return "candidate.envelope";
case Operation::Op17:return "candidate.base64";
case Operation::Op18:return "candidate.json";
case Operation::Op19:return "stomp.parse";
case Operation::Op20:return "signal.json";
case Operation::Op21:return "answer.send";
case Operation::Op22:return "candidate.send";
case Operation::Op23:return "control.json";
default:return "unknown";}}
inline const char* candidateName(unsigned v){switch(v){case 1:return "host";case 2:return "srflx";case 3:return "prflx";case 4:return "relay";default:return "unknown";}}
inline const char* protocolName(unsigned v){switch(v){case 1:return "udp";case 2:return "tcp-active";case 3:return "tcp-passive";case 4:return "tcp-so";case 5:return "tcp-unknown";default:return "unknown";}}
inline std::string routeText(const Route& r){
    auto out=std::string(" requested=")+(r.relay?"relay":"all")+" actual="+(r.actual==2?"turn":r.actual==1?"direct":"unknown")+
        " localType="+candidateName(r.local)+" remoteType="+candidateName(r.remote)+" localProtocol="+protocolName(r.localProtocol)+" remoteProtocol="+protocolName(r.remoteProtocol)+
        " remoteProof="+(r.proof==2?"relay-endpoint":r.proof==1?"direct-endpoint":"none")+
        " iceSnapshotAvailable="+std::to_string(r.iceSnapshotAvailable)+" localPolicy=all routeKnown="+std::to_string(r.selected)+" routeCurrent="+std::to_string(r.current)+" routeSnapshot="+(r.live?"live":"final");
    if(!r.workerPresent)out+=" iceGeneration=0 iceInFlight=0 iceSnapshotAgeMs=-1";
    else out+=" iceGeneration="+std::to_string(r.revision)+" icePublishedGeneration="+std::to_string(r.publishedRevision)+" iceInFlight="+std::to_string(r.active)+" iceInFlightMs="+std::to_string(r.activeMs)+
        " icePending="+std::to_string(r.pending)+" iceRequested="+std::to_string(r.requested)+" icePublishedSerial="+std::to_string(r.serial)+" iceCompleted="+std::to_string(r.completed)+" iceDiscarded="+std::to_string(r.discarded)+" iceSnapshotAgeMs="+std::to_string(r.ageMs)+" iceStopping="+std::to_string(r.stopping);
    return out+" candidateProofs="+std::to_string(r.candidateProofs)+" candidateProofDrops="+std::to_string(r.candidateProofDrops)+" routeSelections="+std::to_string(r.selections)+" routeEventDrops="+std::to_string(r.eventDrops);
}
inline std::string signalingText(const SignalingCounts& c,const char* name){return std::string(" ")+name+"Attempts="+std::to_string(c.attempts)+" "+name+"LocalAccepted="+std::to_string(c.accepted)+" "+name+"Buffered="+std::to_string(c.buffered)+" "+name+"Rejected="+std::to_string(c.rejected);}
inline std::string senderText(const Sender& c){return network::values({{"controlPending",c.pending},{"controlBytes",c.bytes},{"controlRejected",c.rejected},{"controlSent",c.sent},{"controlPongCoalesced",c.pongCoalesced},{"controlAgeMs",c.ageMs},{"controlAgeMaxMs",c.ageMaxMs},{"controlBackpressure",c.backpressure},{"controlOperationStalled",c.operationStalled},{"controlOperationMs",c.operationMs},{"controlQueueResets",c.queueResets},{"controlTransportActive",c.transportActive},{"controlInFlight",c.inFlight},{"controlInFlightMs",c.inFlightMs},{"controlSendCalls",c.send.calls},{"controlSendAvgUs",c.send.calls?c.send.totalUs/c.send.calls:0},{"controlSendMaxUs",c.send.maxUs},{"controlWorkerIdleCalls",c.idle.calls},{"controlWorkerIdleAvgUs",c.idle.calls?c.idle.totalUs/c.idle.calls:0},{"controlWorkerIdleMaxUs",c.idle.maxUs}});}
inline std::string transportText(const Mts& m){return " peerState="+std::to_string(m.peerState)+" peerAgeMs="+std::to_string(m.ages[0])+" iceState="+std::to_string(m.iceState)+" iceAgeMs="+std::to_string(m.ages[1])+" gatheringState="+std::to_string(m.gatheringState)+" gatheringAgeMs="+std::to_string(m.ages[2])+" rtpAgeMs="+std::to_string(m.ages[3])+" audioReceiveAgeMs="+std::to_string(m.ages[4])+" controlAgeMs="+std::to_string(m.ages[5])+
    network::values({{"eventQueue",m.eventQueue},{"eventBytes",m.eventBytes},{"eventHigh",m.eventHigh},{"eventDrop",m.eventDrop},{"eventOverflow",m.eventOverflow},{"callbackFailure",m.callbackFailure}})+network::historyText(m.history)+network::incomingText(network::ControlSnapshot{m.incoming,m.schemas,{}})+" eventDelayHighMs="+std::to_string(m.eventDelayHighMs);}
inline std::string mtsProtocolText(const Mts& m){return routeText(m.route)+" operation="+operationName(m.operation)+network::values({{"offers",m.offers},{"candidates",m.candidates},{"emptyCandidates",m.emptyCandidates},{"hostnameCandidatesSkipped",m.hostnameCandidatesSkipped},{"hbOutMs",m.hbOutMs},{"hbInMs",m.hbInMs},{"hbSent",m.hbSent}})+" wsAgeMs="+std::to_string(m.wsAgeMs)+network::values({{"wsMessages",m.wsMessages},{"stompFrames",m.stompFrames},{"stompConnected",m.stompConnected},{"stompMessage",m.stompMessage},{"stompError",m.stompError},{"stompOther",m.stompOther},{"signalInit",m.signalInit}})+signalingText(m.subscribe,"subscribe")+signalingText(m.enabled,"enabled")+senderText(m.sender)+transportText(m);}
inline std::string encryptionText(const Encryption& value){
    return " enc_requested=" + std::to_string(value.requested) +
        " enc_core_request=" + std::to_string(value.encryptionClientFlags) +
        " enc_state=" + std::to_string(value.encryptionState) + " enc_failure=" + std::to_string(value.encryptionFailure) +
        " enc_host_supported=" + std::to_string(value.encryptionHostSupported) +
        " enc_host_required=" + std::to_string(value.encryptionHostRequired) +
        " enc_selected=" + std::to_string(value.encryptionSelected) +
        " enc_audio=" + std::to_string(value.encryptionAudio) +
        " enc_control=" + std::to_string(value.encryptionControl) +
        " enc_rtsp=" + std::to_string(value.encryptionRtsp) +
        " enc_input_protocol=always" +
        " enc_observed_video=" + std::to_string(value.videoDecryptSuccesses) +
        " enc_observed_audio=" + std::to_string(value.audioDecryptSuccesses) +
        " enc_audio_failures=" + std::to_string(value.audioDecryptFailures);
}
inline std::string fhProtocolText(const Fh& h){const auto& command=h.command;const auto& firstFailure=h.firstFailure;const char* operation=command.operation==2?"resume":command.operation==1?"launch":"none";
        return "Moonlight code=" + std::to_string(h.code) +
            " command=" + operation + " attempt=" + std::to_string(h.attempt) +
            " curl=" + std::to_string(command.curl) + " http=" + std::to_string(command.http) +
            " xml=" + std::to_string(command.xmlStatus) + " xmlResult=" + std::to_string(command.xmlResult) +
            " ackField=" + (command.operation == 2 ? "resume" : "gamesession") +
            " ack=" + std::to_string(command.gameSession) +
            " dispatched=" + std::to_string(command.dispatched) + " cancelled=" + std::to_string(command.cancelled) +
            " commandElapsedMs=" + std::to_string(command.elapsedMs) + " httpElapsedMs=" + std::to_string(command.httpElapsedMs) +
            " commandFailures=" + std::to_string(h.failures) + " firstFailureCurl=" + std::to_string(firstFailure.curl) +
            " firstFailureHttp=" + std::to_string(firstFailure.http) + " firstFailureXml=" + std::to_string(firstFailure.xmlStatus) +
            " firstFailureAck=" + std::to_string(firstFailure.gameSession) +
            " closedToResumeMs=" + std::to_string(h.closedToResumeMs) +
            " video=" + std::to_string(h.video) + " audio=" + std::to_string(h.audio) +
            " assemblyWaiting=" + std::to_string(h.assemblyWaiting) +
            " decoderWaiting=" + std::to_string(h.decoderWaiting) +
            " codec=" + std::string((h.codecH265?"H.265":"H.264")) +
            " requestedCodec=" + std::string((h.requestedH265?"H.265":"H.264")) +
            " width=" + std::to_string(h.width) +
            " height=" + std::to_string(h.height) +
            " fps=" + std::to_string(h.fps) +
            " requestedFps=" + std::to_string(h.requestedFps) +
            " requestedMbps=" + std::to_string(h.requestedMbps) +
            " inputAccepted=" + std::to_string(h.inputAccepted) +
            " inputRejected=" + std::to_string(h.inputRejected) +
            " terminationCode=" + std::to_string(h.terminationCode) +
            " terminationOrigin=" + (h.hostTermination ? "host" : "transport") +
            " retryableFailure=" + std::to_string(h.retryableFailure) +
            " fatalFailure=" + std::to_string(h.fatalFailure) + encryptionText(h.encryption);
}
inline std::string mtsInputText(const Input& out){
    auto text = "PS input scope=transport-generation isPS=" + std::to_string(out.playStation) +
        " frames=" + std::to_string(out.psFrames) + " accepted=" + std::to_string(out.psAccepted) +
        " rejected=" + std::to_string(out.psRejected) + " unavailable=" + std::to_string(out.unavailable) +
        " touchQueuedDown=" + std::to_string(out.touchQueuedDown) + " touchQueuedUp=" + std::to_string(out.touchQueuedUp) +
        " touchRejectedDown=" + std::to_string(out.touchRejectedDown) + " touchRejectedUp=" + std::to_string(out.touchRejectedUp) +
        " touchAttemptDown=" + std::to_string(out.touchAttemptDown) + " touchAttemptUp=" + std::to_string(out.touchAttemptUp) +
        " touchSentDown=" + std::to_string(out.touchSentDown) + " touchSentUp=" + std::to_string(out.touchSentUp) +
        " touchFailedDown=" + std::to_string(out.touchFailedDown) + " touchFailedUp=" + std::to_string(out.touchFailedUp) +
        " touchDroppedAtStop=" + std::to_string(out.touchDropped) + " stateCoalesced=" + std::to_string(out.gamepadCoalesced) +
        " lastQueuedSeq=" + std::to_string(out.lastQueuedSeq) + " lastAttemptSeq=" + std::to_string(out.lastAttemptSeq) +
        " lastSentSeq=" + std::to_string(out.lastSentSeq) + " invalidNative=" + std::to_string(out.invalidNative);
    static constexpr const char* names[]{"unused", "nativeDown", "nativeMove", "nativeUp", "accel", "gyro"};
    for (std::size_t n = 1; n < out.nativeQueued.size(); ++n) {
        const std::string name = names[n];
        text += " " + name + "Queued=" + std::to_string(out.nativeQueued[n]) +
            " " + name + "Rejected=" + std::to_string(out.nativeRejected[n]) +
            " " + name + "Attempt=" + std::to_string(out.nativeAttempt[n]) +
            " " + name + "Sent=" + std::to_string(out.nativeSent[n]) +
            " " + name + "Failed=" + std::to_string(out.nativeFailed[n]) +
            " " + name + "Dropped=" + std::to_string(out.nativeDropped[n]) +
            " " + name + "Coalesced=" + std::to_string(out.nativeCoalesced[n]);
    }
    return text + " sendSuccess=local-rtc-acceptance ack=unsupported";
}
inline const char* stateName(State state){switch(state){case State::Live:return "live";case State::FinalRetained:return "final";case State::CaptureFailed:return "capture-failed";default:return "not-published";}}
inline bool available(const ProtocolSnapshot& s){return s.state==State::Live||s.state==State::FinalRetained;}
inline std::string unavailable(const ProtocolSnapshot& s){return std::string("provider=")+(s.provider==Provider::Mts?"MTS":s.provider==Provider::FogHub?"FH":"unknown")+" unavailable="+stateName(s.state);}
inline std::string observationText(const ProtocolSnapshot& s){return std::string(" protocolState=")+stateName(s.state)+" protocolSourceCapturedAtUs="+std::to_string(s.sourceCapturedAtSteadyUs);}
inline std::string inputText(const ProtocolSnapshot& s){if(!available(s))return unavailable(s);if(s.provider==Provider::Mts)return mtsInputText(s.mts.input)+observationText(s);if(s.provider==Provider::FogHub)return std::string("Moonlight input profile=")+(s.fh.dualSense?"dualsense":"xbox")+" accepted="+std::to_string(s.fh.inputAccepted)+" rejected="+std::to_string(s.fh.inputRejected)+" optionalIgnored="+std::to_string(s.fh.inputIgnored)+" accelRateHz="+std::to_string(s.fh.accelRateHz)+" gyroRateHz="+std::to_string(s.fh.gyroRateHz)+observationText(s);return "provider=unknown unavailable=not-migrated";}
inline std::string mutexGroup(const std::array<MutexStats,3>& snapshot,const char* prefix,bool enabled){
    static constexpr const char* tags[]{"Callback","Client","Diagnostics"};
    std::string text=std::string("scope=transport-lifetime mutex=")+prefix+" timedMutexEnabled="+std::to_string(enabled)+" "+prefix+"SlowUs=1000";
    for(std::size_t i=0;i<snapshot.size();++i){const auto& stats=snapshot[i];const double divisor=stats.count?static_cast<double>(stats.count)*1000.0:1;char fields[400];const auto label=std::string(prefix)+tags[i];
        std::snprintf(fields,sizeof(fields)," %sCount=%llu %sWaitAvgUs=%.2f %sWaitMaxUs=%.2f %sWaitSlow=%llu %sHoldAvgUs=%.2f %sHoldMaxUs=%.2f %sHoldSlow=%llu",label.c_str(),static_cast<unsigned long long>(stats.count),label.c_str(),stats.waitNs/divisor,label.c_str(),stats.waitMaxNs/1000.0,label.c_str(),static_cast<unsigned long long>(stats.waitSlow),label.c_str(),stats.holdNs/divisor,label.c_str(),stats.holdMaxNs/1000.0,label.c_str(),static_cast<unsigned long long>(stats.holdSlow));text+=fields;}
    return text;
}
inline std::string mutexText(const ProtocolSnapshot& s){if(!available(s))return unavailable(s);if(s.provider==Provider::Mts)return mutexGroup(s.mts.eventMutex,"event",s.mts.mutexEnabled)+" | "+mutexGroup(s.mts.videoMutex,"video",s.mts.mutexEnabled)+observationText(s)+" eventMutexKnown="+std::to_string(s.mts.eventMutexKnown)+" videoMutexKnown="+std::to_string(s.mts.videoMutexKnown)+" videoMutexCapturedAtUs="+std::to_string(s.mts.videoMutexCapturedAtUs)+" videoMutexStale="+std::to_string(!s.mts.videoMutexKnown||s.capturedAtSteadyUs<s.mts.videoMutexCapturedAtUs||s.capturedAtSteadyUs-s.mts.videoMutexCapturedAtUs>2000000);if(s.provider==Provider::FogHub){const auto& t=s.fh.queueLock;return "Moonlight cumulative worker_queue_lock_n="+std::to_string(t.calls)+" avg_us="+std::to_string(t.calls?t.totalUs/t.calls:0)+" max_us="+std::to_string(t.maxUs)+" over50ms="+std::to_string(t.over50ms)+observationText(s);}return "provider=unknown unavailable=not-migrated";}
inline std::string controlDeliveryText(const ProtocolSnapshot& s){if(!available(s))return unavailable(s);if(s.provider!=Provider::Mts)return "provider=FH unavailable=unsupported";
    const auto& d=s.mts.delivery;const auto now=s.capturedAtSteadyUs/1000;const bool ageKnown=d.known&&now>=d.sampledAtMs;const auto age=ageKnown?now-d.sampledAtMs:0;const bool stale=!ageKnown||age>5000;const auto end=stale?d.sampledAtMs:now;const auto blockedMs=d.blocked&&ageKnown&&end>=d.blockedSinceMs?end-d.blockedSinceMs:0;
    auto out=std::string("controlBufferedKnown=")+std::to_string(d.known)+" controlBufferedBytes="+std::to_string(d.bytes)+" controlBufferedMaxBytes="+std::to_string(d.maxBytes)+" controlBufferedAgeMs="+(ageKnown?std::to_string(age):"-1")+" controlBufferedStale="+std::to_string(stale)+" controlBufferedBlocked="+std::to_string(d.blocked)+" controlBufferedBlockedMs="+std::to_string(blockedMs)+" reliabilityKnown="+std::to_string(d.reliability.known);
    if(d.reliability.known)out+=" unordered="+std::to_string(d.reliability.unordered)+" maxRetransmits="+(d.reliability.retransmitsKnown?std::to_string(d.reliability.retransmits):"unset")+" maxLifetimeMs="+(d.reliability.lifetimeKnown?std::to_string(d.reliability.lifetimeMs):"unset");
    return out+" sctpRttMs="+(d.rttKnown?std::to_string(d.rttMs):"unknown")+" rttSnapshotAgeMs="+std::to_string(d.rttAgeMs)+" rttSnapshotCurrent="+std::to_string(d.rttCurrent)+" inputAck=unsupported"+observationText(s);
}
inline std::string protocolText(const ProtocolSnapshot& s){if(!available(s))return unavailable(s);if(s.provider==Provider::Mts)return mtsProtocolText(s.mts)+observationText(s);if(s.provider==Provider::FogHub)return fhProtocolText(s.fh)+observationText(s)+" encryptionKnown="+std::to_string(s.fh.encryptionKnown)+" encryptionCapturedAtUs="+std::to_string(s.fh.encryptionCapturedAtUs)+" encryptionStale="+std::to_string(!s.fh.encryptionKnown||s.capturedAtSteadyUs<s.fh.encryptionCapturedAtUs||s.capturedAtSteadyUs-s.fh.encryptionCapturedAtUs>2000000);return "provider=unknown unavailable=not-migrated";}
}
