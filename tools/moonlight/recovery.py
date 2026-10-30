"""Bounded IDR recovery applied only to copied, pinned core sources."""
def apply(dest):
    def replace(file, old, new, count=1):
        path = dest / "core" / file
        source = path.read_text()
        if source.count(old) != count:
            raise RuntimeError(f"Pinned recovery source mismatch: {file}: {old[:70]!r}")
        path.write_text(source.replace(old, new))

    for file in ("Connection.c", "ControlStream.c", "VideoDepacketizer.c", "VideoStream.c"):
        replace(file, '#include "Limelight-internal.h"',
                '#include "Limelight-internal.h"\n#include "FogMoonlightRecovery.h"')
    replace("Connection.c", "    FogDiagReset();", "    FogDiagReset();\n    FogIdrRecoveryReset();")
    replace("VideoDepacketizer.c", "static bool waitingForIdrFrame;",
            "static bool waitingForIdrFrame;\nstatic bool localDecoderRecovery;")
    replace("VideoDepacketizer.c", "    nextFrameNumber = 1;",
            "    nextFrameNumber = 1;\n    localDecoderRecovery = false;")
    replace("VideoDepacketizer.c", "waitingForIdrFrame = false; FogDiagSetWaitingForIdr(0, 0, PltGetMicroseconds());",
            "localDecoderRecovery = false;\n            waitingForIdrFrame = false; FogDiagSetWaitingForIdr(0, 0, PltGetMicroseconds());", 2)
    # Enter the existing core decoder-refresh lifecycle on its receive owner.
    # LiRequestIdrFrame alone does not enable waitingForIdrFrame or its retry.
    replace("VideoDepacketizer.c", "// Return 1 if packet is the first one in the frame",
            "void FogIdrRecoveryPollDecoder(void) {\n"
            "    if (FogIdrRecoveryTakeDecoderRefresh()) {\n"
            "        localDecoderRecovery = true;\n"
            "        requestDecoderRefresh();\n"
            "    }\n"
            "    if (!waitingForIdrFrame) localDecoderRecovery = false;\n"
            "    if (localDecoderRecovery && waitingForIdrFrame && FogIdrRecoveryTryRetry()) {\n"
            "        FogDiagAdd(FOG_DIAG_idrWaitRetryRequests, 1);\n"
            "        LiRequestIdrFrame();\n"
            "    }\n"
            "}\n\n// Return 1 if packet is the first one in the frame")
    # Timeout servicing is limited to the wait explicitly opened by a local
    # decoder command. Network/startup waits retain their frame-driven policy.
    replace("VideoStream.c", "    while (!PltIsThreadInterrupted(&receiveThread)) {",
            "    while (!PltIsThreadInterrupted(&receiveThread)) {\n"
            "        FogIdrRecoveryPollDecoder();")
    # Thread the diagnostic cause through existing drop calls only; this does
    # not alter state decisions. Keep all seven pinned call sites fail closed.
    replace("VideoDepacketizer.c", "static void dropFrameState(void) {",
            "static void dropFrameState(unsigned int waitReason) {")
    replace("VideoDepacketizer.c", "FogDiagSetWaitingForIdr(1, 2, PltGetMicroseconds());",
            "FogDiagSetWaitingForIdr(1, waitReason, PltGetMicroseconds());")
    path = dest / "core" / "VideoDepacketizer.c"
    source = path.read_text()
    if source.count("dropFrameState();") != 7:
        raise RuntimeError("Pinned frame-drop call sites changed")
    for reason in ("4", "9", "6", "10", "2", "5", "speculative ? 7 : 8"):
        source = source.replace("dropFrameState();", f"dropFrameState({reason});", 1)
    path.write_text(source)
    # Current official common-c includes this safety guard: a partially parsed
    # IDR may already have released the wait, but losing it invalidates the
    # reference chain and requires another IDR even on an RFI-capable decoder.
    replace("VideoDepacketizer.c",
            "    if (strictIdrFrameWait || !idrFrameProcessed || waitingForIdrFrame) {",
            "    if (strictIdrFrameWait || !idrFrameProcessed || waitingForIdrFrame || (nalChainHead && frameType == FRAME_TYPE_IDR)) {")
    # Keep the original first-successful-frame request. Frame-driven retries
    # share the reservation with receive-loop timeout servicing above; these
    # retries do not flush the decoder or admit dependent P frames.
    replace("VideoDepacketizer.c", """                if (waitingForNextSuccessfulFrame) {
                    LiRequestIdrFrame();
                }""", """                if (waitingForNextSuccessfulFrame) {
                    LiRequestIdrFrame();
                }
                else if (FogIdrRecoveryTryRetry()) {
                    FogDiagAdd(FOG_DIAG_idrWaitRetryRequests, 1);
                    LiRequestIdrFrame();
                }""")
    # Retain the 120-drop escalation for RFI/continued incomplete frames, while
    # coalescing it with a recent bounded retry or decoder-requested IDR.
    replace("VideoDepacketizer.c", """        LiRequestIdrFrame();
    }

    cleanupFrameState();""", """        if (FogIdrRecoveryTryRetry()) {
            FogDiagAdd(FOG_DIAG_idrDropLimitRequests, 1);
            LiRequestIdrFrame();
        }
    }

    cleanupFrameState();""")
