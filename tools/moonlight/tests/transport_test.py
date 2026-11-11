#!/usr/bin/env python3
"""Run prepared RTSP/socket code against local sockets and exact host parsing."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
TOOLS = ROOT / "tools/moonlight"
CORE = ROOT / "third_party/moonlight/moonlight-switch/extern/moonlight-common-c/src"
PREPARED = ROOT / "third_party/moonlight/prepared/core"
if os.environ.get("FOG_MOONLIGHT_PRECHECKED") != "1":
    subprocess.run(["python3", "-B", str(TOOLS / "prepare.py"), "--check"], check=True)
flags = ["cc", "-std=gnu11", "-DLC_DEBUG", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
         "-Wno-unused-function", "-pthread", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
         "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
         f"-I{TOOLS}", f"-I{CORE}", f"-I{CORE.parent / 'enet/include'}"]
with tempfile.TemporaryDirectory(prefix="moonlight-transport-") as directory:
    destination = Path(directory)
    binary = destination / "rtsp-test"
    subprocess.run(flags + [str(TOOLS / "tests/rtsp_transport_test.c"),
        str(PREPARED / "PlatformSockets.c"), str(CORE / "RtspParser.c"),
        str(CORE / "PlatformCrypto.c"), str(CORE / "Misc.c"), str(TOOLS / "FogMoonlightDiagnostics.c"), "-lcrypto", "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

    control = (PREPARED / "ControlStream.c").read_text()
    start = control.index("                uint32_t terminationErrorCode;")
    end = control.index("                // We used to wait for a ENET_EVENT_TYPE_DISCONNECT event,", start)
    notify = "FogMoonlightNotifyHostTermination(ListenerCallbacks.connectionTerminated, (int)terminationErrorCode);"
    assert control.count(notify) == 1
    connection = (PREPARED / "Connection.c").read_text()
    start_callback = connection.index("static void ClInternalConnectionTerminated(int errorCode)")
    end_callback = connection.index("\n}", start_callback) + 2
    source = destination / "termination-test.c"
    template = (TOOLS / "tests/termination_origin_test.c").read_text()
    source.write_text(template.replace("TERMINATION_SHIM", connection[start_callback:end_callback])
        .replace("HOST_PARSER", control[start:end] + notify))
    binary = destination / "termination-test"
    subprocess.run(flags + [str(source), str(CORE / "ByteBuffer.c"),
        str(TOOLS / "FogMoonlightTermination.c"), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("Moonlight real RTSP cancellation and host termination provenance passed")
