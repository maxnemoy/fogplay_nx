#pragma once
#include "features/playstation/infrastructure/catalog_progress.hpp"
#include <functional>

namespace fog {
class HttpClient;
using PlayStationAvailabilityLoader = std::function<Json(int page, int pageSize)>;
void updatePlayStationAvailability(Json& computers, const PlayStationAvailabilityLoader& load,
                                  int pageSize = 100, int maxPages = 50,
                                  const PlayStationCatalogProgress& progress = {},
                                  const PlayStationCancellation& cancelled = {});
void updatePlayStationAvailability(Json& computers, HttpClient& http,
                                  const PlayStationCatalogProgress& progress = {},
                                  const PlayStationCancellation& cancelled = {});
void updatePlayStationAvailability(Json& computers);
}
