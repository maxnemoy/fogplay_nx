#include "features/catalog/presentation/catalog_artwork.hpp"
#include "features/catalog/infrastructure/artwork.hpp"
#include "features/catalog/infrastructure/steam_artwork.hpp"

#include <algorithm>
namespace fog {
SDL_Texture* catalogArtwork(const Game& game, ArtworkCache& artwork, SteamArtworkResolver& steamArtwork) {
    const auto& urls = game.artworkUrls;
    // MTS remains authoritative. Steam is a separate fallback, not a fifth
    // candidate silently excluded by the four-image catalog bound.
    if (auto* image = artwork.get(urls)) return image;
    const auto& id = game.id;
    const bool canResolve = !id.empty() && !game.name.empty();
    const auto& name = game.name;
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
