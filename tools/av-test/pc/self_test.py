"""Decode actual transmitted media and verify controller echo/reconnect/teardown."""
import argparse
import asyncio
import hashlib
import json
import logging
import pathlib

import aiohttp
import numpy as np
from aiohttp import web
from aiortc import RTCConfiguration, RTCPeerConnection, RTCSessionDescription
from aiortc.mediastreams import MediaStreamError

import server


async def exchange(base_url, run):
    pc = RTCPeerConnection(RTCConfiguration(iceServers=[]))
    tracks = []
    echoes = []
    opened = asyncio.Event()
    got_echo = asyncio.Event()
    measurements = {"video": [], "audio": []}
    input_value = {"type": "input", "seq": run * 1000 + 1, "buttons": [0] * 18,
                   "axes": [0.5, -0.25, 0.75, -1.0], "leftTrigger": 0.75, "rightTrigger": 0.25}
    input_value["buttons"][1] = 1

    async def collect(track):
        try:
            while True:
                frame = await track.recv()
                if track.kind == "video":
                    array = frame.to_ndarray(format="rgb24")
                    measurements["video"].append({"width": frame.width, "height": frame.height,
                        "motion": hashlib.sha256(array[159:178, :, :].tobytes()).hexdigest(),
                        "pts": frame.pts, "time_base": str(frame.time_base)})
                else:
                    array = frame.to_ndarray()
                    measurements["audio"].append({"peak": float(np.max(np.abs(array.astype(np.float64)))),
                        "sample_rate": frame.sample_rate, "channels": len(frame.layout.channels), "pts": frame.pts})
        except MediaStreamError:
            pass

    @pc.on("track")
    def track_received(track):
        tracks.append(asyncio.create_task(collect(track)))

    @pc.on("datachannel")
    def channel_received(channel):
        def send():
            opened.set()
            # Malformed input must not prevent a subsequent valid message.
            channel.send('{"type":"input","seq":-1}')
            channel.send(json.dumps(input_value))
        @channel.on("open")
        def channel_open():
            send()
        @channel.on("message")
        def channel_message(payload):
            value = json.loads(payload)
            if value.get("type") == "echo":
                echoes.append(value)
                got_echo.set()
        if channel.readyState == "open":
            send()

    try:
        async with aiohttp.ClientSession() as session:
            async with session.ws_connect(base_url + "/ws", max_msg_size=server.MAX_MESSAGE) as ws:
                offer = await asyncio.wait_for(ws.receive_json(), 15)
                assert offer["type"] == "offer"
                try:
                    extra = await session.ws_connect(base_url + "/ws")
                except aiohttp.WSServerHandshakeError as exc:
                    assert exc.status == 409, exc.status
                else:
                    await extra.close()
                    raise AssertionError("server accepted a concurrent second peer")
                sdp = offer["sdp"]
                assert "H264/90000" in sdp and "opus/48000/2" in sdp
                assert "a=candidate:" in sdp and "a=end-of-candidates" in sdp
                await pc.setRemoteDescription(RTCSessionDescription(type="offer", sdp=sdp))
                await pc.setLocalDescription(await pc.createAnswer())
                await ws.send_json({"type": "answer", "sdp": pc.localDescription.sdp})
                await asyncio.wait_for(opened.wait(), 15)
                await asyncio.wait_for(got_echo.wait(), 15)
                await asyncio.sleep(2.5)
                assert echoes[0] == dict(input_value, type="echo"), echoes
                videos, audios = measurements["video"], measurements["audio"]
                assert len(videos) >= 20, len(videos)
                assert len(audios) >= 50, len(audios)
                assert all(x["width"] == 640 and x["height"] == 360 for x in videos)
                assert len({x["pts"] for x in videos}) == len(videos)
                assert len({x["motion"] for x in videos}) >= 10
                assert all(x["sample_rate"] == 48000 and x["channels"] == 2 for x in audios)
                assert max(x["peak"] for x in audios) > 0.01
                await ws.send_json({"type": "stop"})
                logging.info("run %d PASS video=%d audio=%d echo=%s", run, len(videos), len(audios), echoes[0]["seq"])
                return {"run": run, "decoded_video_frames": len(videos), "decoded_audio_frames": len(audios),
                        "control_echo": True, "changing_video": True, "nonzero_stereo_48k_audio": True}
    finally:
        await pc.close()
        for task in tracks:
            task.cancel()
        await asyncio.gather(*tracks, return_exceptions=True)


async def test(args):
    app = server.create_app()
    runner = web.AppRunner(app)
    await runner.setup()
    site = web.TCPSite(runner, args.host, 0)
    await site.start()
    port = site._server.sockets[0].getsockname()[1]
    base_url = f"http://{args.host}:{port}"
    results = []
    try:
        for run in range(1, 4):
            results.append(await asyncio.wait_for(exchange(base_url, run), 40))
            for _ in range(100):
                if app[server.STATE_KEY].peer is None:
                    break
                await asyncio.sleep(0.05)
            assert app[server.STATE_KEY].peer is None, "server retained disconnected peer"
            assert app[server.STATE_KEY].latest == server.neutral_input(), "held controls survived disconnect"
        results.append({"sequential_reconnects": 3, "all_peers_closed": True})
        # Stop the server with an accepted, unanswered connection still present.
        async with aiohttp.ClientSession() as session:
            async with session.ws_connect(base_url + "/ws") as ws:
                offer = await asyncio.wait_for(ws.receive_json(), 15)
                assert offer["type"] == "offer"
                await asyncio.wait_for(runner.cleanup(), 10)
                assert app[server.STATE_KEY].peer is None
                assert app[server.STATE_KEY].latest == server.neutral_input()
        results.append({"shutdown_with_pending_peer": True, "single_peer_conflict": True,
                        "controls_neutral_after_disconnect": True})
    finally:
        await runner.cleanup()
    await asyncio.sleep(0.2)
    leaked = [task for task in asyncio.all_tasks() if task is not asyncio.current_task() and not task.done()]
    assert not leaked, f"remaining asyncio tasks: {leaked}"
    results.append({"server_shutdown": True, "remaining_async_tasks": 0})
    args.log_dir.mkdir(parents=True, exist_ok=True)
    (args.log_dir / "self-test.json").write_text(json.dumps({"result": "PASS", "host": args.host, "checks": results}, indent=2), encoding="utf-8")
    print("PASS: decoded changing H264 video, nonzero stereo Opus audio, input echo, three reconnects, complete teardown")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1", type=server.lan_address)
    parser.add_argument("--log-dir", type=pathlib.Path, default=pathlib.Path("logs"))
    args = parser.parse_args()
    args.log_dir.mkdir(parents=True, exist_ok=True)
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s %(message)s",
                        handlers=[logging.StreamHandler(), logging.FileHandler(args.log_dir / "self-test.log", encoding="utf-8")])
    asyncio.run(test(args))


if __name__ == "__main__":
    main()
