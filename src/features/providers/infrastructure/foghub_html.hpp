#pragma once
#include <cstddef>
#include <cstdint>
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

// Derived only from the observed form field metadata, not a captured server
// template. Keep the original validated value for submission; never log it.
struct MoonlightEndpoint { std::string host; std::uint16_t port{0}; };
struct ConnectForm { std::string csrf, actionPath, moonlightUrl; MoonlightEndpoint endpoint; };
struct LaunchSuccess { std::string rentalId; };
enum class CloseMethod { Get };
// GET is inferred from an ordinary href with no data-method. This DTO is not
// evidence that the endpoint's wire method or idempotency was verified.
struct CloseAction { std::string path; CloseMethod method{CloseMethod::Get}; };

LoginForm parseLogin(const std::string& html);
AccountPage parseAccount(const std::string& html);
HistoryPage parseHistory(const std::string& html);
GamesPage parseGames(const std::string& html);
ComputersPage parseComputers(const std::string& html, const std::string& gameId);
ComputerRow parseComputerPreview(const std::string& html, const std::string& computerId);
MoonlightEndpoint parseMoonlightEndpoint(const std::string& value);
ConnectForm parseConnectForm(const std::string& html, const std::string& expectedComputerId);
// Empty when a PIN form already exists; otherwise returns only the explicit
// same-PC resume=0 read link observed on the provider's previous-client page.
std::string freshPairingLink(const std::string& html, const std::string& expectedComputerId);
LaunchSuccess parseLaunchSuccess(const std::string& html, const std::string& expectedComputerId,
    const std::string& effectivePath);
// Caller must provide a fresh, authenticated history response; HTML alone
// cannot establish freshness, account ownership, or authorization to close.
CloseAction parseCloseAction(const std::string& html, const std::string& expectedRentalId);
}
