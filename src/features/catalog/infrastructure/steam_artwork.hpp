#pragma once
#include "core/constants/storage_paths.hpp"
#include "core/domain/identity.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace fog {
struct SteamSearchResponse {
    long status = 0;
    std::string body;
};
struct SteamArtworkOptions {
#ifdef __SWITCH__
    std::string directory = fog::constants::storage::path(fog::constants::storage::SteamCache);
#else
    std::string directory;
#endif
    std::size_t maxRecords = 256;
    std::size_t maxPending = 32;
    // Unix seconds; the default clock is system time. Primarily for offline tests.
    std::function<std::int64_t()> clock;
};
struct SteamArtworkStats {
    std::size_t records = 0;
    std::size_t pending = 0;
    bool ready = false;
};
class SteamArtworkResolver {
public:
    // Downloader receives a title, never an MTS token. Non-200 / malformed replies
    // are transient failures. Custom downloaders must honor cancellation.
    using Downloader = std::function<SteamSearchResponse(const std::string&, const std::atomic_bool&)>;
    explicit SteamArtworkResolver(Downloader downloader = {}, SteamArtworkOptions options = {});
    ~SteamArtworkResolver();
    SteamArtworkResolver(const SteamArtworkResolver&) = delete;
    SteamArtworkResolver& operator=(const SteamArtworkResolver&) = delete;
    // Bounded CPU/mutex work only: disk and network run exclusively on the worker.
    // True means the lookup is cached, pending, or accepted; false means rejected/full/stopped.
    bool request(int mtsId, const std::string& title);
    bool request(const EntityId& id, const std::string& title);
    // Returns a generated Steam header URL or empty. Stale positive mappings remain
    // usable during offline refresh; request() initiates refresh after 30 days.
    std::string get(int mtsId, const std::string& title) const;
    std::string get(const EntityId& id, const std::string& title) const;
    SteamArtworkStats stats() const;
    // Cancels network work, drops queued requests, and joins the worker.
    void stop() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}