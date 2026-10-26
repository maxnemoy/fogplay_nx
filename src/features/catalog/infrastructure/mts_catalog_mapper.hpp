#pragma once
#include "features/catalog/domain/models.hpp"
#include <nlohmann/json.hpp>
namespace fog {
EntityId mtsEntityId(int id);
int mtsNumericId(const EntityId& id);
Game mapMtsGame(const nlohmann::json& value);
Computer mapMtsComputer(const nlohmann::json& value);
Page<Game> mapMtsGames(const nlohmann::json& value);
Page<Computer> mapMtsComputers(const nlohmann::json& value);
AutomaticComputers mapMtsRecommendations(const nlohmann::json& value);
std::vector<HistoryEntry> mapMtsHistory(const nlohmann::json& value);
std::optional<Money> mapMtsMoney(const nlohmann::json& value);
}
