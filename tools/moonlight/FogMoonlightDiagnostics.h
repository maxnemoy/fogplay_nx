#ifndef FOG_MOONLIGHT_DIAGNOSTICS_H
#define FOG_MOONLIGHT_DIAGNOSTICS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Connection-local cumulative counters, reset by LiStartConnection() before
// core threads start. Each field is read atomically; fields may represent
// slightly different instants. Read during the owner's current connection.
// Times share LiGetMicroseconds()' clock; zero means no packet yet. This core
// may use wall time on Switch, so consumers must validate timestamp ordering.
typedef struct FogMoonlightDiagnostics {
    uint64_t videoPacketsReceived;
    uint64_t videoBytesReceived;
    uint64_t videoReceiveErrors;
    uint64_t videoDecryptFailures;
    uint64_t videoRuntPackets;
    uint64_t lastVideoPacketUs;
    uint64_t maxVideoPacketGapUs;
    // RtpvAddPacket() work only: excludes UDP wait, allocation and decryption.
    uint64_t videoPacketProcessingCount;
    uint64_t videoPacketProcessingTotalUs;
    uint64_t videoPacketProcessingMaxUs;
    uint64_t videoPacketProcessingInvalidTimes;
    // Entire receive-loop iteration, including allocation/decryption/RTP work
    // and recvUdpSocket() wait (including timeouts/errors). Work = cycle - wait.
    // Elapsed wall-clock durations can include thread preemption; they aren't CPU time.
    uint64_t videoReceiveCycleCount;
    uint64_t videoReceiveCycleTotalUs;
    uint64_t videoReceiveCycleMaxUs;
    uint64_t videoReceiveWaitTotalUs;
    uint64_t videoReceiveWaitMaxUs;
    uint64_t videoReceiveWorkTotalUs;
    uint64_t videoReceiveWorkMaxUs;
    // Time outside completed iterations: loop condition, diagnostics, scheduling.
    uint64_t videoReceiveLoopGapCount;
    uint64_t videoReceiveLoopGapTotalUs;
    uint64_t videoReceiveLoopGapMaxUs;
    uint64_t videoReceiveAllocationCount;
    uint64_t videoReceiveAllocationTotalUs;
    uint64_t videoReceiveAllocationMaxUs;
    uint64_t videoReceiveDecryptCount;
    uint64_t videoReceiveDecryptTotalUs;
    uint64_t videoReceiveDecryptMaxUs;
    // Invalid/backward samples are omitted from the affected aggregate.
    uint64_t videoReceiveTimingInvalidTimes;
    // Video UDP socket only. Actual is the raw OS SO_RCVBUF report, which may
    // include bookkeeping. Selected is the last successful setsockopt argument.
    uint64_t videoReceiveBufferRequestedBytes;
    uint64_t videoReceiveBufferSelectedBytes;
    uint64_t videoReceiveBufferActualBytes;
    uint64_t videoReceiveBufferSetFailures;
    uint64_t videoReceiveBufferSetLastError;
    uint64_t videoReceiveBufferQueryError;
    uint64_t videoReceiveBufferQuerySucceeded;
    // Complete blocks include blocks needing no recovery. Failed blocks count
    // incomplete/missing blocks and all known trailing blocks abandoned with
    // a dropped frame (including received/rejected tail blocks), not lost UDP packets
    // or whole missing frames whose block count cannot be known. A pending
    // block isn't failed until the core abandons it on newer received data.
    uint64_t fecBlocksCompleted;
    uint64_t fecBlocksFailed;
    uint64_t fecPacketsRecovered;
    uint64_t fecReconstructionFailures;
    // First matching RtpvAddPacket()/queuePacket() rejection reason. Sequence,
    // frame and block mean behind the current cursor: late/duplicate packets
    // cannot be distinguished there. Header means too short for NV metadata;
    // range means beyond this block's advertised FEC sequence window.
    uint64_t videoPacketsRejectedSequence;
    uint64_t videoPacketsRejectedFrame;
    uint64_t videoPacketsRejectedBlock;
    uint64_t videoPacketsRejectedHeader;
    uint64_t videoPacketsRejectedRange;
    uint64_t videoPacketsRejectedDuplicate;
    uint64_t videoPacketsRejectedTransition;
    // Received (not reconstructed) packets accepted behind an already queued
    // packet, and accepted fills of an observed sequence gap. Gap totals include
    // transient reordering and are not a count of proven network packet loss.
    uint64_t videoPacketsOutOfOrder;
    uint64_t videoPacketsGapFill;
    uint64_t videoSequenceGapPackets;
    uint64_t fecSpeculativeLossPredictions;
    uint64_t fecSpeculativeLossMispredictions;
    uint64_t fecSpeculativeModeResumes;
    uint64_t fecOosMode; // 1 suppresses speculative loss predictions.
    // Statistics ONLY for a nonempty current FEC block abandoned on newer data.
    // Expected/received/missing are aggregate shard totals for these blocks;
    // missing means absent at abandonment, not known lost on the network.
    // Unknown entirely absent blocks and discarded trailing blocks are excluded.
    uint64_t fecAbandonedCurrentBlocks;
    uint64_t fecAbandonedPacketsQueued;
    uint64_t fecAbandonedDataExpected;
    uint64_t fecAbandonedDataReceived;
    uint64_t fecAbandonedDataMissing;
    uint64_t fecAbandonedParityExpected;
    uint64_t fecAbandonedParityReceived;
    uint64_t fecAbandonedParityMissing;
    uint64_t fecAbandonedMissingDataMax;
    uint64_t fecAbandonedMissingParityMax;
    uint64_t decodeQueueOverflows;
    // Flush operations and queued frames removed by them (excludes teardown).
    uint64_t decodeQueueFlushes;
    uint64_t decodeQueueFlushedFrames;
    uint64_t decodeQueuePending;
    uint64_t decodeQueueHigh;
    // API requests, not a count of IDR packets sent by the control thread.
    uint64_t idrRequests;
    uint64_t waitingForIdr;
    uint64_t waitingForIdrDrops;
    // Reasons: 1 initial, 2 dropped reference chain, 3 drop limit,
    // 4 decode queue overflow, 5 decoder refresh, 6 frame gap,
    // 7 speculative FEC loss, 8 confirmed FEC loss, 9 corrupt packet sequence,
    // 10 invalid frame payload. Repeated drops keep the
    // original entry reason/time. Release means IDR recognized by depacketizer,
    // not decoder success or presentation. Times use the core clock.
    uint64_t idrWaitEntries;
    uint64_t idrWaitReason;
    uint64_t idrWaitStartedUs;
    uint64_t idrWaitReleases;
    uint64_t idrWaitLastUs;
    uint64_t idrWaitMaxUs;
    uint64_t idrWaitRetryRequests;
    uint64_t idrDropLimitRequests;
    uint64_t lastIdrRequestUs;
    // Actual control transaction begins/success/failure; success establishes
    // local reliable transport acceptance, not host receipt or a decoded IDR.
    uint64_t idrDispatches;
    uint64_t idrDispatchSuccesses;
    uint64_t idrDispatchFailures;
    uint64_t lastIdrDispatchUs;
    uint64_t idrFramesCompleted;
} FogMoonlightDiagnostics;

