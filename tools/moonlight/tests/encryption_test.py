#!/usr/bin/env python3
"""Exercise pinned encryption paths without network or credential fixtures."""
from pathlib import Path
import os
import subprocess
import tempfile
import sys
ROOT = Path(__file__).resolve().parents[3]
TOOLS = ROOT / 'tools/moonlight'
CORE = ROOT / 'third_party/moonlight/moonlight-switch/extern/moonlight-common-c/src'
PREPARED = ROOT / 'third_party/moonlight/prepared/core'
HOST = ROOT / 'third_party/rtc/host'
if os.environ.get('FOG_MOONLIGHT_PRECHECKED') != '1':
    subprocess.run([sys.executable, '-B', str(TOOLS / 'prepare.py'), '--check'], check=True)
flags = ['cc','-std=gnu11','-DLC_DEBUG','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-unused-function',
         '-pthread','-fsanitize=address,undefined','-fno-omit-frame-pointer','-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
         '-DUSE_MBEDTLS',f'-DMBEDTLS_USER_CONFIG_FILE="{ROOT}/tools/rtc/mbedtls-user-config.h"',
         f'-I{HOST}/include',f'-I{TOOLS}',f'-I{CORE}',f'-I{CORE.parent}/enet/include']
with tempfile.TemporaryDirectory(prefix='fogplay-encryption-') as directory:
    work=Path(directory)
    for name in ('encryption_crypto_test','encryption_negotiation_test','encryption_video_test','encryption_audio_test'):
        source=TOOLS/'tests'/f'{name}.c'
        sources=[str(source),str(CORE/'PlatformCrypto.c')]
        if name=='encryption_negotiation_test':
            rtsp=(PREPARED/'RtspConnection.c').read_text()
            anchor='return transactRtspMessageTcp(request, response, error);'
            assert rtsp.count(anchor)==1
            rtsp=rtsp.replace(anchor,'return fixtureTransaction(request, response, error);')
            enet='return transactRtspMessageEnet(request, response, expectingPayload, error);'
            assert rtsp.count(enet)==1
            rtsp=rtsp.replace(enet,'return fixtureTransaction(request, response, error);')
            payload='request.payload = getSdpPayloadForStreamConfig(rtspClientVersion, &payloadLength);'
            assert rtsp.count(payload)==1
            rtsp=rtsp.replace(payload,'request.payload = fixtureSdpPayload(rtspClientVersion, &payloadLength);')
            rtsp=rtsp.replace('freeMessage(', 'fixtureFreeMessage(')

            template=source.read_text(); assert template.count('PREPARED_RTSP')==1
            generated=work/f'{name}.c'; generated.write_text(template.replace('PREPARED_RTSP',rtsp))
            sources=[str(generated),str(CORE/'PlatformCrypto.c'),str(CORE/'SdpGenerator.c'),str(CORE/'RtspParser.c'),
                     str(TOOLS/'FogMoonlightDiagnostics.c')]
        if name=='encryption_video_test':
            video=(PREPARED/'VideoStream.c').read_text()
            start=video.index('    uint64_t previousCycleEndUs = 0;')
            end=video.index('\n    if (buffer != NULL) {\n        free(buffer);',start)
            template=source.read_text(); assert template.count('RECEIVE_LOOP')==1
            generated=work/f'{name}.c'; generated.write_text(template.replace('RECEIVE_LOOP',video[start:end]))
            sources=[str(generated),str(CORE/'PlatformCrypto.c'),str(TOOLS/'FogMoonlightDiagnostics.c')]
        compile_flags=flags
        if name=='encryption_audio_test':
            audio=(PREPARED/'AudioStream.c').read_text()
            start=audio.index('static void decodeInputData(')
            end=audio.index('\nstatic void AudioReceiveThreadProc(',start)
            types_start=audio.index('#define MAX_PACKET_SIZE')
            types_end=audio.index('\nstatic void AudioPingThreadProc',types_start)
            template=source.read_text()
            generated=work/f'{name}.c'
            generated.write_text(template.replace('AUDIO_DECODE',audio[start:end]).replace('AUDIO_TYPES',audio[types_start:types_end]))
            sources=[str(generated),str(CORE/'PlatformCrypto.c'),str(TOOLS/'FogMoonlightDiagnostics.c')]
            compile_flags=flags+['-ULC_DEBUG']  # Exercise release drop-on-error; debug core intentionally asserts.
        binary=work/name
        subprocess.run(compile_flags+sources+[str(HOST/'lib/libmbedcrypto.a'),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
print('Moonlight pinned encryption tests passed')