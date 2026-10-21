#pragma once
#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace fog::detail {
// These exact formats form a private, numeric-only contract with our pinned
// FFmpeg overlay. Never format or retain any other FFmpeg message.
inline constexpr char nvdecStatusFormat[] = "FOG_NVDEC_STATUS_V1 error_status=%u mbs_error=%u mbs_ok=%u cycles_raw=%u slice_header_error=%u\n";
inline constexpr char nvdecWaitFormat[] = "FOG_NVDEC_WAIT_V1 code=%d\n";
struct DecoderHardwareStatus {
    std::uint64_t events = 0;
    unsigned error = 0, badBlocks = 0, goodBlocks = 0, cycles = 0, headerError = 0;
    int waitCode = 0;
    bool hasStatus = false;
};
class DecoderDiagnosticCapture {
public:
    // Context keys are compared only, never dereferenced by the callback.
    void registerContext(void* context) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& entry : entries_) {
            if (!entry.context) { entry = {context, {}}; return; }
        }
    }
    void unregisterContext(void* context) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& entry : entries_) if (entry.context == context) entry = {};
    }
    DecoderHardwareStatus snapshot(void* context) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : entries_) if (entry.context == context) return entry.status;
        return {};
    }
    void capture(void* context, const char* format, va_list args) {
        if (!context || !format) return;
        const bool isStatus = std::strcmp(format, nvdecStatusFormat) == 0;
        const bool isWait = std::strcmp(format, nvdecWaitFormat) == 0;
        if (!isStatus && !isWait) return;
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& entry : entries_) {
            if (entry.context != context) continue;
            auto& status = entry.status;
            ++status.events;
            status.hasStatus = isStatus;
            if (isStatus) {
                status.error = va_arg(args, unsigned);
                status.badBlocks = va_arg(args, unsigned);
                status.goodBlocks = va_arg(args, unsigned);
                status.cycles = va_arg(args, unsigned);
                status.headerError = va_arg(args, unsigned);
                status.waitCode = 0;
            } else {
                status.error = status.badBlocks = status.goodBlocks = status.cycles = status.headerError = 0;
                status.waitCode = va_arg(args, int);
            }
            return;
        }
    }
private:
    struct Entry { void* context = nullptr; DecoderHardwareStatus status; };
    // One Media decoder plus a candidate during reset; bounded, no allocation.
    std::array<Entry, 8> entries_{};
    mutable std::mutex mutex_;
};
}
