"""Fail-closed instrumentation of the pinned core; no stream policy changes."""
import re


def apply(dest):
    def replace(file, old, new, count=1):
        path = dest / "core" / file
        text = path.read_text()
        if text.count(old) != count:
            raise RuntimeError(f"Pinned diagnostic source mismatch: {file}: {old[:70]!r}")
        path.write_text(text.replace(old, new))

    for file in ("Connection.c", "VideoStream.c", "RtpVideoQueue.c",
                 "VideoDepacketizer.c", "ControlStream.c", "PlatformSockets.c"):
        replace(file, '#include "Limelight-internal.h"',
                '#include "Limelight-internal.h"\n#include "FogMoonlightDiagnosticsInternal.h"')
    replace("LinkedBlockingQueue.c", '#include "LinkedBlockingQueue.h"',
            '#include "LinkedBlockingQueue.h"\n#include "FogMoonlightDiagnosticsInternal.h"')

    replace("Connection.c", "    void* audioContext, int arFlags) {\n    int err;",
            "    void* audioContext, int arFlags) {\n    int err;\n\n    FogDiagReset();")
    replace("VideoStream.c", "        if (err < 0) {\n            Limelog(\"Video Receive:",
            "        if (err < 0) {\n            FogDiagAdd(FOG_DIAG_videoReceiveErrors, 1);\n            Limelog(\"Video Receive:")
    replace("VideoStream.c", "        if (!receivedDataFromPeer) {\n            receivedDataFromPeer = true;",
            "        FogDiagVideoPacket((uint64_t)err, PltGetMicroseconds());\n\n"
            "        if (!receivedDataFromPeer) {\n            receivedDataFromPeer = true;")
    replace("VideoStream.c", "            // Runt packet\n            continue;",
            "            // Runt packet\n            FogDiagAdd(FOG_DIAG_videoRuntPackets, 1);\n            continue;")
    replace("VideoStream.c", '                Limelog("Failed to decrypt video packet!\\n");',
            '                FogDiagAdd(FOG_DIAG_videoDecryptFailures, 1);\n'
            '                Limelog("Failed to decrypt video packet!\\n");')
    replace("VideoStream.c", "        queueStatus = RtpvAddPacket(&rtpQueue, packet, err, (PRTPV_QUEUE_ENTRY)&buffer[decryptedSize]);",
            "        const uint64_t packetProcessingStartUs = PltGetMicroseconds();\n"
            "        queueStatus = RtpvAddPacket(&rtpQueue, packet, err, (PRTPV_QUEUE_ENTRY)&buffer[decryptedSize]);\n"
            "        FogDiagVideoPacketProcessing(packetProcessingStartUs, PltGetMicroseconds());")

    # Account every loop exit, including timeout/error/runt/stale/decrypt-failure.
    # Keep allocation, decryption and RTP policy exactly as the pinned source.
    path = dest / "core" / "VideoStream.c"
    video = path.read_text()
    loop_start = video.index("    while (!PltIsThreadInterrupted(&receiveThread)) {")
    loop_end = video.index("\n    if (buffer != NULL) {\n        free(buffer);", loop_start)
    loop = video[loop_start:loop_end]
    finish = ("previousCycleEndUs = PltGetMicroseconds();\n"
              "FogDiagVideoReceiveCycle(cycleStartUs, waitStartUs, waitEndUs, previousCycleEndUs);\n")
    if loop.count("continue;") != 4 or loop.count("break;") != 4:
        raise RuntimeError("Pinned receive loop exit mismatch")
    loop, exits = re.subn(r"(?m)^( +)(continue;|break;)$",
                         lambda match: "".join(match[1] + line + "\n" for line in finish.splitlines()) + match[0], loop)
    if exits != 8:
        raise RuntimeError("Pinned receive loop statement mismatch")

    def replace_loop(old, new):
        nonlocal loop
        if loop.count(old) != 1:
            raise RuntimeError(f"Pinned receive timing source mismatch: {old[:70]!r}")
        loop = loop.replace(old, new)

    replace_loop("        PRTP_PACKET packet;", """        PRTP_PACKET packet;
        const uint64_t cycleStartUs = PltGetMicroseconds();
        uint64_t waitStartUs = cycleStartUs, waitEndUs = cycleStartUs;
        FogDiagVideoReceiveLoopGap(previousCycleEndUs, cycleStartUs);""")
    if loop.count("            buffer = (char*)malloc(bufferSize);") != 1:
        raise RuntimeError("Pinned receive allocation mismatch")
    replace_loop("            buffer = (char*)malloc(bufferSize);", """            const uint64_t allocationStartUs = PltGetMicroseconds();
            buffer = (char*)malloc(bufferSize);
            FogDiagVideoReceiveAllocation(allocationStartUs, PltGetMicroseconds());""")
    replace_loop("        err = recvUdpSocket(rtpSocket,", "        waitStartUs = PltGetMicroseconds();\n        err = recvUdpSocket(rtpSocket,")
    replace_loop("                            useSelect);", """                            useSelect);
        const int receiveSocketError = err < 0 ? (int)LastSocketError() : 0;
        waitEndUs = PltGetMicroseconds();
        if (err < 0) SetLastSocketError(receiveSocketError);""")
    # Time both successful and failed decryption, without moving early stale drops.
    replace_loop("            if (!PltDecryptMessage(", "            const uint64_t decryptStartUs = PltGetMicroseconds();\n            const bool decryptedOk = PltDecryptMessage(")
    replace_loop("                                   (unsigned char*)buffer, &err)) {", """                                   (unsigned char*)buffer, &err);
            FogDiagVideoReceiveDecrypt(decryptStartUs, PltGetMicroseconds());
            if (!decryptedOk) {""")
    if not loop.endswith("    }\n"):
        raise RuntimeError("Pinned receive loop boundary mismatch")
    loop = loop[:-6] + "".join("        " + line + "\n" for line in finish.splitlines()) + "    }\n"
    video = video[:loop_start] + "    uint64_t previousCycleEndUs = 0;\n" + loop + video[loop_end:]
    path.write_text(video)

    # Only publish the video socket. Keep all pinned SO_RCVBUF attempts, decrements
    # and debug logging unchanged. A separate query also works in release builds.
    replace("PlatformSockets.c", "    if (bufferSize != 0) {\n        // We start at the requested recv buffer value",
            "    if (socketQosType == SOCK_QOS_TYPE_VIDEO) {\n"
            "        FogDiagVideoReceiveBufferRequested(bufferSize);\n    }\n"
            "    if (bufferSize != 0) {\n        // We start at the requested recv buffer value")
    replace("PlatformSockets.c", "            err = setsockopt(s, SOL_SOCKET, SO_RCVBUF, (char*)&bufferSize, sizeof(bufferSize));",
            "            err = setsockopt(s, SOL_SOCKET, SO_RCVBUF, (char*)&bufferSize, sizeof(bufferSize));\n"
            "            if (socketQosType == SOCK_QOS_TYPE_VIDEO) {\n"
            "                FogDiagVideoReceiveBufferSetResult(bufferSize, err == 0 ? 0 : (unsigned int)LastSocketError());\n"
            "            }")
    replace("PlatformSockets.c", "    return s;\n}\n\nint setSocketNonBlocking", """    if (socketQosType == SOCK_QOS_TYPE_VIDEO) {
        int actualBufferSize = 0;
        SOCKADDR_LEN actualLength = sizeof(actualBufferSize);
        const int queryResult = getsockopt(s, SOL_SOCKET, SO_RCVBUF, (char*)&actualBufferSize, &actualLength);
        FogDiagVideoReceiveBufferActual(actualBufferSize > 0 ? (uint64_t)actualBufferSize : 0,
                                        queryResult == 0 ? 0 : (unsigned int)LastSocketError());
    }
    return s;
}

int setSocketNonBlocking""")

    # Count completed/abandoned FEC blocks, not every unsuccessful attempt to
    # reconstruct while more packets are still expected. Missing whole frames
    # have an unknown block count and are intentionally not guessed here.
    # Observe existing reject paths only: do not add validation, reorder grace,
    # recovery attempts, or alter which packet advances the pinned core cursor.
    for comment, counter in (
            ("Reject packets behind our current buffer window", "videoPacketsRejectedSequence"),
            ("Reject packets that are too small to fit a NV_VIDEO_PACKET header", "videoPacketsRejectedHeader"),
            ("Reject frames behind our current frame number", "videoPacketsRejectedFrame"),
            ("Reject FEC blocks behind our current block number", "videoPacketsRejectedBlock")):
        replace("RtpVideoQueue.c", f"        // {comment}\n        return RTPF_RET_REJECTED;",
                f"        // {comment}\n        FogDiagAdd(FOG_DIAG_{counter}, 1);\n        return RTPF_RET_REJECTED;")
    replace("RtpVideoQueue.c", "    if (isBefore16(queue->bufferHighestSequenceNumber, packet->sequenceNumber)) {\n        return RTPF_RET_REJECTED;",
            "    if (isBefore16(queue->bufferHighestSequenceNumber, packet->sequenceNumber)) {\n"
            "        FogDiagAdd(FOG_DIAG_videoPacketsRejectedRange, 1);\n        return RTPF_RET_REJECTED;")
    replace("RtpVideoQueue.c", "            if (packet->sequenceNumber == entry->packet->sequenceNumber) {\n                return false;",
            "            if (packet->sequenceNumber == entry->packet->sequenceNumber) {\n"
            "                if (!isFecRecovery) FogDiagAdd(FOG_DIAG_videoPacketsRejectedDuplicate, 1);\n                return false;")
    replace("RtpVideoQueue.c", "                    queue->currentFrameNumber++;\n                    queue->multiFecCurrentBlockNumber = 0;\n                    return RTPF_RET_REJECTED;",
            "                    queue->currentFrameNumber++;\n                    queue->multiFecCurrentBlockNumber = 0;\n"
            "                    FogDiagAdd(FOG_DIAG_videoPacketsRejectedTransition, 1);\n                    return RTPF_RET_REJECTED;")
    replace("RtpVideoQueue.c", "            queue->currentFrameNumber = nvPacket->frameIndex + 1;\n            queue->multiFecCurrentBlockNumber = 0;\n            return RTPF_RET_REJECTED;",
            "            queue->currentFrameNumber = nvPacket->frameIndex + 1;\n            queue->multiFecCurrentBlockNumber = 0;\n"
            "            FogDiagAdd(FOG_DIAG_videoPacketsRejectedTransition, 1);\n            return RTPF_RET_REJECTED;")
    replace("RtpVideoQueue.c", "        if (outOfSequence) {\n            // This packet was received",
            "        if (outOfSequence) {\n            FogDiagAdd(FOG_DIAG_videoPacketsOutOfOrder, 1);\n            // This packet was received")
    for expression in ("packet->sequenceNumber - queue->bufferLowestSequenceNumber",
                       "packet->sequenceNumber - queue->receivedHighestSequenceNumber - 1"):
        replace("RtpVideoQueue.c", f"            queue->missingPackets += U16({expression});",
                f"            const unsigned int observedGap = U16({expression});\n"
                "            if (observedGap) FogDiagAdd(FOG_DIAG_videoSequenceGapPackets, observedGap);\n"
                f"            queue->missingPackets += U16({expression});")
    replace("RtpVideoQueue.c", "            queue->missingPackets--;",
            "            FogDiagAdd(FOG_DIAG_videoPacketsGapFill, 1);\n            queue->missingPackets--;")
    replace("RtpVideoQueue.c", "                notifyFrameLost(queue->currentFrameNumber, true);",
            "                FogDiagAdd(FOG_DIAG_fecSpeculativeLossPredictions, 1);\n"
            "                notifyFrameLost(queue->currentFrameNumber, true);")
    replace("RtpVideoQueue.c", "    if (queue->reportedLostFrame && !queue->receivedOosData) {",
            "    if (queue->reportedLostFrame && !queue->receivedOosData) {\n"
            "        FogDiagAdd(FOG_DIAG_fecSpeculativeLossMispredictions, 1);")
    replace("RtpVideoQueue.c", "queue->receivedOosData = true;",
            "queue->receivedOosData = true; FogDiagFecOosMode(1);", 2)
    replace("RtpVideoQueue.c", "            queue->receivedOosData = false;",
            "            queue->receivedOosData = false; FogDiagFecOosMode(0);\n"
            "            FogDiagAdd(FOG_DIAG_fecSpeculativeModeResumes, 1);")
    replace("RtpVideoQueue.c", "        if (reconstructFrame(queue) == 0) {",
            "        if (reconstructFrame(queue) == 0) {\n"
            "            FogDiagAdd(FOG_DIAG_fecBlocksCompleted, 1);")
    replace("RtpVideoQueue.c", "        if (queue->pendingFecBlockList.count != 0) {\n            // Report the final status",
            "        if (queue->currentFrameNumber != nvPacket->frameIndex) {\n"
            "            FogDiagFecFrameAbandoned(queue->pendingFecBlockList.count,\n"
            "                                     queue->completedFecBlockList.count,\n"
            "                                     queue->multiFecCurrentBlockNumber,\n"
            "                                     queue->multiFecLastBlockNumber);\n        }\n"
            "        if (queue->pendingFecBlockList.count != 0) {\n            // Report the final status")
    replace("RtpVideoQueue.c", "        if (queue->pendingFecBlockList.count != 0) {\n            // Report the final status",
            "        if (queue->pendingFecBlockList.count != 0) {\n"
            "            FogDiagAdd(FOG_DIAG_fecBlocksFailed, 1);\n"
            "            FogDiagFecBlockAbandoned(queue->pendingFecBlockList.count, queue->bufferDataPackets,\n"
            "                                     queue->receivedDataPackets, queue->bufferParityPackets, queue->receivedParityPackets);\n"
            "            // Report the final status")
    replace("RtpVideoQueue.c", "        if (fecCurrentBlockNumber != expectedFecBlockNumber) {",
            "        if (fecCurrentBlockNumber != expectedFecBlockNumber) {\n"
            "            if (queue->currentFrameNumber == nvPacket->frameIndex) {\n"
            "                FogDiagFecFrameAbandoned(queue->pendingFecBlockList.count,\n"
            "                                         queue->completedFecBlockList.count,\n"
            "                                         queue->multiFecCurrentBlockNumber,\n"
            "                                         queue->multiFecLastBlockNumber);\n            }\n"
            "            else {\n"
            "                FogDiagAdd(FOG_DIAG_fecBlocksFailed, fecCurrentBlockNumber - expectedFecBlockNumber);\n            }")
    replace("RtpVideoQueue.c", "                if (queue->currentFrameNumber == nvPacket->frameIndex) {\n                    // Discard any unsubmitted buffers",
            "                if (queue->currentFrameNumber == nvPacket->frameIndex) {\n"
            "                    FogDiagFecFrameAbandoned(queue->pendingFecBlockList.count,\n"
            "                                             queue->completedFecBlockList.count,\n"
            "                                             queue->multiFecCurrentBlockNumber,\n"
            "                                             queue->multiFecLastBlockNumber);\n"
            "                    // Discard any unsubmitted buffers")
    replace("RtpVideoQueue.c", "                queuePacket(queue, queueEntry, rtpPacket, StreamConfig.packetSize + dataOffset, false, true);",
            "                if (queuePacket(queue, queueEntry, rtpPacket, StreamConfig.packetSize + dataOffset, false, true)) {\n"
            "                    FogDiagAdd(FOG_DIAG_fecPacketsRecovered, 1);\n                }")
    replace("RtpVideoQueue.c", "    return ret;\n}\n\nstatic void stageCompleteFecBlock",
            "    if (ret != 0) {\n        FogDiagAdd(FOG_DIAG_fecReconstructionFailures, 1);\n    }\n"
            "    return ret;\n}\n\nstatic void stageCompleteFecBlock")

    replace("VideoDepacketizer.c", "    LbqInitializeLinkedBlockingQueue(&decodeUnitQueue, 15);",
            "    LbqInitializeLinkedBlockingQueue(&decodeUnitQueue, 15);\n"
            "    FogDiagRegisterDecodeQueue(&decodeUnitQueue);")
    # Record the entry cause once per wait; repeated dropped frames must not
    # reset the wait clock or hide which path first invalidated the chain.
    path = dest / "core" / "VideoDepacketizer.c"
    video = path.read_text()
    reasons = iter((1, 2, 3, 4, 5))
    def wait_state(match):
        value = match[1]
        reason = next(reasons) if value == "true" else 0
        return f"waitingForIdrFrame = {value}; FogDiagSetWaitingForIdr({1 if value == 'true' else 0}, {reason}, PltGetMicroseconds());"
    video, assignments = re.subn(r"waitingForIdrFrame = (true|false);", wait_state, video)
    if assignments != 7 or next(reasons, None) is not None:
        raise RuntimeError("Pinned IDR wait assignments changed")
    path.write_text(video)
    replace("VideoDepacketizer.c", '                    Limelog("Video decode unit queue overflow\\n");',
            '                    FogDiagAdd(FOG_DIAG_decodeQueueOverflows, 1);\n'
            '                    Limelog("Video decode unit queue overflow\\n");')
    replace("VideoDepacketizer.c", '                Limelog("Waiting for IDR frame\\n");',
            '                FogDiagAdd(FOG_DIAG_waitingForIdrDrops, 1);\n'
            '                Limelog("Waiting for IDR frame\\n");')
    replace("ControlStream.c", "void LiRequestIdrFrame(void) {",
            "void LiRequestIdrFrame(void) {\n"
            "    const uint64_t requestUs = PltGetMicroseconds();\n"
            "    FogIdrRecoveryRequested();\n"
            "    FogDiagIdrRequested(requestUs);")
    replace("ControlStream.c", "static void requestIdrFrame(void) {",
            "static void requestIdrFrame(void) {\n    FogDiagIdrDispatched(PltGetMicroseconds());")
    replace("ControlStream.c", '            Limelog("Request IDR Frame: Transaction failed: %d\\n", (int)LastSocketError());',
            '            FogDiagAdd(FOG_DIAG_idrDispatchFailures, 1);\n'
            '            Limelog("Request IDR Frame: Transaction failed: %d\\n", (int)LastSocketError());', 2)
    replace("ControlStream.c", '    Limelog("IDR frame request sent\\n");',
            '    FogDiagAdd(FOG_DIAG_idrDispatchSuccesses, 1);\n'
            '    Limelog("IDR frame request sent\\n");')
    replace("VideoDepacketizer.c", "                notifyKeyFrameReceived();",
            "                FogDiagAdd(FOG_DIAG_idrFramesCompleted, 1);\n                notifyKeyFrameReceived();")

    # Publish queue occupancy under the queue's existing mutex; snapshots never
    # call LbqGetItemCount(), which is an unsynchronized plain integer read.
    replace("LinkedBlockingQueue.c", "    // Save the old head\n    head = queueHead->head;",
            "    FogDiagQueueFlushed(queueHead, queueHead->currentSize);\n"
            "    FogDiagQueueDepth(queueHead, 0);\n\n    // Save the old head\n    head = queueHead->head;")
    replace("LinkedBlockingQueue.c", "    queueHead->currentSize++;",
            "    queueHead->currentSize++;\n    FogDiagQueueDepth(queueHead, queueHead->currentSize);")
    replace("LinkedBlockingQueue.c", "    queueHead->currentSize--;",
            "    queueHead->currentSize--;\n    FogDiagQueueDepth(queueHead, queueHead->currentSize);", 2)
    replace("LinkedBlockingQueue.c", "    PltDeleteMutex(&queueHead->mutex);\n    PltDeleteConditionVariable",
            "    FogDiagQueueDepth(queueHead, 0);\n    PltDeleteMutex(&queueHead->mutex);\n    PltDeleteConditionVariable")
