#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace fog::foghub {
// Provider page DTOs. HTML details never enter the common domain models.
struct LoginForm { std::string csrf; };
// Login identity only: the site exposes no verified immutable numeric account ID.
struct AccountPage { std::string accountId, displayName; };
enum class HistoryState { Active, Finished, Unknown };
struct HistoryRow {
    // startedAt contains the observed date and start time. finishedAt is the
    // observed HH:mm only: the table does not establish an end date.
    std::string id, computerId, computerName, startedAt, finishedAt, durationMinutes, amount;
    HistoryState state{HistoryState::Unknown};
};
struct HistoryPage { std::vector<HistoryRow> rows; std::string nextPath; std::size_t totalItems{0}; };
struct GameRow { std::string id, name, imageUrl; };
struct GamesPage { std::vector<GameRow> rows; std::string nextPath; std::size_t totalItems{0}, firstIndex{0}; };
struct ComputerRow {
    std::string id, name, owner, description, pricePerHour;
    bool online{false}, busy{false}, fhAvailable{false};
};
struct ComputersPage { std::vector<ComputerRow> rows; std::string nextPath; std::size_t totalItems{0}, firstIndex{0}; GameRow game; };

LoginForm parseLogin(const std::string& html);
AccountPage parseAccount(const std::string& html);
HistoryPage parseHistory(const std::string& html);
GamesPage parseGames(const std::string& html);
ComputersPage parseComputers(const std::string& html, const std::string& gameId);
}
