#pragma once
#include "features/playstation/application/catalog_progress.hpp"
#include <functional>
#include <nlohmann/json.hpp>

namespace fog {
class HttpClient;
using PlayStationAvailabilityLoader = std::function<nlohmann::json(int page, int pageSize)>;
void updatePlayStationAvailability(std::vector<Computer>& computers, const PlayStationAvailabilityLoader& load,
                                  int pageSize = 100, int maxPages = 50,
                                  const PlayStationCatalogProgress& progress = {},
                                  const PlayStationCancellation& cancelled = {});
void updatePlayStationAvailability(std::vector<Computer>& computers, HttpClient& http,
                                  const PlayStationCatalogProgress& progress = {},
                                  const PlayStationCancellation& cancelled = {});
void updatePlayStationAvailability(std::vector<Computer>& computers);
}
