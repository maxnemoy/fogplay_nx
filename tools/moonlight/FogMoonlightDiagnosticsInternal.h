#ifndef FOG_MOONLIGHT_DIAGNOSTICS_INTERNAL_H
#define FOG_MOONLIGHT_DIAGNOSTICS_INTERNAL_H

#include "FogMoonlightDiagnostics.h"

#define FOG_DIAG_FIELDS(X) \
    X(videoPacketsReceived) X(videoBytesReceived) X(videoReceiveErrors) \
    X(videoDecryptFailures) X(videoRuntPackets) X(lastVideoPacketUs) \
    X(maxVideoPacketGapUs) X(videoPacketProcessingCount) \
    X(videoPacketProcessingTotalUs) X(videoPacketProcessingMaxUs) X(videoPacketProcessingInvalidTimes) \
    X(videoReceiveCycleCount) X(videoReceiveCycleTotalUs) X(videoReceiveCycleMaxUs) \
    X(videoReceiveWaitTotalUs) X(videoReceiveWaitMaxUs) \
    X(videoReceiveWorkTotalUs) X(videoReceiveWorkMaxUs) \
    X(videoReceiveLoopGapCount) X(videoReceiveLoopGapTotalUs) X(videoReceiveLoopGapMaxUs) \
    X(videoReceiveAllocationCount) X(videoReceiveAllocationTotalUs) X(videoReceiveAllocationMaxUs) \
    X(videoReceiveDecryptCount) X(videoReceiveDecryptTotalUs) X(videoReceiveDecryptMaxUs) \
    X(videoReceiveTimingInvalidTimes) X(videoReceiveBufferRequestedBytes) \
    X(videoReceiveBufferSelectedBytes) X(videoReceiveBufferActualBytes) \
    X(videoReceiveBufferSetFailures) X(videoReceiveBufferSetLastError) \
    X(videoReceiveBufferQueryError) X(videoReceiveBufferQuerySucceeded) \
    X(fecBlocksCompleted) X(fecBlocksFailed) \
    X(fecPacketsRecovered) X(fecReconstructionFailures) \
    X(videoPacketsRejectedSequence) X(videoPacketsRejectedFrame) X(videoPacketsRejectedBlock) \
    X(videoPacketsRejectedHeader) X(videoPacketsRejectedRange) X(videoPacketsRejectedDuplicate) \
    X(videoPacketsRejectedTransition) X(videoPacketsOutOfOrder) X(videoPacketsGapFill) X(videoSequenceGapPackets) \
    X(fecSpeculativeLossPredictions) X(fecSpeculativeLossMispredictions) X(fecSpeculativeModeResumes) X(fecOosMode) \
    X(fecAbandonedCurrentBlocks) X(fecAbandonedPacketsQueued) \
    X(fecAbandonedDataExpected) X(fecAbandonedDataReceived) X(fecAbandonedDataMissing) \
    X(fecAbandonedParityExpected) X(fecAbandonedParityReceived) X(fecAbandonedParityMissing) \
    X(fecAbandonedMissingDataMax) X(fecAbandonedMissingParityMax) X(decodeQueueOverflows) \
    X(decodeQueueFlushes) X(decodeQueueFlushedFrames) X(decodeQueuePending) \
    X(decodeQueueHigh) X(idrRequests) X(waitingForIdr) X(waitingForIdrDrops) \
    X(idrWaitEntries) X(idrWaitReason) X(idrWaitStartedUs) X(idrWaitReleases) \
    X(idrWaitLastUs) X(idrWaitMaxUs) X(idrWaitRetryRequests) X(idrDropLimitRequests) \
    X(lastIdrRequestUs) X(idrDispatches) X(idrDispatchSuccesses) X(idrDispatchFailures) \
    X(lastIdrDispatchUs) X(idrFramesCompleted)

#define FOG_DIAG_ENUM(name) FOG_DIAG_##name,
typedef enum FogDiagCounter { FOG_DIAG_FIELDS(FOG_DIAG_ENUM) FOG_DIAG_COUNT } FogDiagCounter;
#undef FOG_DIAG_ENUM

void FogDiagReset(void);
void FogDiagAdd(FogDiagCounter counter, uint64_t amount);
void FogDiagVideoPacket(uint64_t bytes, uint64_t nowUs);
void FogDiagVideoPacketProcessing(uint64_t startUs, uint64_t endUs);
void FogDiagVideoReceiveCycle(uint64_t startUs, uint64_t waitStartUs,
                              uint64_t waitEndUs, uint64_t endUs);
void FogDiagVideoReceiveLoopGap(uint64_t previousEndUs, uint64_t startUs);
void FogDiagVideoReceiveAllocation(uint64_t startUs, uint64_t endUs);
void FogDiagVideoReceiveDecrypt(uint64_t startUs, uint64_t endUs);
void FogDiagVideoReceiveBufferRequested(uint64_t bytes);
void FogDiagVideoReceiveBufferSetResult(uint64_t bytes, unsigned int error);
void FogDiagVideoReceiveBufferActual(uint64_t bytes, unsigned int error);
// Count known unreceived trailing blocks when abandoning a partially built
// multi-FEC frame; a partial current block is counted separately by its caller.
void FogDiagFecFrameAbandoned(unsigned int pending, unsigned int completed,
                             unsigned int currentBlock, unsigned int lastBlock);
void FogDiagFecBlockAbandoned(unsigned int pending, unsigned int expectedData,
                              unsigned int receivedData, unsigned int expectedParity,
                              unsigned int receivedParity);
void FogDiagFecOosMode(uint64_t enabled);
void FogDiagSetWaitingForIdr(uint64_t waiting, uint64_t reason, uint64_t nowUs);
void FogDiagIdrRequested(uint64_t nowUs);
void FogDiagIdrDispatched(uint64_t nowUs);
void FogDiagIdrDispatchResult(uint64_t success, uint64_t nowUs);
void FogDiagIntactFrame(uint64_t frame, uint64_t blocks, uint64_t nowUs);
void FogDiagIdrHeader(uint64_t frame, uint64_t blocks, uint64_t nowUs);
void FogDiagIdrRecognized(uint64_t frame, uint64_t nowUs);
void FogDiagIdrAssembled(uint64_t frame, uint64_t nowUs);
void FogDiagFecFailure(uint64_t frame, uint64_t blocks, uint64_t nowUs);
void FogDiagFecFrameAbandonedWithId(uint64_t frame, uint64_t nowUs, unsigned int pending,
                                   unsigned int completed, unsigned int currentBlock, unsigned int lastBlock);
int FogDiagPayloadStartsIdr(const unsigned char* payload, int length, unsigned int header, int hevc);
void FogDiagThreadStarted(const char* role, int core, int ideal, uint64_t mask,
                          int priority, uint64_t queryErrors, uint64_t nowUs);
// Register once during depacketizer initialization before any core threads.
void FogDiagRegisterDecodeQueue(const void* queue);
// Called only under the queue's existing mutex, except after all users join
// at destruction. No queue state is dereferenced by the diagnostic functions.
void FogDiagQueueDepth(const void* queue, uint64_t depth);
void FogDiagQueueFlushed(const void* queue, uint64_t frames);

#endif
