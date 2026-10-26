#pragma once
#include "features/playstation/domain/computer.hpp"
#include "features/catalog/domain/models.hpp"
#include "features/playstation/application/catalog_progress.hpp"
#include <functional>
#include <string>
#include <string_view>

namespace fog {
class HttpClient;
std::vector<Computer> playStationComputerPlaceholders();
std::vector<Computer> parsePlayStationComputers(std::string_view html, std::string_view promoSlug = "MamkinZabiyaka");
using PlayStationPromoPageLoader = std::function<std::string(int page)>;
std::string loadPlayStationPromoPages(std::string_view promoSlug, const PlayStationPromoPageLoader& load,
    const PlayStationCatalogProgress& progress = {}, const PlayStationCancellation& cancelled = {});
using PlayStationCatalogLoader = std::function<std::string(std::string_view)>;
std::vector<Computer> fetchPlayStationComputers(const PlayStationCatalogLoader& load,
    const PlayStationCatalogProgress& progress = {}, const PlayStationCancellation& cancelled = {});
std::vector<Computer> fetchPlayStationComputers(HttpClient& http, const PlayStationCatalogProgress& progress = {},
    const PlayStationCancellation& cancelled = {});
std::vector<Computer> fetchPlayStationComputers();
std::vector<Computer> fetchPlayStationComputers(const PlayStationCatalogProgress& progress, const PlayStationCancellation& cancelled);
}
