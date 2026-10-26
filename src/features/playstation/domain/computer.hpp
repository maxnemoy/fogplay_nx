#pragma once
#include <array>

namespace fog {
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
}
