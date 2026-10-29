#!/usr/bin/env python3
"""Check fail-closed patches, repeatable preparation, and native diagnostics."""
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
TOOLS = ROOT / "tools/moonlight"
CORE = ROOT / "third_party/moonlight/moonlight-switch/extern/moonlight-common-c/src"
NAMES = ("Connection.c", "VideoStream.c", "RtpVideoQueue.c", "VideoDepacketizer.c",
         "ControlStream.c", "LinkedBlockingQueue.c", "PlatformSockets.c", "PlatformStuff.c")
sys.path.insert(0, str(TOOLS))
from diagnostics import apply
from idr_diagnostics import apply as apply_idr_diagnostics, validate_frame_header_policy


def digests(directory):
    return {name: hashlib.sha256((directory / name).read_bytes()).hexdigest() for name in NAMES}


original = digests(CORE)
header_source = (CORE / "VideoDepacketizer.c").read_text()
validate_frame_header_policy(header_source)
with tempfile.TemporaryDirectory(prefix="moonlight-header-contract-") as directory:
    destination = Path(directory)
    (destination / "core").mkdir()
    # Change a pinned branch's size without touching any replacement anchor:
    # preparation must reject it before making even its first source edit.
    modified = header_source.replace("frameHeaderSize = 44;", "frameHeaderSize = 45;", 1)
    assert modified != header_source
    target = destination / "core/VideoDepacketizer.c"
    target.write_text(modified)
    try:
        apply_idr_diagnostics(destination)
    except RuntimeError as error:
        assert "Pinned frame-header policy changed" in str(error)
        assert target.read_text() == modified
    else:
        raise AssertionError("Changed pinned header policy must reject IDR instrumentation")
