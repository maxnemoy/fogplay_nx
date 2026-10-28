"""Bounded IDR recovery applied only to copied, pinned core sources."""
def apply(dest):
    def replace(file, old, new, count=1):
        path = dest / "core" / file
        source = path.read_text()
        if source.count(old) != count:
            raise RuntimeError(f"Pinned recovery source mismatch: {file}: {old[:70]!r}")
        path.write_text(source.replace(old, new))

    for file in ("Connection.c", "ControlStream.c", "VideoDepacketizer.c"):
        replace(file, '#include "Limelight-internal.h"',
                '#include "Limelight-internal.h"\n#include "FogMoonlightRecovery.h"')
    replace("Connection.c", "    FogDiagReset();", "    FogDiagReset();\n    FogIdrRecoveryReset();")
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
    # Keep the original first-successful-frame request. Subsequent retries only
    # occur on complete discarded frames, so they cannot extend a frame, flush
    # the decoder, or pretend dependent P frames are safe reference frames.
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
