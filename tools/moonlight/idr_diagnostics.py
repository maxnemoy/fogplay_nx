"""Observe IDR recovery stages without changing stream or recovery policy."""
import hashlib


def validate_frame_header_policy(source):
    # The receive hint below mirrors this exact pinned header-size policy.
    # Review both before accepting any upstream change, including new versions.
    start_marker = "        if (APP_VERSION_AT_LEAST(7, 1, 450)) {"
    end_marker = "        LC_ASSERT_VT(currentPos.length >= frameHeaderSize);"
    if source.count(start_marker) != 1 or source.count(end_marker) != 1:
        raise RuntimeError("Pinned frame-header policy changed: boundaries")
    start = source.index(start_marker)
    end = source.index(end_marker, start)
    actual = hashlib.sha256(source[start:end].encode()).hexdigest()
    if actual != "4d8266675e5c040627e4dcdd177aa6164703e9cd6dc551969babaf9d6f50f155":
        raise RuntimeError("Pinned frame-header policy changed: review IDR receive hint")


def apply(dest):
    validate_frame_header_policy((dest / "core" / "VideoDepacketizer.c").read_text())
    def replace(file, old, new, count=1):
        path = dest / "core" / file
        source = path.read_text()
        if source.count(old) != count:
            raise RuntimeError(f"Pinned IDR diagnostic boundary changed: {file}: {old[:70]!r}")
        path.write_text(source.replace(old, new))

    replace("VideoDepacketizer.c", "waitingForIdrFrame = false; FogDiagSetWaitingForIdr(0, 0, PltGetMicroseconds());",
            "FogDiagIdrRecognized(nextFrameNumber, PltGetMicroseconds());\n"
            "            waitingForIdrFrame = false; FogDiagSetWaitingForIdr(0, 0, PltGetMicroseconds());", 2)
    replace("VideoDepacketizer.c", "                FogDiagAdd(FOG_DIAG_idrFramesCompleted, 1);",
            "                FogDiagIdrAssembled((uint32_t)frameNumber, PltGetMicroseconds());\n"
            "                FogDiagAdd(FOG_DIAG_idrFramesCompleted, 1);")
    replace("RtpVideoQueue.c", "static void submitCompletedFrame(PRTP_VIDEO_QUEUE queue) {",
            "static void submitCompletedFrame(PRTP_VIDEO_QUEUE queue) {\n"
            "    FogDiagIntactFrame(queue->currentFrameNumber, queue->multiFecLastBlockNumber + 1, PltGetMicroseconds());")
    # Record failed-block counts at the same exact branches as the cumulative
    # counters. Detailed events are admitted only inside an active IDR wait;
    # the initiating loss remains visible through cumulative loss counters.
    replace("RtpVideoQueue.c", "            FogDiagAdd(FOG_DIAG_fecBlocksFailed, 1);",
            "            FogDiagFecFailure(queue->currentFrameNumber, 1, PltGetMicroseconds());\n"
            "            FogDiagAdd(FOG_DIAG_fecBlocksFailed, 1);")
    replace("RtpVideoQueue.c", "                FogDiagAdd(FOG_DIAG_fecBlocksFailed, fecCurrentBlockNumber - expectedFecBlockNumber);",
            "                FogDiagFecFailure(nvPacket->frameIndex, fecCurrentBlockNumber - expectedFecBlockNumber, PltGetMicroseconds());\n"
            "                FogDiagAdd(FOG_DIAG_fecBlocksFailed, fecCurrentBlockNumber - expectedFecBlockNumber);")
    path = dest / "core" / "RtpVideoQueue.c"
    source = path.read_text()
    import re
    pattern = r"FogDiagFecFrameAbandoned\(queue->pendingFecBlockList.count,\s*queue->completedFecBlockList.count,\s*queue->multiFecCurrentBlockNumber,\s*queue->multiFecLastBlockNumber\);"
    source, count = re.subn(pattern,
        "FogDiagFecFrameAbandonedWithId(queue->currentFrameNumber, PltGetMicroseconds(), "
        "queue->pendingFecBlockList.count, queue->completedFecBlockList.count, "
        "queue->multiFecCurrentBlockNumber, queue->multiFecLastBlockNumber);", source)
    if count != 3:
        raise RuntimeError("Pinned abandoned-frame boundaries changed")
    path.write_text(source)
    replace("RtpVideoQueue.c", "    // Reinitialize the queue if it's empty after a frame delivery or",
            "    // Raw, accepted-window first data packet: may identify an IDR even\n"
            "    // when later FEC blocks never complete. Absence is inconclusive.\n"
            "    if (fecIndex == 0 && fecCurrentBlockNumber == 0 && (nvPacket->flags & FLAG_SOF)) {\n"
            "        const unsigned char* payload = (const unsigned char*)(nvPacket + 1);\n"
            "        const int payloadLength = length - dataOffset - sizeof(*nvPacket);\n"
            "        unsigned int header = 0;\n"
            "        if (APP_VERSION_AT_LEAST(7, 1, 450)) header = payloadLength > 0 && payload[0] == 1 ? 8 : 44;\n"
            "        else if (APP_VERSION_AT_LEAST(7, 1, 446)) header = payloadLength > 0 && payload[0] == 1 ? 8 : 41;\n"
            "        else if (APP_VERSION_AT_LEAST(7, 1, 415)) header = payloadLength > 0 && payload[0] == 1 ? 8 : 24;\n"
            "        else if (APP_VERSION_AT_LEAST(7, 1, 350)) header = 8;\n"
            "        else if (APP_VERSION_AT_LEAST(7, 1, 320)) header = 12;\n"
            "        else if (APP_VERSION_AT_LEAST(5, 0, 0)) header = 8;\n"
            "        const bool headerIdr = APP_VERSION_AT_LEAST(7, 1, 350) && payloadLength >= 4 && payload[3] == 2;\n"
            "        if (headerIdr || FogDiagPayloadStartsIdr(payload, payloadLength, header,\n"
            "                (NegotiatedVideoFormat & VIDEO_FORMAT_MASK_H265) != 0)) {\n"
            "            FogDiagIdrHeader(nvPacket->frameIndex, ((nvPacket->multiFecBlocks >> 6) & 3) + 1, PltGetMicroseconds());\n"
            "        }\n"
            "    }\n\n"
            "    // Reinitialize the queue if it's empty after a frame delivery or")
    replace("ControlStream.c", "            FogDiagAdd(FOG_DIAG_idrDispatchFailures, 1);",
            "            FogDiagIdrDispatchResult(0, PltGetMicroseconds());\n"
            "            FogDiagAdd(FOG_DIAG_idrDispatchFailures, 1);", 2)
    replace("ControlStream.c", "    FogDiagAdd(FOG_DIAG_idrDispatchSuccesses, 1);",
            "    FogDiagIdrDispatchResult(1, PltGetMicroseconds());\n"
            "    FogDiagAdd(FOG_DIAG_idrDispatchSuccesses, 1);")
    replace("PlatformStuff.c", '#include "Limelight-internal.h"',
            '#include "Limelight-internal.h"\n#include "FogMoonlightDiagnosticsInternal.h"')
    replace("PlatformStuff.c", "    applySwitchThreadHints(ctx->name);",
            "    applySwitchThreadHints(ctx->name);\n"
            "    s32 fogIdeal = -1, fogPriority = -1;\n"
            "    u64 fogMask = 0;\n"
            "    const Result fogAffinityResult = svcGetThreadCoreMask(&fogIdeal, &fogMask, CUR_THREAD_HANDLE);\n"
            "    const Result fogPriorityResult = svcGetThreadPriority(&fogPriority, CUR_THREAD_HANDLE);\n"
            "    FogDiagThreadStarted(ctx->name, svcGetCurrentProcessorNumber(), fogIdeal, fogMask, fogPriority,\n"
            "                         (R_FAILED(fogAffinityResult) ? 1 : 0) | (R_FAILED(fogPriorityResult) ? 2 : 0), PltGetMicroseconds());")
