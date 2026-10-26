#pragma once
#include "features/catalog/domain/models.hpp"
struct SDL_Texture;
namespace fog {
class ArtworkCache;
class SteamArtworkResolver;
// UI-thread routing only; both dependencies perform disk/network work off-thread.
SDL_Texture* catalogArtwork(const Game& game, ArtworkCache& artwork, SteamArtworkResolver& steamArtwork);
}
