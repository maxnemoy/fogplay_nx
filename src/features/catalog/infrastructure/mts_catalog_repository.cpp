#include "features/catalog/infrastructure/mts_catalog_repository.hpp"
#include "features/catalog/infrastructure/mts_catalog_mapper.hpp"
#include "features/session/infrastructure/api.hpp"
#include "core/network/http.hpp"
#include "features/session/infrastructure/mts_operation.hpp"
#include "features/playstation/infrastructure/catalog.hpp"
namespace fog {
MtsCatalogRepository::MtsCatalogRepository(Api& api) : api_(api), publicHttp_(std::make_shared<HttpClient>()) {}
Page<Game> MtsCatalogRepository::games(const GameQuery& query) { return mtsOperation([&]{ return mapMtsGames(api_.games(query.search,query.page,query.pageSize,query.genreId)); }); }
Game MtsCatalogRepository::game(const EntityId& id) { return mtsOperation([&]{ return mapMtsGame(api_.gameDetail(mtsNumericId(id))); }); }
Page<Computer> MtsCatalogRepository::computers(const ComputerQuery& query) { return mtsOperation([&]{ return mapMtsComputers(api_.computers(mtsNumericId(query.gameId),query.page,query.pageSize,query.ownSteamAccount)); }); }
AutomaticComputers MtsCatalogRepository::recommend(const ComputerQuery& query) { return mtsOperation([&]{ return mapMtsRecommendations(api_.computerAuto(mtsNumericId(query.gameId),query.ownSteamAccount)); }); }
std::vector<Computer> MtsCatalogRepository::playStationComputers(const CatalogProgress& progress,const CatalogCancellation& cancelled) { return mtsOperation([&]{ return fetchPlayStationComputers(*publicHttp_,progress,cancelled); }); }
void MtsCatalogRepository::cancel() noexcept { api_.cancel(); publicHttp_->cancel(); }
}
