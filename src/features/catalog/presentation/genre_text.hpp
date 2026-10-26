#pragma once
#include "core/constants/ui_text.hpp"
#include "features/catalog/domain/catalog.hpp"

namespace fog::ui {
inline const char* genreLabel(const CatalogGenre& genre) {
    namespace text = constants::ui::Catalog;
    switch (genre.id) {
        case 173: return text::Shooter;
        case 13: return text::Racing;
        case 75: return text::Horror;
        case 93: return text::Fantasy;
        case 4: return text::Action;
        case 21: return text::Multiplayer;
        case 7: return text::Indie;
        case 8: return text::Strategy;
        case 9: return text::Rpg;
        case 112: return text::Anime;
        case 12: return text::Simulation;
        case 15: return text::Casual;
        case 20: return text::Sport;
        case 94: return text::Arcade;
        case 176: return text::Survival;
        case 212: return text::Platformer;
        case 309: return text::Fighting;
        case 331: return text::Mmorpg;
        default: return genre.slug;
    }
}
}
