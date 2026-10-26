#pragma once
#include "core/constants/storage_paths.hpp"
#include "features/catalog/infrastructure/artwork_url.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct SDL_Renderer;
struct SDL_Texture;

namespace fog {
constexpr std::size_t maxArtworkBytes = 4 * 1024 * 1024;
constexpr std::size_t maxArtworkPixels = 1024 * 1024;
// Only anonymous public FogPlay previews and canonical Steam header images are permitted.
struct ArtworkPixels {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
};
// Inspect WebP/JPEG/PNG dimensions before allocating decoded pixels. Throws on invalid data.
ArtworkPixels decodeArtwork(const std::vector<std::uint8_t>& bytes);

// Compressed public WebP/JPEG/PNG files only. Empty directory disables disk storage on hosts.
// Limits include metadata and in-progress temporary files; lower values aid host tests.
struct ArtworkDiskOptions {
#ifdef __SWITCH__
    std::string directory = fog::constants::storage::path(fog::constants::storage::ArtworkCache);
#else
    std::string directory;
#endif
    std::size_t maxBytes = 128 * 1024 * 1024;
    std::size_t maxFiles = 256;
};

class ArtworkCache {
public:
    using Downloader = std::function<std::vector<std::uint8_t>(const std::string&, const std::atomic_bool&)>;
    explicit ArtworkCache(Downloader downloader = {}, ArtworkDiskOptions disk = {});
    ~ArtworkCache();
    ArtworkCache(const ArtworkCache&) = delete;
    ArtworkCache& operator=(const ArtworkCache&) = delete;
    // All public methods run on the UI thread. request never waits for network I/O.
    bool request(const std::string& url);
    // Try later catalog images only after an earlier candidate fails validation/download/decode.
    bool request(const std::vector<std::string>& urls);
    // True only when every valid candidate (within the four-URL limit) has failed.
    // Untried, queued, ready, cached, or retryable candidates keep this false.
    // No valid candidates is exhausted; queue capacity never counts as a failure.
    bool exhausted(const std::vector<std::string>& urls) const;
    void update(SDL_Renderer* renderer);
    // Borrowed pointer, valid until the next update/stop; null while unavailable.
    SDL_Texture* get(const std::string& url);
    SDL_Texture* get(const std::vector<std::string>& urls);
    // Cancels and joins the worker and frees textures. Call before SDL teardown.
    void stop() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
