"""LAN-only generated H264/Opus test. No desktop capture or OS input injection."""
import argparse
import asyncio
import fractions
import ipaddress
import json
import logging
import math
import pathlib
import time

import numpy as np
from aiohttp import WSMsgType, web
from aiortc import (AudioStreamTrack, RTCConfiguration, RTCPeerConnection,
                    RTCRtpSender, RTCSessionDescription, VideoStreamTrack)
from aiortc.sdp import candidate_from_sdp
from av import AudioFrame, VideoFrame
from PIL import Image, ImageDraw

LOG = logging.getLogger("av-test")
MAX_MESSAGE = 128 * 1024
BUTTON_LABELS = ["B", "A", "Y", "X", "L", "R", "ZL", "ZR", "Minus", "Plus",
                 "LS", "RS", "Up", "Down", "Left", "Right", "Home", "Capture"]


def lan_address(value):
    address = ipaddress.ip_address(value)
    if address.version != 4 or not (address.is_private or address.is_loopback):
        raise ValueError("Use a private IPv4 LAN address or 127.0.0.1")
    if address.is_unspecified or address.is_multicast or address.is_reserved:
        raise ValueError("Bind to a specific private IPv4 address")
    return str(address)


def parse_input(payload):
    if not isinstance(payload, str) or len(payload) > 4096:
        raise ValueError("input must be a short JSON string")
    value = json.loads(payload)
    if not isinstance(value, dict) or value.get("type") != "input":
        raise ValueError("expected input")
    sequence = value.get("seq")
    buttons, axes = value.get("buttons"), value.get("axes")
    if type(sequence) is not int or not 0 <= sequence <= 2**53 - 1:
        raise ValueError("invalid sequence")
    if not isinstance(buttons, list) or len(buttons) != 18:
        raise ValueError("expected 18 buttons")
    if any(type(button) not in (int, bool) or button not in (0, 1) for button in buttons):
        raise ValueError("buttons must be zero or one")
    if not isinstance(axes, list) or len(axes) != 4:
        raise ValueError("expected four axes")
    if any(type(axis) not in (float, int) or not math.isfinite(axis) or abs(axis) > 1 for axis in axes):
        raise ValueError("axes out of range")
    triggers = [value.get("leftTrigger", 0), value.get("rightTrigger", 0)]
    if any(type(trigger) not in (float, int) or not math.isfinite(trigger) or not 0 <= trigger <= 1 for trigger in triggers):
        raise ValueError("triggers out of range")
    return {"type": "echo", "seq": sequence, "buttons": [int(x) for x in buttons],
            "axes": [float(x) for x in axes], "leftTrigger": float(triggers[0]),
            "rightTrigger": float(triggers[1])}


def neutral_input():
    return {"type": "echo", "seq": 0, "buttons": [0] * 18,
            "axes": [0.0] * 4, "leftTrigger": 0.0, "rightTrigger": 0.0}


def ipv4_lan_sdp(sdp):
    """Avoid probing IPv6/public candidates from a multi-interface LAN client."""
    lines = []
    for line in sdp.splitlines():
        if line.startswith("a=candidate:"):
            candidate = candidate_from_sdp(line[len("a=candidate:"):])
            try:
                lan_address(candidate.ip)
            except ValueError:
                LOG.info("ignored unsupported remote candidate address")
                continue
        lines.append(line)
    return "\r\n".join(lines) + "\r\n"


class CalibrationVideo(VideoStreamTrack):
    def __init__(self, state):
        super().__init__()
        self.state = state
        self.frames = 0

    async def recv(self):
        pts, time_base = await self.next_timestamp()
        count = self.frames
        self.frames += 1
        image = Image.new("RGB", (640, 360), (14, 21, 38))
        draw = ImageDraw.Draw(image)
        bars = [(240, 240, 240), (230, 215, 20), (20, 220, 220), (20, 210, 55),
                (220, 30, 220), (220, 35, 35), (30, 55, 230), (24, 24, 24)]
        for index, color in enumerate(bars):
            draw.rectangle((index * 80, 0, index * 80 + 79, 75), fill=color)
        draw.text((16, 91), "FOGPLAY NX - LOCAL VIDEO / AUDIO / CONTROL", fill="white", font_size=21)
        draw.text((16, 123), f"640 x 360 / 30 fps   frame: {count:06d}", fill="white", font_size=20)
        draw.rectangle((16, 159, 623, 177), fill=(43, 56, 80))
        position = 16 + (count * 5) % 586
        draw.rectangle((position, 159, position + 21, 177), fill=(30, 245, 185))
        control = self.state.latest
        active = [BUTTON_LABELS[index] for index, pressed in enumerate(control["buttons"]) if pressed]
        draw.text((16, 197), f"Input seq: {control['seq']}  buttons: {','.join(active) or '-'}", fill="white", font_size=18)
        draw.text((16, 225), "Axes: " + "  ".join(f"{axis:+.2f}" for axis in control["axes"]), fill="white", font_size=18)
        draw.text((16, 251), f"Triggers: L {control['leftTrigger']:.2f} / R {control['rightTrigger']:.2f}", fill="white", font_size=18)
        draw.text((16, 319), "Stereo tone: L 440 Hz / R 660 Hz; A changes pitch", fill=(145, 220, 230), font_size=17)
        x = 320 + int(control["axes"][0] * 280)
        y = 292 + int(control["axes"][1] * 16)
        draw.ellipse((x - 9, y - 9, x + 9, y + 9), fill=(255, 90, 140))
        frame = VideoFrame.from_ndarray(np.array(image), format="rgb24")
        frame.pts, frame.time_base = pts, time_base
        self.state.video_frames += 1
        return frame