// Always safe to read, including before start and after stop. NULL is accepted.
// No locks, allocation, logging, or internal queue reads are performed here.
void FogMoonlightGetDiagnostics(FogMoonlightDiagnostics* snapshot);

enum FogMoonlightEventKind {
    FOG_EVENT_WAIT_ENTER = 1, FOG_EVENT_IDR_REQUEST, FOG_EVENT_IDR_DISPATCH,
    FOG_EVENT_IDR_DISPATCH_RESULT, FOG_EVENT_FEC_FAILURE, FOG_EVENT_INTACT_FRAME,
    FOG_EVENT_IDR_HEADER, FOG_EVENT_IDR_RECOGNIZED, FOG_EVENT_WAIT_EXIT,
    FOG_EVENT_IDR_ASSEMBLED, FOG_EVENT_THREAD_START
};
typedef struct FogMoonlightEvent {
    uint64_t sequence, connectionId, waitId, timeUs;
    uint64_t kind, frameNumber, value;
    uint64_t requests, intactFrames, fecFailures, totalsValid;
    // Only THREAD_START uses these fields; signed values preserve OS sentinels.
    int32_t threadCore, threadIdealCore, threadPriority;
    uint32_t threadQueryErrors; // bit 0 affinity query; bit 1 priority query.
    uint64_t threadAffinityMask;
    char role[24];
} FogMoonlightEvent;
// requests/intactFrames/fecFailures count admitted observations in this wait's
// keyed record. totalsValid=0 when no wait is associated or its record expired.
// Any reported journal loss means totals/chain may be incomplete. Dispatch
// results keep their dispatch's wait ID across concurrent wait transitions.
// Best-effort bounded journal. The cursor is process-local, monotonic across
// connections. Each event has a connection/wait ID; frameNumber is valid only
// for frame events. value: reason / block count / dispatch success by kind.
// Returns one stable event, or zero if empty/busy. missed includes overwritten
// entries and producer contention; callers must report it, not infer no loss.
// A short nonblocking atomic gate protects copies; never waits or allocates.
int FogMoonlightReadEvent(uint64_t* cursor, FogMoonlightEvent* event, uint64_t* missed);

#ifdef __cplusplus
}
#endif

#endif
