#pragma once
#include <array>
#include <cstddef>
#include <cstring>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace fog::detail {
enum class MediaPixelFormat { Iyuv, Nv12 };
struct MediaPixelLayout {
    int width, height, chromaWidth, chromaHeight;
    std::size_t ySize, chromaSize, size;
    static MediaPixelLayout make(int w, int h) {
        if (w < 1 || h < 1 || w > 1920 || h > 1080)
            throw std::invalid_argument("Invalid media pixel dimensions");
        const auto y = static_cast<std::size_t>(w) * h;
        const auto c = static_cast<std::size_t>((w + 1) / 2) * ((h + 1) / 2);
        return {w, h, (w + 1) / 2, (h + 1) / 2, y, c, y + 2 * c};
    }
};
inline void copyMediaPlane(unsigned char* target, int targetPitch, const unsigned char* source,
    int sourcePitch, int rowBytes, int rows) {
    // FFmpeg data points at logical row zero, even for a negative linesize.
    if (!source || !target || std::int64_t(sourcePitch) < -2147483647LL ||
        (sourcePitch < 0 ? -std::int64_t(sourcePitch) : sourcePitch) < rowBytes)
        throw std::invalid_argument("Invalid media plane stride");
    for (int row = 0; row < rows; ++row)
        std::memcpy(target + static_cast<std::ptrdiff_t>(row) * targetPitch,
            source + static_cast<std::ptrdiff_t>(row) * sourcePitch, rowBytes);
}
inline void mediaNv12ToIyuv(const unsigned char* source, unsigned char* target, const MediaPixelLayout& layout) {
    std::memcpy(target, source, layout.ySize);
    for (std::size_t i = 0; i < layout.chromaSize; ++i) {
        target[layout.ySize + i] = source[layout.ySize + 2 * i];
        target[layout.ySize + layout.chromaSize + i] = source[layout.ySize + 2 * i + 1];
    }
}
// Cache only two spare buffers; queue/UI/decoder own their buffers exclusively.
// A released buffer cannot be acquired until its Frame has relinquished it.
class MediaCpuBufferPool {
public:
    static constexpr std::size_t capacity = 2;
    static constexpr std::size_t maximumBytes = 1920 * 1080 * 3 / 2;
    std::vector<unsigned char> acquire() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& slot : spare_) if (slot.capacity()) {
            std::vector<unsigned char> result;
            result.swap(slot);
            return result;
        }
        return {};
    }
    void recycle(std::vector<unsigned char>& pixels) noexcept {
        if (!pixels.capacity() || pixels.capacity() > maximumBytes) return;
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& slot : spare_) if (!slot.capacity()) {
            pixels.clear(); slot.swap(pixels); return;
        }
    }
    void clear() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& slot : spare_) std::vector<unsigned char>().swap(slot);
    }
private:
    std::mutex mutex_;
    std::array<std::vector<unsigned char>, capacity> spare_;
};
}
