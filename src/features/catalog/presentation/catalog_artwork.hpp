#pragma once
#include <nlohmann/json_fwd.hpp>
struct SDL_Texture;
namespace fog {
class ArtworkCache;
class SteamArtworkResolver;
// UI-thread routing only; both dependencies perform disk/network work off-thread.
SDL_Texture* catalogArtwork(const nlohmann::json& game, ArtworkCache& artwork, SteamArtworkResolver& steamArtwork);
}
