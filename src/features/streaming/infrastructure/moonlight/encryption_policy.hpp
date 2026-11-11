#pragma once
#include <FogMoonlightDiagnostics.h>
#include <Limelight.h>
#include <cstdint>
#include <string>

#ifndef FOGPLAY_MOONLIGHT_ENCRYPTION
#define FOGPLAY_MOONLIGHT_ENCRYPTION 0
#endif
#if FOGPLAY_MOONLIGHT_ENCRYPTION != 0 && FOGPLAY_MOONLIGHT_ENCRYPTION != 1
#error "FOGPLAY_MOONLIGHT_ENCRYPTION must be 0 or 1"
#endif
namespace moonlight::detail {
inline constexpr int encryptionFlags() noexcept {
    return FOGPLAY_MOONLIGHT_ENCRYPTION ? static_cast<int>(ENCFLG_ALL) : ENCFLG_NONE;
}
inline std::string encryptionSummary(const FogMoonlightDiagnostics& value) {
    if (value.encryptionState == FOG_ENCRYPTION_FAILED) return "Шифрование: согласование не удалось";
    if (value.encryptionState != FOG_ENCRYPTION_ACCEPTED) return "Шифрование: режим не подтверждён";
    return std::string("Видео ") + (value.encryptionSelected & 2 ? "AES-GCM" : "без шифрования") +
        "; аудио " + (value.encryptionAudio ? "AES-CBC" : "без шифрования") + " (согласовано)";
}
inline std::string encryptionDiagnostics(const FogMoonlightDiagnostics& value) {
    return " enc_requested=" + std::to_string(static_cast<std::uint32_t>(encryptionFlags())) +
        " enc_core_request=" + std::to_string(value.encryptionClientFlags) +
        " enc_state=" + std::to_string(value.encryptionState) + " enc_failure=" + std::to_string(value.encryptionFailure) +
        " enc_host_supported=" + std::to_string(value.encryptionHostSupported) +
        " enc_host_required=" + std::to_string(value.encryptionHostRequired) +
        " enc_selected=" + std::to_string(value.encryptionSelected) +
        " enc_audio=" + std::to_string(value.encryptionAudio) +
        " enc_control=" + std::to_string(value.encryptionControl) +
        " enc_rtsp=" + std::to_string(value.encryptionRtsp) +
        " enc_input_protocol=always" +
        " enc_observed_video=" + std::to_string(value.videoDecryptSuccesses) +
        " enc_observed_audio=" + std::to_string(value.audioDecryptSuccesses) +
        " enc_audio_failures=" + std::to_string(value.audioDecryptFailures);
}
}
