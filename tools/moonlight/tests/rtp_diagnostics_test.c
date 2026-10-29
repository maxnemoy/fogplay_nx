// Exercise the entire prepared RTP/FEC queue with its pinned nanors algorithm.
// Only clock, downstream delivery, loss notification and logging are test ports.
#include "Limelight-internal.h"
#include "FogMoonlightDiagnosticsInternal.h"
#undef NDEBUG // Keep harness checks enabled while the queue uses release policy.
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>

STREAM_CONFIGURATION StreamConfig;
CONNECTION_LISTENER_CALLBACKS ListenerCallbacks;
int AppVersionQuad[4] = {7, 1, 431, -1};
int NegotiatedVideoFormat = VIDEO_FORMAT_H264;
static uint64_t clockUs = 1000;
static unsigned delivered, speculativeLosses;
static bool sendIdrHint;
uint64_t PltGetMicroseconds(void) { return ++clockUs; }
void connectionSawFrame(uint32_t frameIndex) { (void)frameIndex; }
void connectionSendFrameFecStatus(PSS_FRAME_FEC_STATUS status) { (void)status; }
void notifyFrameLost(unsigned int frameNumber, bool speculative) {
    (void)frameNumber;
    if (speculative) speculativeLosses++;
}
void queueRtpPacket(PRTPV_QUEUE_ENTRY entry) { delivered++; free(entry->packet); }
static void logMessage(const char* format, ...) { (void)format; }

static FogMoonlightDiagnostics snapshot(void) {
    FogMoonlightDiagnostics result;
    FogMoonlightGetDiagnostics(&result);
    return result;
}
static void initialize(RTP_VIDEO_QUEUE* queue) {
    FogDiagReset();
    delivered = speculativeLosses = 0;
    RtpvInitializeQueue(queue);
}
static int packet(RTP_VIDEO_QUEUE* queue, uint16_t sequence, uint32_t frameIndex,
                  unsigned index, unsigned data, unsigned fecPercent,
                  unsigned block, unsigned lastBlock, int length, uint32_t timestamp) {
    const int receiveSize = StreamConfig.packetSize + MAX_RTP_HEADER_SIZE;
    char* buffer = calloc(1, receiveSize + sizeof(RTPV_QUEUE_ENTRY));
    assert(buffer);
    PRTP_PACKET rtp = (PRTP_PACKET)buffer;
    rtp->header = FLAG_EXTENSION;
    rtp->sequenceNumber = sequence; rtp->timestamp = timestamp;
    PNV_VIDEO_PACKET nv = (PNV_VIDEO_PACKET)(buffer + sizeof(*rtp) + 4);
    nv->frameIndex = LE32(frameIndex);
    nv->flags = FLAG_SOF | FLAG_EOF;
    nv->multiFecBlocks = (uint8_t)(((lastBlock << 2) | block) << 4);
    nv->fecInfo = LE32((data << 22) | (index << 12) | (fecPercent << 4));
    if (sendIdrHint && index == 0 && block == 0) {
        unsigned char* payload = (unsigned char*)(nv + 1);
        payload[0] = 1; payload[3] = 104; // Sunshine; no useful type in header
        payload[8] = 0; payload[9] = 0; payload[10] = 1; payload[11] = 0x67;
    }
    const int result = RtpvAddPacket(queue, rtp, length < 0 ? receiveSize : length,
                                    (PRTPV_QUEUE_ENTRY)(buffer + receiveSize));
    if (result == RTPF_RET_REJECTED) free(buffer);
    return result;
}
static int submit(RTP_VIDEO_QUEUE* queue, uint16_t seq, uint32_t frame, unsigned index,
                unsigned data, unsigned fec, unsigned block, unsigned last) {
    return packet(queue, seq, frame, index, data, fec, block, last, -1, 90000);
}

