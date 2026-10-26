#pragma once
#include "features/playstation/domain/computer.hpp"
#include "features/session/domain/service_models.hpp"
#include "features/playstation/infrastructure/catalog_progress.hpp"
#include <functional>
#include <string>
#include <string_view>

namespace fog {
class HttpClient;
Json playStationComputerPlaceholders();
Json parsePlayStationComputers(std::string_view html, std::string_view promoSlug = "MamkinZabiyaka");
using PlayStationPromoPageLoader = std::function<std::string(int page)>;
std::string loadPlayStationPromoPages(std::string_view promoSlug, const PlayStationPromoPageLoader& load,
    const PlayStationCatalogProgress& progress = {}, const PlayStationCancellation& cancelled = {});
using PlayStationCatalogLoader = std::function<std::string(std::string_view)>;
Json fetchPlayStationComputers(const PlayStationCatalogLoader& load,
    const PlayStationCatalogProgress& progress = {}, const PlayStationCancellation& cancelled = {});
Json fetchPlayStationComputers(HttpClient& http, const PlayStationCatalogProgress& progress = {},
    const PlayStationCancellation& cancelled = {});
Json fetchPlayStationComputers();
}
