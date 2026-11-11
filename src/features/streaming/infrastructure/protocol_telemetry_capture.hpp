#pragma once
#include "features/streaming/domain/protocol_telemetry.hpp"
#include "features/streaming/infrastructure/timed_mutex.hpp"
namespace stream::telemetry::protocol {
inline std::array<MutexStats,3> captureMutex(const TimedMutex::Snapshot& snapshot){std::array<MutexStats,3> out{};for(std::size_t i=0;i<out.size();++i){const auto& v=snapshot[i];out[i]={v.count,v.waitNs,v.holdNs,v.waitMaxNs,v.holdMaxNs,v.waitSlow,v.holdSlow};}return out;}
template<class T> Sender captureSender(const T& c){Sender out;
out.pending=c.pending;
out.bytes=c.bytes;
out.rejected=c.rejected;
out.sent=c.sent;
out.pongCoalesced=c.pongCoalesced;
out.ageMs=c.ageMs;
out.ageMaxMs=c.ageMaxMs;
out.operationMs=c.operationMs;
out.queueResets=c.queueResets;
out.inFlightMs=c.inFlightMs;
out.backpressure=c.backpressure;
out.operationStalled=c.operationStalled;
out.transportActive=c.transportActive;
out.inFlight=c.inFlight;
out.send={c.sendCalls,c.sendTotalUs,c.sendMaxUs,0};out.idle={c.waitCalls,c.waitTotalUs,c.waitMaxUs,0};return out;}
template<class T> Input captureInput(const T& c){Input out;
out.psFrames=c.psFrames;
out.psAccepted=c.psAccepted;
out.psRejected=c.psRejected;
out.touchQueuedDown=c.touchQueuedDown;
out.touchQueuedUp=c.touchQueuedUp;
out.touchRejectedDown=c.touchRejectedDown;
out.touchRejectedUp=c.touchRejectedUp;
out.touchAttemptDown=c.touchAttemptDown;
out.touchAttemptUp=c.touchAttemptUp;
out.touchSentDown=c.touchSentDown;
out.touchSentUp=c.touchSentUp;
out.touchFailedDown=c.touchFailedDown;
out.touchFailedUp=c.touchFailedUp;
out.touchDropped=c.touchDropped;
out.gamepadCoalesced=c.gamepadCoalesced;
out.lastQueuedSeq=c.lastQueuedSeq;
out.lastAttemptSeq=c.lastAttemptSeq;
out.lastSentSeq=c.lastSentSeq;
out.nativeQueued=c.nativeQueued;
out.nativeRejected=c.nativeRejected;
out.nativeAttempt=c.nativeAttempt;
out.nativeSent=c.nativeSent;
out.nativeFailed=c.nativeFailed;
out.nativeDropped=c.nativeDropped;
out.nativeCoalesced=c.nativeCoalesced;
return out;}
}
