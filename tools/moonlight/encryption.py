"""Observe negotiated encryption; reject contradictory required capabilities."""


def apply(dest):
    # Applying twice could otherwise duplicate counters while retaining anchors.
    if "FOG_DIAG_encryptionClientFlags" in (dest / "core/Connection.c").read_text():
        raise RuntimeError("Pinned encryption source is already transformed")
    def change(file, old, new):
        path = dest / "core" / file
        text = path.read_text()
        if text.count(old) != 1:
            raise RuntimeError(f"Pinned encryption source mismatch: {file}: {old[:70]!r}")
        path.write_text(text.replace(old, new))

    for name in ("RtspConnection.c", "AudioStream.c"):
        change(name, '#include "Limelight-internal.h"',
               '#include "Limelight-internal.h"\n#include "FogMoonlightDiagnosticsInternal.h"')
    change("Connection.c", "    FogDiagReset();", "    FogDiagReset();\n    FogDiagSet(FOG_DIAG_encryptionClientFlags, (uint32_t)streamConfig->encryptionFlags);")
    change("RtspConnection.c", "    encryptionCtx = PltCreateCryptoContext();",
           "    FogDiagSet(FOG_DIAG_encryptionRtsp, encryptedRtspEnabled);\n    encryptionCtx = PltCreateCryptoContext();")
    change("RtspConnection.c", "        EncryptionFeaturesEnabled = 0;", """        EncryptionFeaturesEnabled = 0;
        FogDiagSet(FOG_DIAG_encryptionHostSupported, EncryptionFeaturesSupported);
        FogDiagSet(FOG_DIAG_encryptionHostRequired, EncryptionFeaturesRequested);
        FogDiagSet(FOG_DIAG_encryptionState, FOG_ENCRYPTION_CAPABILITIES);
        // A required known feature without support is contradictory. Never
        // proceed to ANNOUNCE/PLAY or silently select plaintext in this case.
        if (EncryptionFeaturesRequested & ~EncryptionFeaturesSupported &
                (SS_ENC_CONTROL_V2 | SS_ENC_VIDEO | SS_ENC_AUDIO)) {
            FogDiagSet(FOG_DIAG_encryptionFailure, FOG_ENCRYPTION_FAILURE_CONFLICT);
            ret = FOG_MOONLIGHT_ENCRYPTION_CONFLICT;
            freeMessage(&response);
            goto Exit;
        }""")
    change("RtspConnection.c", "        request.flags |= FLAG_ALLOCATED_PAYLOAD;", """        FogDiagSet(FOG_DIAG_encryptionSelected, EncryptionFeaturesEnabled);
        FogDiagSet(FOG_DIAG_encryptionAudio, AudioEncryptionEnabled);
        FogDiagSet(FOG_DIAG_encryptionState, FOG_ENCRYPTION_SELECTED);
        request.flags |= FLAG_ALLOCATED_PAYLOAD;
        // Advertising support does not guarantee this host/version selected it.
        // Audio has an independent NV switch; the SS audio bit is insufficient.
        // Control initializes after RTSP using this same pinned version gate.
        if ((EncryptionFeaturesRequested & (SS_ENC_VIDEO | SS_ENC_CONTROL_V2) &
                ~EncryptionFeaturesEnabled) ||
                ((EncryptionFeaturesRequested & SS_ENC_AUDIO) && !AudioEncryptionEnabled) ||
                ((EncryptionFeaturesRequested & SS_ENC_CONTROL_V2) && !APP_VERSION_AT_LEAST(7, 1, 431))) {
            FogDiagSet(FOG_DIAG_encryptionFailure, FOG_ENCRYPTION_FAILURE_CONFLICT);
            *error = FOG_MOONLIGHT_ENCRYPTION_CONFLICT;
            goto FreeMessage; // FLAG_ALLOCATED_PAYLOAD releases the generated SDP.
        }""")
    change("RtspConnection.c", "    ret = 0;\n\nExit:", """    ret = 0;
    FogDiagSet(FOG_DIAG_encryptionState, FOG_ENCRYPTION_ACCEPTED);

Exit:
    if (ret != 0) {
        if (ret != FOG_MOONLIGHT_ENCRYPTION_CONFLICT)
            FogDiagSet(FOG_DIAG_encryptionFailure, FOG_ENCRYPTION_FAILURE_HANDSHAKE);
        FogDiagSet(FOG_DIAG_encryptionState, FOG_ENCRYPTION_FAILED);
    }""")
    change("ControlStream.c", "    encryptedControlStream = APP_VERSION_AT_LEAST(7, 1, 431);",
           "    encryptedControlStream = APP_VERSION_AT_LEAST(7, 1, 431);\n    FogDiagSet(FOG_DIAG_encryptionControl, encryptedControlStream);")
    change("VideoStream.c", "            if (!decryptedOk) {", """            if (decryptedOk) FogDiagAdd(FOG_DIAG_videoDecryptSuccesses, 1);
            if (!decryptedOk) {""")
    change("AudioStream.c", '            Limelog("Failed to decrypt audio packet (sequence number: %u)\\n", rtp->sequenceNumber);',
           '            FogDiagAdd(FOG_DIAG_audioDecryptFailures, 1);\n            Limelog("Failed to decrypt audio packet (sequence number: %u)\\n", rtp->sequenceNumber);')
    change("AudioStream.c", "            LC_ASSERT_VT(false);\n            return;\n        }", "            LC_ASSERT_VT(false);\n            return;\n        }\n        FogDiagAdd(FOG_DIAG_audioDecryptSuccesses, 1);")
