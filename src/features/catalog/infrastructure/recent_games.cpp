#include "core/constants/error_text.hpp"
#include "features/catalog/infrastructure/recent_games.hpp"
#include "features/catalog/infrastructure/artwork.hpp"
#include "core/storage/storage.hpp"
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <set>
#include <charconv>
#include <nlohmann/json.hpp>
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
    const bool validId=!game.id.empty() && game.id.value.size()<=256 &&
        std::none_of(game.id.value.begin(),game.id.value.end(),[](unsigned char c){ return c<=32 || c==127; });
    int numeric=0;
    const auto parsed=std::from_chars(game.id.value.data(),game.id.value.data()+game.id.value.size(),numeric);
    const bool providerValid=game.id.provider!=ProviderId::Mts ||
        (parsed.ec==std::errc{} && parsed.ptr==game.id.value.data()+game.id.value.size() && numeric>0);
    return validId && providerValid && validName(game.name) && (game.artwork.empty() || validArtworkUrl(game.artwork)) &&
        game.alternateArtwork.size() < maxArtworkCandidates &&
        (game.alternateArtwork.empty() || !game.artwork.empty()) &&
        std::all_of(game.alternateArtwork.begin(), game.alternateArtwork.end(), validArtworkUrl);
}
std::optional<RecentGame> parse(const Json& item, int version) {
    if (!item.is_object() || !item.contains("id") || !item.contains("name") || !item.at("name").is_string()) return {};
    EntityId id;
    if (version==1) {
        if (!item.at("id").is_number_integer() || item.at("id")<=0 || item.at("id")>2147483647) return {};
        id={ProviderId::Mts,std::to_string(item.at("id").get<int>())};
    } else {
        if (!item.at("id").is_string() || !item.contains("provider") || !item.at("provider").is_string()) return {};
        const auto provider=item.at("provider").get<std::string>();
        if (provider!="mts" && provider!="foghub") return {};
        id={provider=="mts" ? ProviderId::Mts : ProviderId::FogHub,item.at("id").get<std::string>()};
    }
    RecentGame game{id,item.at("name").get<std::string>(),{}, {}};
    const auto fields=version==1 ? 3u : 4u;
    if ((item.size()!=fields && item.size()!=fields+1) || !item.contains("artwork") || !item.at("artwork").is_string()) return {};
    if (item.size()==fields+1) {
        const auto alternates=item.find("alternateArtwork");
        if (alternates==item.end() || !alternates->is_array() || alternates->size()>=maxArtworkCandidates) return {};
        for (const auto& url:*alternates) {
            if (!url.is_string()) return {};
            game.alternateArtwork.push_back(url.get<std::string>());
        }
    }
    game.artwork=item.at("artwork").get<std::string>();
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
        throw std::runtime_error(fog::constants::error::Catalog::RecentGamesReadFailed);
    }
    struct stat info{};
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 1 ||
        static_cast<std::uint64_t>(info.st_size) > RecentGames::MaxBytes) {
        close(fd); throw std::runtime_error(fog::constants::error::Catalog::InvalidRecentGamesFile);
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
    if (failed) throw std::runtime_error(fog::constants::error::Catalog::RecentGamesReadFailed);
    return bytes;
}
}
std::optional<RecentGame> RecentGame::fromCatalog(const Game& game) {
    RecentGame result{game.id,game.name,{}, {}};
    for (const auto& url:game.artworkUrls) {
        if (result.alternateArtwork.size()+!result.artwork.empty()>=maxArtworkCandidates) break;
        if (!validArtworkUrl(url)) continue;
        if (result.artwork.empty()) result.artwork=url;
        else if (url!=result.artwork && std::find(result.alternateArtwork.begin(),result.alternateArtwork.end(),url)==result.alternateArtwork.end()) result.alternateArtwork.push_back(url);
    }
    return validGame(result) ? std::optional<RecentGame>(std::move(result)) : std::nullopt;
}
std::vector<std::string> RecentGame::artworkCandidates() const {
    auto result = alternateArtwork;
    if (!artwork.empty()) result.insert(result.begin(), artwork);
    return result;
}
Game RecentGame::catalogItem() const { return {id,name,{},{},artworkCandidates()}; }
bool RecentGames::load() {
    try {
        recoverStorage(path_);
        bool missing = false;
        const auto bytes = readFile(path_, missing);
        if (missing) { items_.clear(); error_.clear(); return true; }
        const auto data = Json::parse(bytes, nullptr, false);
        if (!data.is_object() || data.size() != 2 || !data.contains("version") ||
            !data.at("version").is_number_integer() || (data.at("version") != 1 && data.at("version") != 2) ||
            !data.contains("games") || !data.at("games").is_array() || data.at("games").size() > Limit)
            throw std::runtime_error(fog::constants::error::Catalog::InvalidRecentGamesFile);
        std::vector<RecentGame> next;
        std::set<EntityId> ids;
        for (const auto& item : data.at("games")) {
            auto game = parse(item,data.at("version").get<int>());
            if (!game || !ids.insert(game->id).second) throw std::runtime_error(fog::constants::error::Catalog::InvalidRecentGamesEntry);
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
        const bool providerIds=std::any_of(items_.begin(),items_.end(),[](const auto& game){return game.id.provider!=ProviderId::Mts;});
        Json games = Json::array();
        for (const auto& item : items_) {
            int numeric=0; const auto parsed=std::from_chars(item.id.value.data(),item.id.value.data()+item.id.value.size(),numeric);
            if (item.id.provider==ProviderId::Mts && (parsed.ec!=std::errc{} || parsed.ptr!=item.id.value.data()+item.id.value.size() || numeric<=0))
                throw std::runtime_error("Invalid recent-game persistent identity");
            Json entry{{"name",item.name},{"artwork",item.artwork}};
            if (providerIds) {
                entry["id"]=item.id.value;
                entry["provider"]=item.id.provider==ProviderId::Mts ? "mts" : "foghub";
            } else entry["id"]=numeric;
            if (!item.alternateArtwork.empty()) entry["alternateArtwork"] = item.alternateArtwork;
            games.push_back(std::move(entry));
        }
        const auto bytes = Json{{"version",providerIds ? 2 : 1},{"games",std::move(games)}}.dump() + "\n";
        if (bytes.size() > MaxBytes) throw std::runtime_error(fog::constants::error::Catalog::RecentGamesLimitExceeded);
        replaceStorage(path_, bytes); error_.clear(); return true;
    } catch (...) { error_ = "Recent games not saved; available for this run only"; return false; }
}
}
