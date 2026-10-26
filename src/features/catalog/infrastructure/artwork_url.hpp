#pragma once
#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
namespace fog {
constexpr std::size_t maxArtworkCandidates = 4;
inline bool validArtworkUrl(const std::string& url) noexcept {
    if (url.size() > 1024) return false;
    // The resolver generates only canonical app headers. Do not accept general
    // Steam URLs, extra path segments, auth, queries, or arbitrary CDN subdomains.
    for (const std::string_view prefix : {
            "https://shared.cloudflare.steamstatic.com/store_item_assets/steam/apps/",
            "https://shared.akamai.steamstatic.com/store_item_assets/steam/apps/",
            "https://shared.steamstatic.com/store_item_assets/steam/apps/"}) {
        constexpr std::string_view suffix = "/header.jpg";
        if (url.compare(0, prefix.size(), prefix) != 0) continue;
        if (url.size() <= prefix.size() + suffix.size() ||
            url.compare(url.size() - suffix.size(), suffix.size(), suffix) != 0) return false;
        const auto id = std::string_view(url).substr(prefix.size(),
            url.size() - prefix.size() - suffix.size());
        return id.front() >= '1' && id.front() <= '9' &&
            std::all_of(id.begin(), id.end(), [](unsigned char c) { return c >= '0' && c <= '9'; });
    }
    constexpr std::string_view prefix = "https://fogplay.mts.ru/image/preview/";
    if (url.size() <= prefix.size() || url.size() > 1024 ||
        url.compare(0, prefix.size(), prefix) != 0 ||
        (!(url.size() >= 5 && url.compare(url.size() - 5, 5, ".webp") == 0) &&
        !(url.size() >= 4 && (url.compare(url.size() - 4, 4, ".jpg") == 0 ||
                            url.compare(url.size() - 4, 4, ".png") == 0)) &&
        !(url.size() >= 5 && url.compare(url.size() - 5, 5, ".jpeg") == 0))) return false;
    const auto path = std::string_view(url).substr(prefix.size());
    if (path.find("..") != std::string_view::npos || path.front() == '/') return false;
    return std::all_of(path.begin(), path.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '/' || c == '.' || c == '-' || c == '_';
    });
}
}