class CalibrationAudio(AudioStreamTrack):
    def __init__(self, state):
        super().__init__()
        self.state = state
        self.pts = 0
        self.start = None

    async def recv(self):
        now = asyncio.get_running_loop().time()
        if self.start is None:
            self.start = now
        await asyncio.sleep(max(0, self.start + self.pts / 48000 - now))
        samples = np.arange(960, dtype=np.float64) + self.pts
        multiplier = 1.5 if self.state.latest["buttons"][1] else 1.0
        left = np.sin(samples * 2 * math.pi * 440 * multiplier / 48000)
        right = np.sin(samples * 2 * math.pi * 660 * multiplier / 48000)
        # Packed stereo, deliberately gentle volume (8% full scale).
        packed = (np.column_stack((left, right)) * (32767 * 0.08)).astype(np.int16).reshape(1, -1)
        frame = AudioFrame.from_ndarray(packed, format="s16", layout="stereo")
        frame.sample_rate, frame.pts = 48000, self.pts
        frame.time_base = fractions.Fraction(1, 48000)
        self.pts += 960
        self.state.audio_frames += 1
        return frame


class ServerState:
    def __init__(self):
        self.peer = None
        self.websocket = None
        self.started = time.monotonic()
        self.connections = 0
        self.video_frames = self.audio_frames = self.input_messages = 0
        self.latest = neutral_input()
        self.last_error = None

    def snapshot(self):
        return {"connected": self.peer is not None, "state": self.peer.connectionState if self.peer else "idle",
                "connections": self.connections, "video_frames": self.video_frames,
                "audio_frames": self.audio_frames, "input_messages": self.input_messages,
                "uptime_seconds": round(time.monotonic() - self.started, 1),
                "last_error": self.last_error, "input": self.latest}


STATE_KEY = web.AppKey("state", ServerState)
PAGE = """<!doctype html><meta charset=utf-8><title>FogPlay NX LAN test</title>
<style>body{font:20px monospace;background:#111827;color:#e5e7eb;padding:30px}pre{white-space:pre-wrap}</style>
<h1>FogPlay NX: local AV and controller test</h1>
<p>Generated H264 video + stereo Opus. Controller feedback only inside this test.</p>
<pre id=status>Waiting...</pre><script>async function update(){try{let r=await fetch('/status');
document.getElementById('status').textContent=JSON.stringify(await r.json(),null,2)}catch(e){
document.getElementById('status').textContent='Server unavailable'}setTimeout(update,500)}update()</script>"""


async def index(request):
    return web.Response(text=PAGE, content_type="text/html")


async def status(request):
    return web.json_response(request.app[STATE_KEY].snapshot())


