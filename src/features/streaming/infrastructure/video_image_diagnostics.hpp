#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace fog::detail {
// Bounded, allocation-free observation of the converted IYUV image, never GPU
// readback. A dark sample is a heuristic, not proof of a black remote desktop:
// unsampled details, legitimate dark scenes and decoder faults remain possible.
struct VideoImageSample {
    unsigned points = 0, yMin = 255, yMax = 0, yMean = 0, darkPoints = 0;
    unsigned chromaDeviation = 0;
    std::uint64_t fingerprint = 14695981039346656037ULL;
    bool nearBlack() const { return points && darkPoints == points && chromaDeviation <= 8; }

    static VideoImageSample inspect(const unsigned char* pixels, std::size_t size, int w, int h) {
        VideoImageSample sample;
        if (!pixels || w <= 0 || h <= 0 || w > 1920 || h > 1080) return sample;
        const auto lumaSize = static_cast<std::size_t>(w) * h;
        const auto cw = (w + 1) / 2, ch = (h + 1) / 2;
        const auto chromaSize = static_cast<std::size_t>(cw) * ch;
        if (size < lumaSize + 2 * chromaSize) return sample;
        const auto* u = pixels + lumaSize;
        const auto* v = u + chromaSize;
        const int columns = std::min(w, 32), rows = std::min(h, 18);
        unsigned sum = 0;
        for (int row = 0; row < rows; ++row) {
            const int y = (2 * row + 1) * h / (2 * rows);
            for (int column = 0; column < columns; ++column) {
                const int x = (2 * column + 1) * w / (2 * columns);
                const unsigned value = pixels[static_cast<std::size_t>(y) * w + x];
                const auto uv = static_cast<std::size_t>(y / 2) * cw + x / 2;
                sample.yMin = std::min(sample.yMin, value);
                sample.yMax = std::max(sample.yMax, value);
                sum += value;
                ++sample.points;
                if (value <= 24) ++sample.darkPoints;
                for (const unsigned component : {value, unsigned(u[uv]), unsigned(v[uv])}) {
                    sample.fingerprint ^= component;
                    sample.fingerprint *= 1099511628211ULL;
                }
                const unsigned ud = u[uv] > 128 ? u[uv] - 128 : 128 - u[uv];
                const unsigned vd = v[uv] > 128 ? v[uv] - 128 : 128 - v[uv];
                sample.chromaDeviation = std::max(sample.chromaDeviation, std::max(ud, vd));
            }
        }
        sample.yMean = sum / sample.points;
        return sample;
    }
};

// Owned exclusively by the application thread, like the texture itself.
struct VideoImageDiagnostics {
    VideoImageSample last;
    std::uint64_t samples = 0, blackFrames = 0, blackRuns = 0;
    std::uint64_t blackStreak = 0, blackStreakMax = 0;
    std::uint64_t unchangedStreak = 0, unchangedStreakMax = 0;
    int width = 0, height = 0;

    void uploaded(const VideoImageSample& sample, int w, int h) {
        const bool sameSize = width == w && height == h;
        const bool unchanged = samples && sameSize && last.fingerprint == sample.fingerprint;
        unchangedStreak = unchanged ? unchangedStreak + 1 : 0;
        unchangedStreakMax = std::max(unchangedStreakMax, unchangedStreak);
        if (sample.nearBlack()) {
            if (!blackStreak || !sameSize) ++blackRuns;
            blackStreak = sameSize ? blackStreak + 1 : 1;
            ++blackFrames;
            blackStreakMax = std::max(blackStreakMax, blackStreak);
        } else blackStreak = 0;
        ++samples;
        last = sample;
        width = w; height = h;
    }
};
}
