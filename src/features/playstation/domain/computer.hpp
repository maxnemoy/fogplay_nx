#pragma once
#include <array>
#include "core/domain/identity.hpp"
#include "features/catalog/domain/models.hpp"

namespace fog {
std::vector<Computer> playStationComputerPlaceholders();
struct PlayStationComputer { int id; const char* name; const char* promoSlug; };
inline constexpr std::array<PlayStationComputer, 7> PlayStationComputers{{
    {11573, "3A7X47", "MamkinZabiyaka"}, {12921, "3A7X49", "MamkinZabiyaka"},
    {11575, "3A7X48", "MamkinZabiyaka"}, {13025, "F4BX8", "Karahun"},
    {7705, "61EFX15", "CrazyGames"}, {5632, "61EFX4", "CrazyGames"},
    {5510, "61EFX3", "CrazyGames"}
}};
inline bool isPlayStationComputerId(int id) noexcept {
    for (const auto& computer : PlayStationComputers)
        if (computer.id == id) return true;
    return false;
}
inline bool isPlayStationComputerId(const EntityId& id) noexcept {
    if (id.provider != ProviderId::Mts) return false;
    for (const auto& computer : PlayStationComputers)
        if (id.value == std::to_string(computer.id)) return true;
    return false;
}
}