subprocess.run([sys.executable, str(TOOLS / "prepare.py")], check=True)
prepared = ROOT / "third_party/moonlight/prepared/core"
first = digests(prepared)
subprocess.run([sys.executable, str(TOOLS / "prepare.py")], check=True)
assert first == digests(prepared), "Preparation is not repeatable"
assert original == digests(CORE), "Pinned source was modified"
with tempfile.TemporaryDirectory(prefix="moonlight-diagnostics-") as directory:
    destination = Path(directory)
    (destination / "core").mkdir()
    for name in NAMES:
        (destination / "core" / name).write_bytes((prepared / name).read_bytes())
    try:
        apply(destination)
    except RuntimeError:
        pass
    else:
        raise AssertionError("Patch must reject already transformed/unexpected source")
    binary = destination / "core-diagnostics-test"
    subprocess.run([
        "cc", "-std=c11", "-DLC_DEBUG", "-DFOG_DIAG_TEST_HOOK", "-Wall", "-Wextra", "-Werror", "-pthread",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        f"-I{TOOLS}", f"-I{CORE}", str(TOOLS / "FogMoonlightDiagnostics.c"),
        str(prepared / "LinkedBlockingQueue.c"), str(TOOLS / "tests/core_diagnostics_test.c"),
        "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
    # Compile the full prepared receive/FEC policy with the actual pinned
    # nanors implementation; no queue algorithm or reject branch is mirrored.
    core_root = CORE.parent
    binary = destination / "rtp-diagnostics-test"
    subprocess.run([
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        f"-I{TOOLS}", f"-I{CORE}", f"-I{core_root / 'enet/include'}",
        f"-I{core_root / 'nanors'}", f"-I{core_root / 'nanors/deps/obl'}",
        str(TOOLS / "FogMoonlightDiagnostics.c"), str(prepared / "RtpVideoQueue.c"),
        str(TOOLS / "tests/rtp_diagnostics_test.c"), str(core_root / "nanors/rs.c"),
        str(core_root / "nanors/deps/obl/oblas_common.c"), str(core_root / "nanors/deps/obl/oblas_lite.c"),
        "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
    control = (prepared / "ControlStream.c").read_text()
    api_start = control.index("void LiRequestIdrFrame(void) {")
    api_end = control.index("\n// Invalidate reference frames lost by the network", api_start)
    dispatch_start = control.index("static void requestIdrFrame(void) {")
    dispatch_end = control.index("\nstatic void requestInvalidateReferenceFrames", dispatch_start)
    template = (TOOLS / "tests/idr_recovery_test.c").read_text()
    source = destination / "idr-recovery.c"
    source.write_text(template.replace("CONTROL_API", control[api_start:api_end])
                     .replace("CONTROL_DISPATCH", control[dispatch_start:dispatch_end]))
    binary = destination / "idr-recovery"
    subprocess.run([
        "cc", "-std=c11", "-DLC_DEBUG", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
        "-pthread", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-DFOG_IDR_RECOVERY_TEST_CLOCK",
        f"-I{TOOLS}", f"-I{prepared}", f"-I{CORE}", f"-I{core_root / 'enet/include'}",
        str(TOOLS / "FogMoonlightDiagnostics.c"), str(TOOLS / "FogMoonlightRecovery.c"),
        str(prepared / "LinkedBlockingQueue.c"), str(CORE / "ByteBuffer.c"),
        str(source), "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
    video = (prepared / "VideoStream.c").read_text()
    start = video.index("    uint64_t previousCycleEndUs = 0;")
    end = video.index("\n    if (buffer != NULL) {\n        free(buffer);", start)
    sockets = (prepared / "PlatformSockets.c").read_text()
    start_socket = sockets.index("    if (socketQosType == SOCK_QOS_TYPE_VIDEO) {\n        FogDiagVideoReceiveBufferRequested")
    end_socket = sockets.index("\n}\n\nint setSocketNonBlocking", start_socket)
    for name, marker, body in (
            ("receive_diagnostics_test", "RECEIVE_LOOP", video[start:end]),
            ("socket_diagnostics_test", "SOCKET_BUFFER", sockets[start_socket:end_socket])):
        source = destination / f"{name}.c"
        template = (TOOLS / "tests" / f"{name}.c").read_text()
        assert template.count(marker) == 1
        source.write_text(template.replace(marker, body))
        binary = destination / name
        subprocess.run([
            "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer", f"-I{TOOLS}",
            str(TOOLS / "FogMoonlightDiagnostics.c"), str(source), "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)
    # Exercise the exact prepared frame-transition control flow, including its
    # early returns, with only logging/control/list cleanup replaced by doubles.
    # No FEC algorithm or data-path behavior is reimplemented by this harness.
    fec = (prepared / "RtpVideoQueue.c").read_text()
    start = fec.index("    if (queue->pendingFecBlockList.count == 0 ||")
    end = fec.index("        // Discard any pending buffers from the previous FEC block", start)
    transition = fec[start:end] + "    }\n    return 1;\n"
    harness = r'''
#include "FogMoonlightDiagnosticsInternal.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#define Limelog(...) ((void)0)
#define RTPF_RET_REJECTED 0
typedef struct List { unsigned int count; } List;
typedef struct Queue {
    List pendingFecBlockList, completedFecBlockList;
    unsigned int currentFrameNumber, multiFecCurrentBlockNumber, multiFecLastBlockNumber;
    bool reportedLostFrame;
    unsigned int bufferDataPackets, receivedDataPackets, bufferParityPackets, receivedParityPackets;
} Queue;
typedef struct Packet { unsigned int frameIndex; } Packet;
static void reportFinalFrameFecStatus(Queue* queue) { (void)queue; }
static void purgeListEntries(List* list) { list->count = 0; }
static void notifyFrameLost(unsigned int frame, bool speculative) { (void)frame; (void)speculative; }
static uint64_t PltGetMicroseconds(void) { return 1000; }
static int transition(Queue* queue, unsigned int frame, uint8_t fecCurrentBlockNumber) {
    Packet packet = {frame};
    Packet* nvPacket = &packet;
TRANSITION_BODY
}
static void check(Queue queue, unsigned int frame, uint8_t incomingBlock,
                  int expectedResult, uint64_t failedBlocks) {
    FogDiagReset();
    assert(transition(&queue, frame, incomingBlock) == expectedResult);
    FogMoonlightDiagnostics snapshot;
    FogMoonlightGetDiagnostics(&snapshot);
    assert(snapshot.fecBlocksFailed == failedBlocks);
}
int main(void) {
    // Block 0 complete, blocks 1 and 2 absent, then next frame arrives.
    check((Queue){{0}, {3}, 1, 1, 2, false, 0, 0, 0, 0}, 2, 0, 1, 2);
    // Block 1 partial, block 2 absent, then next frame arrives.
    check((Queue){{2}, {3}, 1, 1, 2, false, 0, 0, 0, 0}, 2, 0, 1, 2);
    // Block 0 partial, block 1 absent, block 2 triggers dropping all 4 blocks.
    check((Queue){{2}, {0}, 1, 0, 3, false, 0, 0, 0, 0}, 1, 2, 0, 4);
    // Complete prefix, no pending block 1, block 3 arrives: tail 1..3 dropped.
    check((Queue){{0}, {3}, 1, 1, 3, false, 0, 0, 0, 0}, 1, 3, 0, 3);
    // Unknown whole missing frames have no inferred FEC block count.
    check((Queue){{0}, {0}, 1, 0, 0, false, 0, 0, 0, 0}, 4, 0, 1, 0);
    // A single incomplete block is counted once.
    check((Queue){{2}, {0}, 1, 0, 0, false, 0, 0, 0, 0}, 2, 0, 1, 1);
    // Successfully completed prior frame has no residual lists.
    check((Queue){{0}, {0}, 2, 0, 0, false, 0, 0, 0, 0}, 2, 0, 1, 0);
    return 0;
}
'''.replace("TRANSITION_BODY", transition)
    source = destination / "fec-transitions.c"
    source.write_text(harness)
    binary = destination / "fec-transitions"
    subprocess.run([
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", f"-I{TOOLS}",
        str(TOOLS / "FogMoonlightDiagnostics.c"), str(source), "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
print("Core diagnostic preparation and snapshot tests passed")
