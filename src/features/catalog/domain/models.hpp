#pragma once
#include "core/domain/identity.hpp"
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <stdexcept>
#include <vector>

namespace fog {
template<class T> struct Page { std::vector<T> items; };
struct Game {
    EntityId id;
    std::string name, description, rating;
    std::vector<std::string> artworkUrls;
};
enum class Availability { Unknown, Online, Offline, Busy };
struct RouteLatency { std::string routeKey; int serverToNodeMs = 0; };
struct Computer {
    EntityId id;
    std::string name, cpu, gpu, ram, price;
    std::optional<Money> hourlyPrice;
    EntityId offerId;
    Availability availability = Availability::Unknown;
    bool listed = false, launchable = false;
    std::vector<RouteLatency> latencyRoutes;
};
struct AutomaticComputers {
    std::vector<Computer> cheapest, balanced, powerful;
    const std::vector<Computer>& at(std::size_t index) const {
        if (index==0) return cheapest;
        if (index==1) return balanced;
        if (index==2) return powerful;
        throw std::out_of_range("Invalid automatic computer category");
    }
};
struct GameDetail { Game game; AutomaticComputers recommendations; };
enum class HistoryStatus { Unknown, Active, Finished };
struct HistoryEntry {
    std::string computerName, startedAt, finishedAt, durationMinutes, amount;
    std::optional<Money> total;
    EntityId id{};
    EntityId computerId{};
    HistoryStatus status = HistoryStatus::Unknown;
};
struct GameQuery { std::string search; int page = 1, pageSize = 16, genreId = 0; };
struct ComputerQuery { EntityId gameId; int page = 1, pageSize = 5; bool ownSteamAccount = false; };
using CatalogProgress = std::function<void(const std::vector<Computer>&)>;
using CatalogCancellation = std::function<bool()>;
class CatalogRepository {
public:
    virtual ~CatalogRepository() = default;
    virtual Page<Game> games(const GameQuery&) = 0;
    virtual Game game(const EntityId&) = 0;
    virtual Page<Computer> computers(const ComputerQuery&) = 0;
    virtual AutomaticComputers recommend(const ComputerQuery&) = 0;
    virtual std::vector<Computer> playStationComputers(const CatalogProgress& = {}, const CatalogCancellation& = {}) = 0;
    virtual void cancel() noexcept = 0;
};
inline const Computer* findPlayStationComputer(const std::vector<Computer>& computers, const EntityId& id) {
    for (const auto& computer : computers) if (computer.id == id) return &computer;
    return nullptr;
}
inline bool playStationLaunchable(const Computer& computer) noexcept {
    return computer.launchable && computer.listed &&
           !computer.id.empty() && !computer.offerId.empty();
}
inline bool sameOffer(const Computer& first, const Computer& second) noexcept {
    return first.id == second.id && first.offerId == second.offerId && first.price == second.price;
}
}
