#pragma once
#include "features/catalog/domain/models.hpp"
#include <cstddef>
#include <optional>
#include <string>
#include <vector>
#include <utility>

namespace fog {
struct RecentGame {
    EntityId id;
    std::string name, artwork;
    std::vector<std::string> alternateArtwork{};
    std::vector<std::string> artworkCandidates() const;
    static std::optional<RecentGame> fromCatalog(const Game& game);
    Game catalogItem() const;
};
// Public game metadata only. No account, session, order, token or launch arguments.
// Main-thread store; failures remain observable and never affect rental ownership.
class RecentGames {
public:
    static constexpr std::size_t Limit = 5, MaxBytes = 32768;
    explicit RecentGames(std::string path) : path_(std::move(path)) {}
    bool load();
    // Updates the in-memory list even if SD persistence fails.
    bool remember(const RecentGame& game);
    bool update(const RecentGame& game);
    bool save();
    const std::vector<RecentGame>& items() const { return items_; }
    const std::string& error() const { return error_; }
private:
    std::string path_, error_;
    std::vector<RecentGame> items_;
};
// A catalog click is only metadata selection. A record becomes eligible after
// that new rental starts AND produces its first decoded frame. Reconnecting an
// existing rental must explicitly discard any previously selected catalog game.
class RecentLaunch {
public:
    void prepare(std::optional<RecentGame> game) { prepared_ = std::move(game); active_.reset(); }
    void stream(bool createdFromCatalog) {
        active_ = createdFromCatalog ? std::move(prepared_) : std::nullopt;
        prepared_.reset();
    }
    std::optional<RecentGame> decoded() { auto game = std::move(active_); active_.reset(); return game; }
    void close() { prepared_.reset(); active_.reset(); }
private:
    std::optional<RecentGame> prepared_, active_;
};
}
