#include "features/catalog/infrastructure/recent_games.hpp"
#include "features/catalog/infrastructure/artwork.hpp"
#include "core/storage/storage.hpp"
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unordered_set>
#include <unistd.h>

namespace fog {
namespace {
using Json = nlohmann::json;
bool validName(const std::string& name) {
    if (name.empty() || name.size() > 256 ||
        std::none_of(name.begin(), name.end(), [](unsigned char c) { return c != ' '; }) ||
        std::any_of(name.begin(), name.end(), [](unsigned char c) { return c < 32 || c == 127; })) return false;
    try { Json(name).dump(); return true; } catch (...) { return false; }
}
bool validGame(const RecentGame& game) {
    return game.id > 0 && validName(game.name) && (game.artwork.empty() || validArtworkUrl(game.artwork)) &&
        game.alternateArtwork.size() < maxArtworkCandidates &&
        (game.alternateArtwork.empty() || !game.artwork.empty()) &&
        std::all_of(game.alternateArtwork.begin(), game.alternateArtwork.end(), validArtworkUrl);
}
std::optional<RecentGame> parse(const Json& item, bool persisted) {
    if (!item.is_object() || !item.contains("id") || !item.at("id").is_number_integer() ||
        item.at("id") <= 0 || item.at("id") > 2147483647 || !item.contains("name") || !item.at("name").is_string()) return std::nullopt;
    RecentGame game{item.at("id").get<int>(), item.at("name").get<std::string>(), {}};
    if (persisted) {
        if ((item.size() != 3 && item.size() != 4) || !item.contains("artwork") || !item.at("artwork").is_string()) return std::nullopt;
        if (item.size() == 4) {
            const auto alternates = item.find("alternateArtwork");
            if (alternates == item.end() || !alternates->is_array() || alternates->size() >= maxArtworkCandidates)
                return std::nullopt;
            for (const auto& url : *alternates) {
                if (!url.is_string()) return std::nullopt;
                game.alternateArtwork.push_back(url.get<std::string>());
            }
        }
        game.artwork = item.at("artwork").get<std::string>();
    } else {
        const auto images = item.find("images");
        if (images != item.end() && images->is_array()) {
            // Catalog image order remains authoritative; invalid entries never block alternatives.
            std::size_t inspected = 0;
            for (const auto& entry : *images) {
                if (++inspected > 16 || game.alternateArtwork.size() + !game.artwork.empty() >= maxArtworkCandidates) break;
                if (!entry.is_string()) continue;
                auto url = entry.get<std::string>();
                if (!validArtworkUrl(url)) continue;
                if (game.artwork.empty()) game.artwork = std::move(url);
                else if (url != game.artwork && std::find(game.alternateArtwork.begin(), game.alternateArtwork.end(), url) == game.alternateArtwork.end())
                    game.alternateArtwork.push_back(std::move(url));
            }
        }
    }
    return validGame(game) ? std::optional<RecentGame>(std::move(game)) : std::nullopt;
}
std::string readFile(const std::string& path, bool& missing) {
    int flags = O_RDONLY;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
#ifdef O_NONBLOCK
    flags |= O_NONBLOCK;
#endif
    const int fd = open(path.c_str(), flags);
    if (fd < 0) {
        if (errno == ENOENT) { missing = true; return {}; }
        throw std::runtime_error("Recent games read failed");
    }
    struct stat info{};
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 1 ||
        static_cast<std::uint64_t>(info.st_size) > RecentGames::MaxBytes) {
        close(fd); throw std::runtime_error("Invalid recent games file");
    }
    std::string bytes(static_cast<std::size_t>(info.st_size), '\0');
    std::size_t offset = 0; bool failed = false;
    while (offset < bytes.size()) {
        const auto count = read(fd, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { failed = true; break; }
        offset += static_cast<std::size_t>(count);
    }
    char extra{}; ssize_t count;
    do { count = read(fd, &extra, 1); } while (count < 0 && errno == EINTR);
    if (count != 0) failed = true;
    if (close(fd) != 0) failed = true;
    if (failed) throw std::runtime_error("Recent games read failed");
    return bytes;
}
}
std::optional<RecentGame> RecentGame::fromCatalog(const nlohmann::json& game) { return parse(game, false); }
std::vector<std::string> RecentGame::artworkCandidates() const {
    auto result = alternateArtwork;
    if (!artwork.empty()) result.insert(result.begin(), artwork);
    return result;
}
nlohmann::json RecentGame::catalogItem() const {
    return {{"id", id}, {"name", name}, {"images", artworkCandidates()}};
}
bool RecentGames::load() {
    try {
        recoverStorage(path_);
        bool missing = false;
        const auto bytes = readFile(path_, missing);
        if (missing) { items_.clear(); error_.clear(); return true; }
        const auto data = Json::parse(bytes, nullptr, false);
        if (!data.is_object() || data.size() != 2 || !data.contains("version") ||
            !data.at("version").is_number_integer() || data.at("version") != 1 ||
            !data.contains("games") || !data.at("games").is_array() || data.at("games").size() > Limit)
            throw std::runtime_error("Invalid recent games file");
        std::vector<RecentGame> next;
        std::unordered_set<int> ids;
        for (const auto& item : data.at("games")) {
            auto game = parse(item, true);
            if (!game || !ids.insert(game->id).second) throw std::runtime_error("Invalid recent games entry");
            next.push_back(std::move(*game));
        }
        items_ = std::move(next); error_.clear(); return true;
    } catch (...) { error_ = "Recent games unavailable; using this run's history"; return false; }
}
bool RecentGames::remember(const RecentGame& game) { return update(game) && save(); }
bool RecentGames::update(const RecentGame& game) {
    if (!validGame(game)) { error_ = "Invalid recent game metadata"; return false; }
    auto next = items_;
    next.erase(std::remove_if(next.begin(), next.end(), [&](const RecentGame& item) { return item.id == game.id; }), next.end());
    next.insert(next.begin(), game);
    if (next.size() > Limit) next.resize(Limit);
    items_ = std::move(next);
    error_.clear();
    return true;
}
bool RecentGames::save() {
    try {
        Json games = Json::array();
        for (const auto& item : items_) {
            Json entry{{"id",item.id},{"name",item.name},{"artwork",item.artwork}};
            if (!item.alternateArtwork.empty()) entry["alternateArtwork"] = item.alternateArtwork;
            games.push_back(std::move(entry));
        }
        const auto bytes = Json{{"version",1},{"games",std::move(games)}}.dump() + "\n";
        if (bytes.size() > MaxBytes) throw std::runtime_error("Recent games limit exceeded");
        replaceStorage(path_, bytes); error_.clear(); return true;
    } catch (...) { error_ = "Recent games not saved; available for this run only"; return false; }
}
}
