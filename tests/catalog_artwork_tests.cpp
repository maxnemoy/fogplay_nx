#include "features/catalog/presentation/catalog_artwork.hpp"
#include "features/catalog/infrastructure/artwork.hpp"
#include "features/catalog/infrastructure/steam_artwork.hpp"
#include "features/catalog/infrastructure/recent_games.hpp"
#include <SDL.h>
#include <SDL_image.h>
#include <nlohmann/json.hpp>
#include "features/catalog/infrastructure/mts_catalog_mapper.hpp"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
using Json = nlohmann::json;
using namespace std::chrono_literals;
const std::string mtsPrefix = "https://fogplay.mts.ru/image/preview/";
const std::string steamUrl = "https://shared.cloudflare.steamstatic.com/store_item_assets/steam/apps/123/header.jpg";
const std::string title = "Integration Game";
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
struct TemporaryDirectory {
    std::string path;
    struct stat original{};
    TemporaryDirectory() {
        char pattern[] = "/tmp/fogplay-catalog-artwork-XXXXXX";
        const auto* created = mkdtemp(pattern);
        if (!created) throw std::runtime_error("Cannot create fixture directory");
        path = created;
        require(lstat(path.c_str(), &original) == 0 && S_ISDIR(original.st_mode), "Cannot inspect fixture directory");
    }
    ~TemporaryDirectory() {
        // Remove only this exact mkdtemp directory, after verifying its original
        // identity. Never accept a replaced directory, symlink, or user path.
        struct stat current{};
        if (path.rfind("/tmp/fogplay-catalog-artwork-", 0) != 0 ||
            lstat(path.c_str(), &current) != 0 || !S_ISDIR(current.st_mode) ||
            current.st_ino != original.st_ino || current.st_dev != original.st_dev) return;
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    fog::ArtworkDiskOptions artwork() const { return {path + "/artwork"}; }
    fog::SteamArtworkOptions steam() const {
        fog::SteamArtworkOptions result;
        result.directory = path + "/steam";
        result.clock = [] { return std::int64_t{2000000000}; };
        return result;
    }
};
std::vector<std::uint8_t> jpeg() {
    auto* surface = SDL_CreateRGBSurfaceWithFormat(0, 3, 2, 32, SDL_PIXELFORMAT_RGBA32);
    require(surface != nullptr, "Cannot create JPEG source");
    SDL_FillRect(surface, nullptr, SDL_MapRGBA(surface->format, 32, 128, 200, 255));
    std::vector<std::uint8_t> bytes(65536);
    auto* output = SDL_RWFromMem(bytes.data(), static_cast<int>(bytes.size()));
    require(output != nullptr, "Cannot create JPEG output");
    const auto result = IMG_SaveJPG_RW(surface, output, 0, 95);
    const auto size = SDL_RWtell(output);
    SDL_RWclose(output); SDL_FreeSurface(surface);
    require(result == 0 && size > 0, "Cannot encode JPEG fixture");
    bytes.resize(static_cast<std::size_t>(size));
    return bytes;
}
Json game() { return {{"id", 91001}, {"name", title}}; }
fog::SteamSearchResponse match(const std::string& name) {
    return {200, Json{{"items", Json::array({{{"id", 123}, {"name", name}, {"type", "app"}}})}}.dump()};
}
SDL_Texture* awaitCover(const fog::Game& item, fog::ArtworkCache& artwork,
                        fog::SteamArtworkResolver& steam, SDL_Renderer* renderer) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline) {
        artwork.update(renderer);
        if (auto* cover = fog::catalogArtwork(item, artwork, steam)) return cover;
        std::this_thread::sleep_for(1ms);
    }
    throw std::runtime_error("Catalog cover never became available");
}
SDL_Texture* awaitCover(const Json& item, fog::ArtworkCache& artwork,
                        fog::SteamArtworkResolver& steam, SDL_Renderer* renderer) {
    return awaitCover(fog::mapMtsGame(item),artwork,steam,renderer);
}
void idle(fog::SteamArtworkResolver& steam, const std::atomic_int& searches) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto state = steam.stats();
        if (searches.load() > 0 && state.ready && state.pending == 0) return;
        std::this_thread::sleep_for(1ms);
    }
    throw std::runtime_error("Steam search never completed");
}
void mtsSuccess(SDL_Renderer* renderer) {
    std::atomic_int searches{0}, downloads{0};
    const auto bytes = jpeg();
    fog::SteamArtworkResolver steam([&](const auto& name, const auto&) {
        ++searches; return match(name);
    });
    const auto mts = mtsPrefix + "successful.jpg";
    fog::ArtworkCache artwork([&](const auto& url, const auto&) {
        require(url == mts, "Successful MTS route requested another host");
        ++downloads; return bytes;
    });
    auto item = game(); item["images"] = Json::array({mts});
    require(awaitCover(item, artwork, steam, renderer) == artwork.get(mts), "Wrong MTS texture returned");
    for (int i = 0; i < 30; ++i) require(fog::catalogArtwork(fog::mapMtsGame(item), artwork, steam) != nullptr, "MTS cover disappeared");
    const auto state = steam.stats();
    require(searches == 0 && state.pending == 0 && state.records == 0 && downloads == 1,
        "MTS success triggered Steam or duplicate image work");
}
void missingImagesPersist(SDL_Renderer* renderer) {
    TemporaryDirectory directory;
    std::atomic_int searches{0}, downloads{0};
    const auto bytes = jpeg();
    {
        fog::SteamArtworkResolver steam([&](const auto& name, const auto&) {
            ++searches; return match(name);
        }, directory.steam());
        fog::ArtworkCache artwork([&](const auto& url, const auto&) {
            require(url == steamUrl, "Missing images requested an unexpected image");
            ++downloads; return bytes;
        }, directory.artwork());
        require(awaitCover(game(), artwork, steam, renderer) == artwork.get(steamUrl), "Missing images did not use Steam");
        require(searches == 1 && downloads == 1, "Missing images work count incorrect");
    }
    require(std::filesystem::is_regular_file(directory.path + "/steam/mappings.json"), "Steam mapping was not persisted");
    require(!std::filesystem::is_empty(directory.path + "/artwork"), "Steam cover was not persisted");
    std::atomic_int offlineSearches{0}, offlineDownloads{0};
    fog::SteamArtworkResolver steam([&](const auto&, const auto&) -> fog::SteamSearchResponse {
        ++offlineSearches; throw std::runtime_error("Offline search unexpectedly attempted");
    }, directory.steam());
    fog::ArtworkCache artwork([&](const auto&, const auto&) -> std::vector<std::uint8_t> {
        ++offlineDownloads; throw std::runtime_error("Offline image download unexpectedly attempted");
    }, directory.artwork());
    require(awaitCover(game(), artwork, steam, renderer) == artwork.get(steamUrl), "Offline persisted Steam cover unavailable");
    require(offlineSearches == 0 && offlineDownloads == 0, "Recreated components attempted network");
}
void fourMtsFailures(SDL_Renderer* renderer) {
    std::atomic_int searches{0}, mtsDownloads{0}, steamDownloads{0};
    const auto bytes = jpeg();
    const std::vector<std::string> mts{mtsPrefix + "first.jpg", mtsPrefix + "second.jpg",
        mtsPrefix + "third.jpg", mtsPrefix + "fourth.jpg"};
    fog::SteamArtworkResolver steam([&](const auto& name, const auto&) {
        require(mtsDownloads == 4, "Steam started before all four MTS candidates failed");
        ++searches; return match(name);
    });
    fog::ArtworkCache artwork([&](const std::string& url, const auto&) {
        if (url == steamUrl) { ++steamDownloads; return bytes; }
        const auto index = mtsDownloads.fetch_add(1);
        require(index >= 0 && index < 4 && url == mts[static_cast<std::size_t>(index)], "MTS order or cap changed");
        if (index == 1) return std::vector<std::uint8_t>{1, 2, 3};
        throw std::runtime_error("Simulated missing MTS artwork");
    });
    auto item = game(); item["images"] = mts;
    require(awaitCover(item, artwork, steam, renderer) == artwork.get(steamUrl), "Four failures did not route to Steam");
    require(artwork.exhausted(mts) && searches == 1 && mtsDownloads == 4 && steamDownloads == 1,
        "Steam fallback was lost to the four-candidate image cap");
}
void invalidImages(SDL_Renderer* renderer) {
    const auto bytes = jpeg();
    const std::vector<Json> invalid{nullptr, "not an array", Json::object(), 7,
        Json::array({nullptr, 7, "https://evil.test/image.jpg", mtsPrefix + "../secret.jpg"}), Json::array()};
    for (const auto& images : invalid) {
        std::atomic_int searches{0}, downloads{0};
        fog::SteamArtworkResolver steam([&](const auto& name, const auto&) {
            ++searches; return match(name);
        });
        fog::ArtworkCache artwork([&](const auto& url, const auto&) {
            require(url == steamUrl, "Invalid image metadata reached image downloader");
            ++downloads; return bytes;
        });
        auto item = game(); item["images"] = images;
        require(awaitCover(item, artwork, steam, renderer) != nullptr, "Invalid image metadata prevented fallback");
        require(searches == 1 && downloads == 1, "Invalid image metadata produced extra work");
    }
}
void noExactMatch(SDL_Renderer* renderer) {
    std::atomic_int searches{0}, downloads{0};
    fog::SteamArtworkResolver steam([&](const auto& name, const auto&) {
        ++searches; return match(name + " 2");
    });
    fog::ArtworkCache artwork([&](const auto&, const auto&) -> std::vector<std::uint8_t> {
        ++downloads; throw std::runtime_error("Nonexact Steam result must not download");
    });
    require(fog::catalogArtwork(fog::mapMtsGame(game()), artwork, steam) == nullptr, "Missing cover did not start with placeholder");
    idle(steam, searches);
    for (int i = 0; i < 30; ++i) {
        artwork.update(renderer);
        require(fog::catalogArtwork(fog::mapMtsGame(game()), artwork, steam) == nullptr, "Nonexact match produced a cover");
    }
    require(searches == 1 && downloads == 0, "Nonexact result retried or downloaded an unrelated cover");
}
void recentWithoutArtwork(SDL_Renderer* renderer) {
    const auto recent = fog::RecentGame::fromCatalog(fog::mapMtsGame(game()));
    require(recent.has_value() && recent->artwork.empty(), "Missing-artwork recent game was rejected");
    const auto item = recent->catalogItem();
    require(item.artworkUrls.empty(), "Recent catalog conversion invented artwork");
    std::atomic_int searches{0}, downloads{0};
    const auto bytes = jpeg();
    fog::SteamArtworkResolver steam([&](const auto& name, const auto&) { ++searches; return match(name); });
    fog::ArtworkCache artwork([&](const auto& url, const auto&) {
        require(url == steamUrl, "Recent fallback URL incorrect"); ++downloads; return bytes;
    });
    require(awaitCover(item, artwork, steam, renderer) != nullptr, "Recent game did not use shared fallback router");
    require(searches == 1 && downloads == 1, "Recent game fallback work count incorrect");
}
void cachedSteamStillRetriesMtsAndRefreshes(SDL_Renderer* renderer) {
    std::atomic_int searches{0}, mtsDownloads{0}, steamDownloads{0};
    std::atomic_bool releaseMts{false}, releaseRefresh{false};
    std::atomic<std::int64_t> time{2000000000};
    fog::SteamArtworkOptions options;
    options.clock = [&time] { return time.load(); };
    const auto bytes = jpeg();
    const auto mts = mtsPrefix + "recovering.jpg";
    fog::SteamArtworkResolver steam([&](const auto& name, const std::atomic_bool& cancelled) {
        const auto count = ++searches;
        require(count <= 2, "Unexpected duplicate Steam refresh");
        if (count == 2)
            while (!releaseRefresh.load() && !cancelled.load()) std::this_thread::sleep_for(1ms);
        return match(name);
    }, options);
    fog::ArtworkCache artwork([&](const auto& url, const std::atomic_bool& cancelled) {
        if (url == steamUrl) { ++steamDownloads; return bytes; }
        require(url == mts, "Unexpected image during cached-cover recovery");
        ++mtsDownloads;
        while (!releaseMts.load() && !cancelled.load()) std::this_thread::sleep_for(1ms);
        return bytes;
    });
    // Preload a positive mapping and decoded texture through the real router.
    auto* fallback = awaitCover(game(), artwork, steam, renderer);
    require(fallback == artwork.get(steamUrl) && searches == 1 && steamDownloads == 1,
        "Cannot preload cached Steam cover");
    auto withMts = game(); withMts["images"] = Json::array({mts});
    require(fog::catalogArtwork(fog::mapMtsGame(withMts), artwork, steam) == fallback,
        "Cached Steam cover not served while MTS starts");
    auto deadline = std::chrono::steady_clock::now() + 3s;
    while (mtsDownloads == 0 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
    require(mtsDownloads == 1, "Cached Steam texture prevented an MTS request");
    require(fog::catalogArtwork(fog::mapMtsGame(withMts), artwork, steam) == fallback,
        "Steam cover disappeared while MTS download was pending");
    releaseMts = true;
    SDL_Texture* authoritative = nullptr;
    deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline) {
        artwork.update(renderer);
        authoritative = artwork.get(mts);
        if (authoritative && fog::catalogArtwork(fog::mapMtsGame(withMts), artwork, steam) == authoritative) break;
        std::this_thread::sleep_for(1ms);
    }
    require(authoritative && authoritative != fallback &&
        fog::catalogArtwork(fog::mapMtsGame(withMts), artwork, steam) == authoritative,
        "Recovered MTS cover did not take priority over cached Steam");
    require(searches == 1 && mtsDownloads == 1 && steamDownloads == 1, "Recovery repeated fresh mapping/image work");
    // The missing-images view still uses the same positive mapping and cached
    // Steam texture, so a 30-day refresh must start before returning that texture.
    time += 30 * 24 * 60 * 60;
    require(fog::catalogArtwork(fog::mapMtsGame(game()), artwork, steam) == fallback,
        "Expired mapping interrupted its cached Steam cover");
    deadline = std::chrono::steady_clock::now() + 3s;
    while (searches < 2 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
    require(searches == 2 && steam.stats().pending == 1,
        "Cached Steam texture prevented the 30-day mapping refresh");
    require(fog::catalogArtwork(fog::mapMtsGame(game()), artwork, steam) == fallback,
        "Cached Steam cover unavailable during mapping refresh");
    releaseRefresh = true;
    idle(steam, searches);
    require(searches == 2 && steamDownloads == 1 &&
        fog::catalogArtwork(fog::mapMtsGame(game()), artwork, steam) == fallback,
        "Mapping refresh unnecessarily reloaded its cached texture");
}
void pendingAndFullQueue(SDL_Renderer* renderer) {
    std::atomic_int searches{0}, entered{0};
    std::atomic_bool release{false};
    const auto bytes = jpeg();
    fog::SteamArtworkResolver steam([&](const auto& name, const auto&) { ++searches; return match(name); });
    const auto blocker = mtsPrefix + "blocked.jpg", target = mtsPrefix + "queued.jpg";
    fog::ArtworkCache artwork([&](const auto&, const std::atomic_bool& cancelled) {
        ++entered;
        while (!release.load() && !cancelled.load()) std::this_thread::sleep_for(1ms);
        return bytes;
    });
    require(artwork.request(blocker), "Blocker request rejected");
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (entered == 0 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
    require(entered == 1, "Blocked image worker did not start");
    auto queued = game(); queued["images"] = Json::array({target});
    for (int i = 0; i < 30; ++i)
        require(fog::catalogArtwork(fog::mapMtsGame(queued), artwork, steam) == nullptr, "Queued image unexpectedly ready");
    // One running + target + thirty queued requests fills the existing bound.
    for (int i = 0; i < 30; ++i)
        require(artwork.request(mtsPrefix + "fill-" + std::to_string(i) + ".jpg"), "Cannot fill artwork queue");
    const auto overflow = mtsPrefix + "overflow.jpg";
    require(!artwork.request(overflow), "Artwork queue did not saturate");
    auto full = game(); full["id"] = 91002; full["images"] = Json::array({overflow});
    for (int i = 0; i < 30; ++i)
        require(fog::catalogArtwork(fog::mapMtsGame(full), artwork, steam) == nullptr, "Queue-full image unexpectedly ready");
    const auto state = steam.stats();
    require(!artwork.exhausted({target}) && !artwork.exhausted({overflow}) && searches == 0 &&
        state.pending == 0 && state.records == 0,
        "Queued or capacity-rejected MTS image incorrectly triggered Steam");
    release = true;
    require(awaitCover(queued, artwork, steam, renderer) == artwork.get(target), "Queued MTS image did not complete");
    require(searches == 0, "Transient queue pressure eventually triggered Steam");
}
}
int main() {
    SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { std::cerr << SDL_GetError() << '\n'; return 2; }
    auto* surface = SDL_CreateRGBSurfaceWithFormat(0, 640, 480, 32, SDL_PIXELFORMAT_RGBA32);
    auto* renderer = surface ? SDL_CreateSoftwareRenderer(surface) : nullptr;
    int result = 0;
    try {
        require(renderer != nullptr, "Cannot create software renderer");
        mtsSuccess(renderer); std::cout << "MTS success: zero Steam searches passed\n";
        missingImagesPersist(renderer); std::cout << "Missing images: Steam lookup + SD + both-component offline restart passed\n";
        fourMtsFailures(renderer); std::cout << "Four MTS failures: separate Steam fallback passed\n";
        invalidImages(renderer); std::cout << "Invalid/nonarray images: fallback passed\n";
        noExactMatch(renderer); std::cout << "Nonexact Steam result: placeholder passed\n";
        recentWithoutArtwork(renderer); std::cout << "Recent catalogItem missing artwork: fallback passed\n";
        cachedSteamStillRetriesMtsAndRefreshes(renderer); std::cout << "Cached Steam texture: MTS recovery/priority + 30-day mapping refresh passed\n";
        pendingAndFullQueue(renderer); std::cout << "Pending/full MTS queue: zero premature Steam searches passed\n";
        std::cout << "Catalog artwork integration tests passed (injected downloaders; no real network).\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n'; result = 1;
    }
    SDL_DestroyRenderer(renderer); SDL_FreeSurface(surface); SDL_Quit();
    return result;
}