static void rejectsAndWrap(void) {
    RTP_VIDEO_QUEUE queue;
    initialize(&queue);
    queue.nextContiguousSequenceNumber = 65534;
    assert(submit(&queue, 65534, 1, 0, 2, 0, 0, 0) == RTPF_RET_QUEUED);
    assert(submit(&queue, 65535, 1, 1, 2, 0, 0, 0) == RTPF_RET_QUEUED);
    assert(submit(&queue, 0, 2, 0, 2, 0, 0, 0) == RTPF_RET_QUEUED);
    assert(submit(&queue, 1, 2, 1, 2, 0, 0, 0) == RTPF_RET_QUEUED);
    assert(delivered == 4 && queue.currentFrameNumber == 3);
    assert(submit(&queue, 65535, 2, 1, 2, 0, 0, 0) == RTPF_RET_REJECTED);
    assert(snapshot().videoPacketsRejectedSequence == 1);
    assert(snapshot().videoSequenceGapPackets == 0 && snapshot().videoPacketsOutOfOrder == 0);
    RtpvCleanupQueue(&queue);

    initialize(&queue);
    queue.currentFrameNumber = 5; queue.nextContiguousSequenceNumber = 100;
    assert(submit(&queue, 100, 4, 0, 4, 0, 0, 0) == RTPF_RET_REJECTED);
    queue.multiFecCurrentBlockNumber = 1;
    assert(submit(&queue, 100, 5, 0, 4, 0, 0, 1) == RTPF_RET_REJECTED);
    assert(packet(&queue, 100, 5, 0, 4, 0, 1, 1,
                  sizeof(RTP_PACKET) + 4 + sizeof(NV_VIDEO_PACKET) - 1, 90000) == RTPF_RET_REJECTED);
    assert(snapshot().videoPacketsRejectedFrame == 1 && snapshot().videoPacketsRejectedBlock == 1);
    assert(snapshot().videoPacketsRejectedHeader == 1);
    RtpvCleanupQueue(&queue);

    initialize(&queue);
    assert(submit(&queue, 100, 1, 0, 4, 0, 0, 0) == RTPF_RET_QUEUED);
    assert(submit(&queue, 110, 1, 10, 4, 0, 0, 0) == RTPF_RET_REJECTED);
    assert(snapshot().videoPacketsRejectedRange == 1);
    RtpvCleanupQueue(&queue);
}
static void reorderingAndPrediction(void) {
    RTP_VIDEO_QUEUE queue;
    initialize(&queue);
    assert(submit(&queue, 100, 1, 0, 4, 0, 0, 0) == RTPF_RET_QUEUED);
    assert(submit(&queue, 102, 1, 2, 4, 0, 0, 0) == RTPF_RET_QUEUED);
    assert(snapshot().videoSequenceGapPackets == 1 && speculativeLosses == 1);
    assert(submit(&queue, 102, 1, 2, 4, 0, 0, 0) == RTPF_RET_REJECTED);
    assert(snapshot().videoPacketsRejectedDuplicate == 1);
    assert(submit(&queue, 101, 1, 1, 4, 0, 0, 0) == RTPF_RET_QUEUED);
    assert(submit(&queue, 103, 1, 3, 4, 0, 0, 0) == RTPF_RET_QUEUED);
    assert(delivered == 4 && snapshot().fecBlocksCompleted == 1);
    assert(snapshot().videoPacketsOutOfOrder == 1 && snapshot().videoPacketsGapFill == 1);
    assert(snapshot().fecSpeculativeLossPredictions == 1 && snapshot().fecOosMode == 1);
    assert(snapshot().fecAbandonedCurrentBlocks == 0); // transient gap wasn't network-loss proof
    assert(packet(&queue, 104, 2, 0, 2, 0, 0, 0, -1, 27100000) == RTPF_RET_QUEUED);
    assert(snapshot().fecSpeculativeModeResumes == 1 && snapshot().fecOosMode == 0);
    RtpvCleanupQueue(&queue);

    initialize(&queue);
    assert(submit(&queue, 100, 1, 0, 2, 0, 0, 0) == RTPF_RET_QUEUED);
    // Force only the pinned predictor's remembered state, then execute its
    // actual successful reconstruction threshold and false-prediction branch.
    queue.reportedLostFrame = true;
    assert(submit(&queue, 101, 1, 1, 2, 0, 0, 0) == RTPF_RET_QUEUED);
    assert(snapshot().fecSpeculativeLossMispredictions == 1 && snapshot().fecOosMode == 1);
    RtpvCleanupQueue(&queue);
}
static void abandonedBlocks(void) {
    RTP_VIDEO_QUEUE queue;
    initialize(&queue);
    assert(submit(&queue, 100, 1, 0, 4, 25, 0, 1) == RTPF_RET_QUEUED);
    assert(submit(&queue, 105, 1, 0, 4, 25, 1, 1) == RTPF_RET_REJECTED);
    FogMoonlightDiagnostics value = snapshot();
    assert(value.videoPacketsRejectedTransition == 1 && value.fecBlocksFailed == 2);
    assert(value.fecAbandonedCurrentBlocks == 1 && value.fecAbandonedPacketsQueued == 1);
    assert(value.fecAbandonedDataExpected == 4 && value.fecAbandonedDataReceived == 1 && value.fecAbandonedDataMissing == 3);
    assert(value.fecAbandonedParityExpected == 1 && value.fecAbandonedParityReceived == 0 && value.fecAbandonedParityMissing == 1);
    assert(value.fecAbandonedMissingDataMax == 3 && value.fecAbandonedMissingParityMax == 1);
    // An actual late shard after cursor advance is rejected before it can fill
    // the old block; its missing-data statistic remains an abandonment snapshot.
    assert(submit(&queue, 101, 1, 1, 4, 25, 0, 1) == RTPF_RET_REJECTED);
    assert(snapshot().videoPacketsRejectedFrame == 1 && snapshot().fecAbandonedDataMissing == 3);
    RtpvCleanupQueue(&queue);

    initialize(&queue);
    assert(submit(&queue, 100, 1, 0, 4, 25, 0, 0) == RTPF_RET_QUEUED);
    assert(submit(&queue, 104, 1, 4, 4, 25, 0, 0) == RTPF_RET_QUEUED); // parity arrived, 3 data shards absent
    assert(submit(&queue, 105, 2, 0, 2, 0, 0, 0) == RTPF_RET_QUEUED);
    value = snapshot();
    assert(value.fecAbandonedCurrentBlocks == 1 && value.fecAbandonedPacketsQueued == 2);
    assert(value.fecAbandonedDataMissing == 3 && value.fecAbandonedParityMissing == 0);
    assert(value.fecAbandonedParityReceived == 1 && value.fecBlocksFailed == 1);
    RtpvCleanupQueue(&queue);

    initialize(&queue);
    assert(submit(&queue, 100, 2, 0, 4, 25, 1, 1) == RTPF_RET_REJECTED);
    assert(snapshot().videoPacketsRejectedTransition == 1 && snapshot().fecAbandonedCurrentBlocks == 0);
    RtpvCleanupQueue(&queue);

    initialize(&queue);
    assert(submit(&queue, 100, 1, 0, 2, 0, 0, 1) == RTPF_RET_QUEUED);
    assert(submit(&queue, 101, 1, 1, 2, 0, 0, 1) == RTPF_RET_QUEUED);
    assert(queue.multiFecCurrentBlockNumber == 1 && delivered == 0);
    assert(submit(&queue, 102, 1, 0, 2, 0, 1, 1) == RTPF_RET_QUEUED);
    assert(submit(&queue, 101, 1, 1, 2, 0, 0, 1) == RTPF_RET_REJECTED);
    assert(submit(&queue, 103, 1, 1, 2, 0, 1, 1) == RTPF_RET_QUEUED);
    assert(delivered == 4 && snapshot().fecBlocksCompleted == 2 && snapshot().fecAbandonedCurrentBlocks == 0);
    assert(snapshot().videoPacketsRejectedSequence == 1);
    RtpvCleanupQueue(&queue);
}
static void damagedIdrEvidence(void) {
    uint64_t cursor = 0, missed = 0;
    FogMoonlightEvent event;
    while (FogMoonlightReadEvent(&cursor, &event, &missed)) {}
    missed = 0;
    RTP_VIDEO_QUEUE queue;
    initialize(&queue);
    FogDiagSetWaitingForIdr(1, 8, 1000);
    FogDiagIdrRequested(1010);
    sendIdrHint = true;
    assert(submit(&queue, 100, 1, 0, 2, 0, 0, 2) == RTPF_RET_QUEUED);
    sendIdrHint = false;
    // The raw IDR prefix arrived, but none of its 3 FEC blocks can finish.
    assert(submit(&queue, 106, 2, 0, 1, 0, 0, 0) == RTPF_RET_QUEUED);
    assert(snapshot().fecBlocksFailed == 3);
    unsigned hints = 0, intact = 0;
    uint64_t failures = 0;
    while (FogMoonlightReadEvent(&cursor, &event, &missed)) {
        assert(event.waitId == 1);
        if (event.kind == FOG_EVENT_IDR_HEADER) {
            ++hints; assert(event.frameNumber == 1 && event.value == 3);
        }
        if (event.kind == FOG_EVENT_FEC_FAILURE) {
            assert(event.frameNumber == 1); failures += event.value;
        }
        if (event.kind == FOG_EVENT_INTACT_FRAME) {
            ++intact; assert(event.frameNumber == 2 && event.value == 1);
            assert(event.requests == 1 && event.intactFrames == 1 && event.fecFailures == 3);
        }
        assert(event.kind != FOG_EVENT_IDR_ASSEMBLED);
    }
    assert(hints == 1 && failures == 3 && intact == 1 && !missed);
    RtpvCleanupQueue(&queue);
}
static void idleLossKeepsCumulativeCountersOnly(void) {
    uint64_t cursor = 0, missed = 0;
    FogMoonlightEvent event;
    while (FogMoonlightReadEvent(&cursor, &event, &missed)) {}
    missed = 0;
    RTP_VIDEO_QUEUE queue;
    initialize(&queue);
    assert(submit(&queue, 100, 1, 0, 2, 0, 0, 2) == RTPF_RET_QUEUED);
    assert(submit(&queue, 106, 2, 0, 1, 0, 0, 0) == RTPF_RET_QUEUED);
    assert(snapshot().fecBlocksFailed == 3 && snapshot().fecAbandonedCurrentBlocks == 1);
    // Actual prepared queue failure hooks are silent while not awaiting IDR.
    assert(!FogMoonlightReadEvent(&cursor, &event, &missed) && missed == 0);
    FogDiagSetWaitingForIdr(1, 8, 2000);
    assert(submit(&queue, 107, 3, 0, 2, 0, 0, 2) == RTPF_RET_QUEUED);
    assert(submit(&queue, 113, 4, 0, 1, 0, 0, 0) == RTPF_RET_QUEUED);
    assert(snapshot().fecBlocksFailed == 6 && snapshot().fecAbandonedCurrentBlocks == 2);
    uint64_t failed = 0;
    while (FogMoonlightReadEvent(&cursor, &event, &missed)) {
        assert(event.waitId == 1);
        if (event.kind == FOG_EVENT_FEC_FAILURE) {
            assert(event.frameNumber == 3); failed += event.value;
        }
        if (event.kind == FOG_EVENT_INTACT_FRAME) assert(event.fecFailures == 3 && event.intactFrames == 1);
    }
    assert(failed == 3 && missed == 0);
    RtpvCleanupQueue(&queue);
}
int main(void) {
    StreamConfig.packetSize = 64;
    ListenerCallbacks.logMessage = logMessage;
    rejectsAndWrap(); reorderingAndPrediction(); abandonedBlocks(); damagedIdrEvidence();
    idleLossKeepsCumulativeCountersOnly();
    puts("Prepared RTP/FEC diagnostics passed: cursor rejects, sequence wrap, duplicates/gap fill, speculative modes and abandonment shard evidence");
    return 0;
}
