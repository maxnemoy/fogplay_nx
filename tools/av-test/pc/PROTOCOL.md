# Local AV/controller test: PC endpoint

The server generates animated H264 640x360 at 30 fps and stereo 48 kHz Opus.
The left/right tone frequencies are 440/660 Hz at 8% amplitude. Holding A
raises both frequencies. Controller input changes the rendered calibration
screen and the `/status` response; it never injects keys or a gamepad into Windows.

Run from the packaged `av-test` folder with its private Python runtime:

```powershell
.\runtime\Scripts\python.exe .\pc\server.py --host 192.0.2.10 --port 8765 --log-dir .\logs
```

Bind to the PC's actual private IPv4 LAN address, not a WSL or Hyper-V adapter.
Without `--host` the server listens only on 127.0.0.1. No STUN/TURN server,
account, Fog Play API or rental is used. Port 8765 carries HTTP/WebSocket;
WebRTC media and control use negotiated UDP ports. Ctrl+C stops the server.

`GET /` displays status. `GET /status` returns JSON counters, connection state,
the most recent input, and `last_error`. `GET /ws` upgrades to WebSocket.
Exactly one peer is accepted at a time (second clients receive HTTP 409).

The PC initiates negotiation and sends:

```json
{"type":"offer","sdp":"<complete SDP including gathered host ICE candidates>"}
```

The Switch responds:

```json
{"type":"answer","sdp":"<answer SDP>"}
```

Optional additional client candidates:

```json
{"type":"candidate","candidate":"candidate:...","sdpMid":"0","sdpMLineIndex":0}
```

Empty/null candidate means end of candidates. Candidates arriving before the
answer are queued, up to 64. IPv6 or public candidates are ignored; this test
uses numeric private IPv4 LAN candidates only. Maximum signaling message:
128 KiB. The SDP answer must arrive within 30 seconds. `{"type":"stop"}` or
WebSocket close ends the peer and releases all tracks/transports. Disconnect
and reconnect reset controls to neutral.

The PC creates an ordered reliable data channel named `control`. Input format:

```json
{"type":"input","seq":1,"buttons":[0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0],"axes":[0.5,-0.25,0,0],"leftTrigger":0,"rightTrigger":0}
```

Button order: B, A, Y, X, L, R, ZL, ZR, Minus, Plus, LStick, RStick, Up, Down,
Left, Right, Home, Capture. Axes are LX, LY, RX, RY in [-1,1], with up negative.
Triggers are in [0,1]. Sequence is a nonnegative integer up to 2^53-1. Buttons
must be 0/1. Messages are limited to 4096 bytes and malformed input is ignored.
The Switch client reserves Plus for test exit.

The reply repeats the accepted values in a flat object, changing `type` to `echo`.
Codec payload types must be read from SDP; the server negotiates H264 and Opus
only. The pinned aiortc encoder uses Baseline H264, zerolatency, and responds to
RTCP PLI keyframe requests. No private encoder changes force a fixed GOP.

Validation command:

```powershell
.\runtime\Scripts\python.exe .\pc\self_test.py --host 192.0.2.10 --log-dir .\logs
```

This starts an ephemeral server and a second aiortc client on the PC. It decodes
actual transmitted video/audio, verifies controller echo, repeats three
connections, checks single-peer rejection, neutral controls, shutdown with a
pending peer, and no remaining asyncio tasks. Evidence is in `logs/self-test.json`
and `logs/self-test.log`. This establishes native Windows PC interoperability;
physical Switch video/audio/display/playback still require the user's console.
