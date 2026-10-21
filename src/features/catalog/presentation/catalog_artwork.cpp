#include "features/catalog/presentation/catalog_artwork.hpp"
#include "features/catalog/infrastructure/artwork.hpp"
#include "features/catalog/infrastructure/steam_artwork.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
namespace fog {
SDL_Texture* catalogArtwork(const nlohmann::json& game, ArtworkCache& artwork, SteamArtworkResolver& steamArtwork) {
    std::vector<std::string> urls;
    const auto images = game.find("images");
    if (images != game.end() && images->is_array()) {
        const auto count = std::min<std::size_t>(images->size(),16);
        for (std::size_t index=0; index<count && urls.size()<fog::maxArtworkCandidates; ++index) {
            const auto& candidate = images->at(index);
            if (!candidate.is_string()) continue;
            const auto url = candidate.get<std::string>();
            if (fog::validArtworkUrl(url) && std::find(urls.begin(),urls.end(),url)==urls.end()) urls.push_back(url);
        }
    }
    // MTS remains authoritative. Steam is a separate fallback, not a fifth
    // candidate silently excluded by the four-image catalog bound.
    if (auto* image = artwork.get(urls)) return image;
    const auto identity = game.find("id"), title = game.find("name");
    const bool canResolve = identity != game.end() && identity->is_number_integer() &&
        *identity > 0 && *identity <= 2147483647 && title != game.end() && title->is_string();
    const int id = canResolve ? identity->get<int>() : 0;
    const std::string name = canResolve ? title->get<std::string>() : std::string{};
    artwork.request(urls);
    auto steamUrl = canResolve ? steamArtwork.get(id,name) : std::string{};
    if (!steamUrl.empty()) {
        // Keep serving the cover while optional mapping refresh or MTS retry runs.
        steamArtwork.request(id,name);
        if (auto* image = artwork.get(steamUrl)) return image;
    }
    // Full queues and pending requests are not failed MTS downloads.
    if (!artwork.exhausted(urls) || !canResolve) return nullptr;
    steamArtwork.request(id,name);
    steamUrl = steamArtwork.get(id,name);
    if (steamUrl.empty()) return nullptr;
    artwork.request(steamUrl);
    return artwork.get(steamUrl);
}
}