async def websocket(request):
    state = request.app[STATE_KEY]
    try:
        lan_address(request.remote)
    except ValueError:
        raise web.HTTPForbidden(text="Private IPv4 clients only")
    if state.peer is not None:
        raise web.HTTPConflict(text="One Switch at a time; disconnect the previous client")
    ws = web.WebSocketResponse(max_msg_size=MAX_MESSAGE, heartbeat=10)
    pc = RTCPeerConnection(RTCConfiguration(iceServers=[]))
    video, audio = CalibrationVideo(state), CalibrationAudio(state)
    state.peer, state.websocket = pc, ws
    state.latest = neutral_input()
    state.last_error = None
    state.connections += 1
    LOG.info("peer %d accepted from %s", state.connections, request.remote)

    @pc.on("connectionstatechange")
    async def connection_state():
        LOG.info("RTC state %s", pc.connectionState)
        if pc.connectionState == "failed":
            state.last_error = "RTC failed; check that PC and Switch share a LAN and the PC firewall allows the test"
            await ws.close(message=b"RTC failed")

    control = pc.createDataChannel("control", ordered=True)

    @control.on("open")
    def control_open():
        LOG.info("control open")

    @control.on("message")
    def control_message(payload):
        try:
            echoed = parse_input(payload)
        except (ValueError, TypeError, json.JSONDecodeError):
            LOG.warning("rejected invalid control input")
            return
        state.latest = echoed
        state.input_messages += 1
        if control.readyState == "open" and control.bufferedAmount < 65536:
            control.send(json.dumps(echoed, separators=(",", ":")))

    answered = False
    queued_candidates = []
    try:
        await ws.prepare(request)
        deadline = asyncio.get_running_loop().time() + 30
        for track, mime in [(video, "video/H264"), (audio, "audio/opus")]:
            transceiver = pc.addTransceiver(track, direction="sendonly")
            codecs = [codec for codec in RTCRtpSender.getCapabilities(track.kind).codecs
                      if codec.mimeType.lower() == mime.lower()]
            if not codecs:
                raise RuntimeError(f"Required encoder missing: {mime}")
            transceiver.setCodecPreferences(codecs)
        await asyncio.wait_for(pc.setLocalDescription(await pc.createOffer()), 30)
        await ws.send_json({"type": "offer", "sdp": pc.localDescription.sdp})
        LOG.info("full ICE offer sent (%d bytes)", len(pc.localDescription.sdp))
        while not ws.closed:
            if not answered:
                remaining = deadline - asyncio.get_running_loop().time()
                if remaining <= 0:
                    raise asyncio.TimeoutError()
                message = await asyncio.wait_for(ws.receive(), remaining)
            else:
                message = await ws.receive()
            if message.type != WSMsgType.TEXT:
                if message.type in (WSMsgType.ERROR, WSMsgType.CLOSE, WSMsgType.CLOSED):
                    break
                continue
            try:
                value = json.loads(message.data)
                if not isinstance(value, dict):
                    raise ValueError("expected object")
                kind = value.get("type")
                if kind == "answer":
                    sdp = value.get("sdp")
                    if answered or not isinstance(sdp, str) or len(sdp) > MAX_MESSAGE - 100:
                        raise ValueError("invalid answer")
                    await pc.setRemoteDescription(RTCSessionDescription(sdp=ipv4_lan_sdp(sdp), type="answer"))
                    answered = True
                    for candidate in queued_candidates:
                        await pc.addIceCandidate(candidate)
                    queued_candidates.clear()
                    LOG.info("answer accepted")
                elif kind == "candidate":
                    raw = value.get("candidate")
                    if raw in (None, ""):
                        candidate = None
                    elif isinstance(raw, str) and len(raw) <= 4096:
                        candidate = candidate_from_sdp(raw.removeprefix("candidate:"))
                        try:
                            lan_address(candidate.ip)
                        except ValueError:
                            LOG.info("ignored unsupported trickled candidate address")
                            continue
                        candidate.sdpMid = value.get("sdpMid")
                        candidate.sdpMLineIndex = value.get("sdpMLineIndex")
                        if candidate.sdpMid is None and candidate.sdpMLineIndex is None:
                            raise ValueError("candidate must identify a media line")
                    else:
                        raise ValueError("invalid candidate")
                    if answered:
                        await pc.addIceCandidate(candidate)
                    elif len(queued_candidates) < 64:
                        queued_candidates.append(candidate)
                    else:
                        raise ValueError("too many candidates")
                elif kind == "stop":
                    break
                else:
                    raise ValueError("unknown signaling type")
            except (ValueError, TypeError, KeyError, json.JSONDecodeError):
                LOG.exception("invalid signaling message")
                await ws.close(code=1008, message=b"Invalid signaling")
                break
    except asyncio.TimeoutError:
        state.last_error = "No SDP answer within 30 seconds; verify Switch server IP and local network/firewall"
        LOG.error(state.last_error)
    except asyncio.CancelledError:
        raise
    except Exception:
        state.last_error = "Peer error; see pc-server.log"
        LOG.exception("peer error")
    finally:
        video.stop()
        audio.stop()
        await pc.close()
        if ws.prepared:
            await ws.close()
        if state.peer is pc:
            state.peer, state.websocket = None, None
            state.latest = neutral_input()
        LOG.info("peer cleanup complete: %s", state.snapshot())
    return ws


async def shutdown(app):
    state = app[STATE_KEY]
    if state.websocket:
        await state.websocket.close(code=1001, message=b"Server stopping")
    if state.peer:
        await state.peer.close()


def create_app():
    app = web.Application(client_max_size=MAX_MESSAGE)
    app[STATE_KEY] = ServerState()
    app.router.add_get("/", index)
    app.router.add_get("/status", status)
    app.router.add_get("/ws", websocket)
    app.on_shutdown.append(shutdown)
    return app


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1", type=lan_address)
    parser.add_argument("--port", default=8765, type=int)
    parser.add_argument("--log-dir", default="logs", type=pathlib.Path)
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("port must be between 1 and 65535")
    args.log_dir.mkdir(parents=True, exist_ok=True)
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s %(message)s",
                        handlers=[logging.StreamHandler(), logging.FileHandler(args.log_dir / "pc-server.log", encoding="utf-8")])
    LOG.info("Generated LAN AV test; no account or rental; bind %s:%d", args.host, args.port)
    web.run_app(create_app(), host=args.host, port=args.port)


if __name__ == "__main__":
    main()
