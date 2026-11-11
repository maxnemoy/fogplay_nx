#pragma once
#include "features/streaming/domain/network_telemetry.hpp"
#include <array>
#include <cstdint>
#include <type_traits>
namespace stream::telemetry::protocol {
enum class Provider : unsigned char { Unknown, Mts, FogHub };
enum class State : unsigned char { NotPublished, Live, FinalRetained, CaptureFailed };
enum class Operation : unsigned char { Unknown, Op0, Op1, Op2, Op3, Op4, Op5, Op6, Op7, Op8, Op9, Op10, Op11, Op12, Op13, Op14, Op15, Op16, Op17, Op18, Op19, Op20, Op21, Op22, Op23 };
// Fixed client-owned labels only. No remote identifiers, payloads or SDK handles.
struct Reliability { bool known=false,unordered=false,retransmitsKnown=false,lifetimeKnown=false; std::uint64_t retransmits=0,lifetimeMs=0; };
struct MutexStats { std::uint64_t count=0,waitNs=0,holdNs=0,waitMaxNs=0,holdMaxNs=0,waitSlow=0,holdSlow=0; };
struct Timing {std::uint64_t calls=0,totalUs=0,maxUs=0,over50ms=0;};
struct Route {
    bool relay=false,selected=false,current=false,live=false,iceSnapshotAvailable=false,workerPresent=false;
    unsigned local=0,remote=0,localProtocol=0,remoteProtocol=0,proof=0,actual=0;
    std::uint64_t revision=0,publishedRevision=0,activeMs=0,pending=0,requested=0,serial=0,completed=0,discarded=0;
    bool active=false,stopping=false;std::int64_t ageMs=-1;
    std::uint64_t candidateProofs=0,candidateProofDrops=0,selections=0,eventDrops=0;
};
struct SignalingCounts {std::uint64_t attempts=0,accepted=0,buffered=0,rejected=0;};
struct Sender {
    std::uint64_t pending=0,bytes=0,rejected=0,sent=0,pongCoalesced=0,ageMs=0,ageMaxMs=0,operationMs=0,queueResets=0,inFlightMs=0;
    bool backpressure=false,operationStalled=false,transportActive=false,inFlight=false;Timing send{},idle{};
};
struct Input {
    bool playStation=false;std::uint64_t psFrames=0,psAccepted=0,psRejected=0,unavailable=0,invalidNative=0;
    std::uint64_t touchQueuedDown=0,touchQueuedUp=0,touchRejectedDown=0,touchRejectedUp=0,touchAttemptDown=0,touchAttemptUp=0;
    std::uint64_t touchSentDown=0,touchSentUp=0,touchFailedDown=0,touchFailedUp=0,touchDropped=0,gamepadCoalesced=0,lastQueuedSeq=0,lastAttemptSeq=0,lastSentSeq=0;
    std::array<std::uint64_t,6> nativeQueued{},nativeRejected{},nativeAttempt{},nativeSent{},nativeFailed{},nativeDropped{},nativeCoalesced{};
};
struct Delivery {
    bool known=false,blocked=false,rttKnown=false,rttCurrent=false;std::uint64_t bytes=0,maxBytes=0,sampledAtMs=0,blockedSinceMs=0;
    std::int64_t rttMs=0,rttAgeMs=-1;Reliability reliability{};
};
struct Mts {
    Route route{};Operation operation=Operation::Unknown;Sender sender{};Input input{};Delivery delivery{};
    std::uint64_t offers=0,candidates=0,emptyCandidates=0,hostnameCandidatesSkipped=0,hbOutMs=0,hbInMs=0,hbSent=0;
    std::int64_t wsAgeMs=0;std::uint64_t wsMessages=0,stompFrames=0,stompConnected=0,stompMessage=0,stompError=0,stompOther=0,signalInit=0;
    SignalingCounts subscribe{},enabled{};
    int peerState=0,iceState=0,gatheringState=0;
    std::array<std::int64_t,6> ages{{-1,-1,-1,-1,-1,-1}}; // peer,ice,gathering,RTP,audio,control, milliseconds
    std::uint64_t eventQueue=0,eventBytes=0,eventHigh=0,eventDrop=0,eventOverflow=0,callbackFailure=0,eventDelayHighMs=0;
    network::HistorySnapshot history{};network::IncomingSnapshot incoming{};network::IncomingSchemaSnapshot schemas{};
    std::array<MutexStats,3> eventMutex{},videoMutex{};
    bool mutexEnabled=false,eventMutexKnown=false,videoMutexKnown=false;std::uint64_t videoMutexCapturedAtUs=0;
};
struct Command {
    unsigned operation=0;int curl=-1,xmlStatus=0,xmlResult=-1,gameSession=-1;std::int64_t http=0;
    bool dispatched=false,cancelled=false;std::uint64_t elapsedMs=0,httpElapsedMs=0;
};
struct Encryption {
    std::uint64_t requested=0,encryptionClientFlags=0,encryptionState=0,encryptionFailure=0,encryptionHostSupported=0,encryptionHostRequired=0,encryptionSelected=0,encryptionAudio=0,encryptionControl=0,encryptionRtsp=0,videoDecryptSuccesses=0,audioDecryptSuccesses=0,audioDecryptFailures=0;
};
struct Fh {
    Command command{},firstFailure{};Encryption encryption{};
    std::uint64_t attempt=0,failures=0,closedToResumeMs=0,video=0,audio=0,inputAccepted=0,inputRejected=0,inputIgnored=0;
    int code=0,terminationCode=0,width=0,height=0,fps=0,requestedFps=0,requestedMbps=0;
    bool assemblyWaiting=false,decoderWaiting=false,codecH265=false,requestedH265=false,hostTermination=false,retryableFailure=false,fatalFailure=false,dualSense=false;
    unsigned accelRateHz=0,gyroRateHz=0;Timing queueLock{};
    bool encryptionKnown=false;std::uint64_t encryptionCapturedAtUs=0;
};
struct ProtocolSnapshot {
    Provider provider=Provider::Unknown;State state=State::NotPublished;
    std::uint64_t streamEpoch=0,transportEpoch=0,videoGeneration=0,capturedAtSteadyUs=0,completedAtSteadyUs=0,sourceCapturedAtSteadyUs=0;
    Mts mts{};Fh fh{};
};
static_assert(std::is_trivially_copyable_v<ProtocolSnapshot> && sizeof(ProtocolSnapshot)<=4096);
}
namespace stream {
class StreamProtocolTelemetrySource {
public:
    virtual ~StreamProtocolTelemetrySource()=default;
    // Application owner only. Bounded copied values; no SDK queries or formatting.
    virtual telemetry::protocol::ProtocolSnapshot captureProtocolTelemetry()const noexcept{return {};}
};
}
