#pragma once
#include "features/catalog/domain/models.hpp"
#include <memory>
namespace fog {
class Api;
class HttpClient;
class MtsCatalogRepository final : public CatalogRepository {
public:
    explicit MtsCatalogRepository(Api& api);
    Page<Game> games(const GameQuery&) override;
    Game game(const EntityId&) override;
    Page<Computer> computers(const ComputerQuery&) override;
    AutomaticComputers recommend(const ComputerQuery&) override;
    std::vector<Computer> playStationComputers(const CatalogProgress& = {}, const CatalogCancellation& = {}) override;
    void cancel() noexcept override;
private:
    Api& api_;
    std::shared_ptr<HttpClient> publicHttp_;
};
}
