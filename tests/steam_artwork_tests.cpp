#include "features/catalog/infrastructure/steam_artwork.hpp"
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

namespace {
using namespace std::chrono_literals;
using Json = nlohmann::json;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void idle(fog::SteamArtworkResolver& resolver) {
    const auto limit = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < limit) {
        const auto stats = resolver.stats();
        if (stats.ready && stats.pending == 0) return;
        std::this_thread::sleep_for(1ms);
    }
    throw std::runtime_error("Resolver did not become idle");
}
void entered(const std::atomic_int& calls) {
    const auto limit = std::chrono::steady_clock::now() + 3s;
    while (!calls.load() && std::chrono::steady_clock::now() < limit) std::this_thread::sleep_for(1ms);
    require(calls.load() != 0, "Downloader not entered");
}
std::string url(int id) {
    return "https://shared.cloudflare.steamstatic.com/store_item_assets/steam/apps/" +
        std::to_string(id) + "/header.jpg";
}
std::string response(const std::string& name, int id = 100) {
    return Json{{"items", Json::array({{{"type", "app"}, {"id", id}, {"name", name}}})}}.dump();
}
fog::SteamArtworkOptions config(const std::string& directory, std::atomic<std::int64_t>& time) {
    fog::SteamArtworkOptions result;
    result.directory = directory;
    result.clock = [&time] { return time.load(); };
    return result;
}
void checkMatch(const std::string& title, const std::string& body, int expected) {
    fog::SteamArtworkResolver resolver([&body](const auto&, const auto&) {
        return fog::SteamSearchResponse{200, body};
    });
    require(resolver.request(1, title), "Match request rejected");
    idle(resolver);
    require(resolver.get(1, title) == (expected ? url(expected) : std::string{}), "Incorrect exact match");
}
void matching() {
    checkMatch("God of War", response("God of War", 1593500), 1593500);
    checkMatch("Baldur's Gate 3", response("Baldurs Gate 3", 1086940), 1086940);
    checkMatch("Game & Name™®", response("GAME and NAME", 101), 101);
    checkMatch("Игра один", response("Игра один", 102), 102);
    checkMatch("Игра один", response("Игра два", 102), 0);
    checkMatch("Игра один", response("Another game", 102), 0);
    checkMatch("Game Name", response("Game Name 2"), 0);
    checkMatch("Game Name 2", response("Game Name"), 0);
    checkMatch("Game Name", response("Game Name DLC"), 0);
    checkMatch("Game Name Deluxe Edition", response("Game Name"), 0);
    checkMatch("Game Name Remastered", response("Game Name"), 0);
    checkMatch("Game Name", R"({"items":[{"type":"app","id":123,"name":"Game Name"},{"type":"app","id":456,"name":"Game Name"}]})", 0);
    checkMatch("Game Name", R"({"items":[{"type":"app","id":123,"name":"Game Name"},{"type":"app","id":123,"name":"Game Name"}]})", 123);
    checkMatch("Game Name", R"({"items":[{"type":"app","id":1,"name":"Game Name 2"},{"type":"app","id":9,"name":"Game Name"}]})", 9);
    checkMatch("Game Name", R"({"items":[{"type":"sub","id":900,"name":"Game Name"}]})", 0);
    checkMatch("Game Name", R"({"items":[{"type":"bundle","id":901,"name":"Game Name"}]})", 0);
    checkMatch("Game Name", R"({"items":[{"id":902,"name":"Game Name"}]})", 0);
    checkMatch("Game Name", R"({"items":[{"type":"sub","id":900,"name":"Game Name"},{"type":"bundle","id":901,"name":"Game Name"},{"id":902,"name":"Game Name"},{"type":"app","id":903,"name":"Game Name"}]})", 903);
    std::atomic_int calls{0};
    fog::SteamArtworkResolver resolver([&calls](const auto&, const auto&) {
        ++calls; return fog::SteamSearchResponse{200, "{\"items\":[]}"};
    });
    require(!resolver.request(1, "™®!?"), "Empty normalized title accepted");
    require(!resolver.request(1, "AB"), "Very short title accepted");
    require(!resolver.request(1, "12345"), "Numeric title accepted");
    require(!resolver.request(0, "Game Name"), "Invalid MTS id accepted");
    require(!resolver.request(1, std::string(257, 'a')), "Oversize title accepted");
    require(!resolver.request(1, std::string("Bad\xff", 4)), "Malformed UTF8 accepted");
    require(!resolver.request(1, "Bad\nName"), "Control character accepted");
    require(resolver.request(175, "Cyberpunk 2077 + DLC"), "Explicit alias rejected");
    idle(resolver);
    require(resolver.get(175, "Cyberpunk 2077 + DLC") == url(1091500), "Explicit alias wrong");
    require(resolver.get(175, "Different Game").empty(), "Renamed MTS id reused old title");
    require(calls.load() == 0, "Alias unexpectedly used network");
    resolver.stop(); resolver.stop();
    require(!resolver.request(1, "Game Name"), "Stopped resolver accepted work");
}
void persistence(const std::string& directory) {
    std::atomic<std::int64_t> time{2000000000};
    std::atomic_int calls{0};
    const auto options = config(directory, time);
    {
        fog::SteamArtworkResolver resolver([&calls](const auto& title, const auto&) {
            ++calls;
            return fog::SteamSearchResponse{200, title == "Missing Game" ? "{\"items\":[]}" : response(title, 123)};
        }, options);
        require(resolver.request(1, "Game Name"), "Positive request rejected");
        require(resolver.request(2, "Missing Game"), "Negative request rejected");
        idle(resolver); resolver.stop();
    }
    require(calls.load() == 2, "Unexpected initial request count");
    require(std::filesystem::file_size(directory + "/mappings.json") <= 80 * 1024, "Mapping file unbounded");
    {
        fog::SteamArtworkResolver resolver([&calls](const auto&, const auto&) {
            ++calls; return fog::SteamSearchResponse{};
        }, options);
        // Requests may arrive before disk load, but fresh records must suppress network.
        resolver.request(1, "Game Name"); resolver.request(2, "Missing Game");
        idle(resolver);
        require(calls.load() == 2, "Fresh disk mappings ignored");
        require(resolver.get(1, "Game Name") == url(123), "Offline positive unavailable");
        require(resolver.get(2, "Missing Game").empty(), "Negative mapping became positive");
        time += 24 * 60 * 60;
        resolver.request(2, "Missing Game"); idle(resolver);
        require(calls.load() == 3, "Negative TTL did not expire at 24h");
        resolver.request(2, "Missing Game"); idle(resolver);
        require(calls.load() == 3, "Transport cooldown ignored");
        time += 60;
        resolver.request(2, "Missing Game"); idle(resolver);
        require(calls.load() == 4, "Transport cooldown did not expire");
        resolver.request(1, "Game Name"); idle(resolver);
        require(calls.load() == 4, "Positive TTL too short");
        time = 2000000000 + 30 * 24 * 60 * 60;
        resolver.request(1, "Game Name"); idle(resolver);
        require(calls.load() == 5, "Positive TTL did not expire at 30 days");
        require(resolver.get(1, "Game Name") == url(123), "Offline refresh discarded stale positive");
        resolver.stop();
    }
    {
        fog::SteamArtworkResolver resolver([&calls](const auto&, const auto&) {
            ++calls; return fog::SteamSearchResponse{200, response("Game Name", 456)};
        }, options);
        resolver.request(1, "Game Name"); idle(resolver);
        require(calls.load() == 6 && resolver.get(1, "Game Name") == url(456), "Transport error poisoned persisted mapping");
        resolver.stop();
    }
    {
        fog::SteamArtworkResolver resolver([&calls](const auto&, const auto&) {
            ++calls; return fog::SteamSearchResponse{200, "{\"items\":[]}"};
        }, options);
        resolver.request(1, "Renamed Game"); idle(resolver);
        require(resolver.get(1, "Renamed Game").empty(), "Rename reused appid");
        require(calls.load() == 7, "Rename failed to perform new search");
    }
}
void clockRollback(const std::string& directory) {
    std::filesystem::create_directories(directory);
    std::atomic<std::int64_t> time{2000000000};
    const Json data = {{"version", 2}, {"records", Json::array({
        {{"mtsId",1},{"title","Fresh Game"},{"appId",111},{"saved",1999999999}},
        {{"mtsId",2},{"title","Future Game"},{"appId",222},{"saved",2000003600}},
        {{"mtsId",3},{"title","Future Missing"},{"appId",0},{"saved",2000003600}}
    })}};
    const auto path = directory + "/mappings.json";
    { std::ofstream file(path); file << data.dump(); }
    std::atomic_int calls{0};
    fog::SteamArtworkResolver resolver([&calls](const auto&, const auto&) {
        ++calls; return fog::SteamSearchResponse{};
    }, config(directory, time));
    idle(resolver);
    require(resolver.stats().records == 3 && std::filesystem::exists(path),
        "Future timestamp discarded valid mapping file");
    require(resolver.get(1, "Fresh Game") == url(111) && resolver.get(2, "Future Game") == url(222),
        "Clock rollback discarded offline positive mappings");
    resolver.request(1, "Fresh Game"); idle(resolver);
    require(calls.load() == 0, "Unrelated fresh mapping was refetched after clock rollback");
    resolver.request(2, "Future Game"); resolver.request(3, "Future Missing"); idle(resolver);
    require(calls.load() == 2, "Future positive/negative mappings were incorrectly fresh");
    require(resolver.get(2, "Future Game") == url(222), "Offline refresh discarded future positive mapping");
    resolver.stop();
    std::ifstream file(path); Json retained; file >> retained;
    require(retained == data, "Transient refresh rewrote or removed valid mappings");
    fog::SteamArtworkResolver restarted([](const auto&, const auto&) {
        return fog::SteamSearchResponse{};
    }, config(directory, time));
    idle(restarted);
    require(restarted.stats().records == 3 && restarted.get(1, "Fresh Game") == url(111) &&
        restarted.get(2, "Future Game") == url(222), "Future mappings unavailable after offline restart");
}
void corruptAndDiskFailure(const std::string& directory) {
    const auto invalid = std::vector<std::string>{
        "not json", std::string(90 * 1024, 'x'),
        R"({"version":1,"records":[{"mtsId":1,"title":"Game Name","appId":900,"saved":2000000000}]})",
        R"({"version":2,"records":[{"mtsId":1,"title":"Game Name","appId":"https://evil/","saved":1}]})",
        R"({"version":2,"records":[{"mtsId":1,"title":"Game Name","appId":18446744073709551615,"saved":1}]})"};
    std::atomic<std::int64_t> time{2000000000};
    for (const auto& bytes : invalid) {
        std::filesystem::create_directories(directory);
        { std::ofstream file(directory + "/mappings.json"); file << bytes; }
        fog::SteamArtworkResolver resolver([](const auto& title, const auto&) {
            return fog::SteamSearchResponse{200, response(title, 222)};
        }, config(directory, time));
        resolver.request(1, "Game Name"); idle(resolver);
        require(resolver.get(1, "Game Name") == url(222), "Corrupt disk blocked network");
        resolver.stop();
    }
    const auto filePath = directory + "/not-directory";
    { std::ofstream file(filePath); file << "x"; }
    fog::SteamArtworkResolver resolver([](const auto& title, const auto&) {
        return fog::SteamSearchResponse{200, response(title, 333)};
    }, config(filePath, time));
    resolver.request(1, "Game Name"); idle(resolver);
    require(resolver.get(1, "Game Name") == url(333), "Disk write failure blocked lookup");
}
void fullBounds(const std::string& directory) {
    std::atomic<std::int64_t> time{2000000000};
    auto options = config(directory, time);
    options.maxRecords = 1000; // Configuration cannot relax the hard bound.
    options.maxPending = 1000;
    fog::SteamArtworkResolver resolver([](const auto& title, const auto&) {
        return fog::SteamSearchResponse{200, response(title, 777)};
    }, options);
    for (int i = 1; i <= 280; ++i) {
        // Escaped backslashes exercise the byte cap before the record cap.
        const auto title = "Game " + std::to_string(i) + std::string(235, '\\');
        require(resolver.request(i, title), "Sequential bounded work rejected");
        idle(resolver);
        require(resolver.stats().records <= 256, "Hard memory cap ignored");
    }
    resolver.stop();
    std::uintmax_t total = 0;
    for (const auto& file : std::filesystem::directory_iterator(directory)) {
        total += file.file_size();
    }
    require(total <= 256 * 1024, "Disk byte budget exceeded");
    std::ifstream file(directory + "/mappings.json"); Json data; file >> data;
    require(data.at("records").size() <= 256, "Hard disk cap ignored");
    require(std::filesystem::file_size(directory + "/mappings.json") <= 80 * 1024, "Large escaped titles exceeded file cap");
}
void directorySymlink(const std::string& directory) {
    std::filesystem::create_directories(directory + "/real");
    const auto protectedPath = directory + "/real/mappings.json";
    { std::ofstream file(protectedPath); file << "untouched"; }
    std::filesystem::create_directory_symlink(directory + "/real", directory + "/link");
    std::atomic<std::int64_t> time{2000000000};
    fog::SteamArtworkResolver resolver([](const auto& title, const auto&) {
        return fog::SteamSearchResponse{200, response(title, 888)};
    }, config(directory + "/link", time));
    resolver.request(1, "Game Name"); idle(resolver); resolver.stop();
    require(resolver.get(1, "Game Name").empty(), "Stopped resolver leaked result");
    std::ifstream file(protectedPath); std::string bytes; file >> bytes;
    require(bytes == "untouched", "Unsafe directory cleanup followed symlink");
}
void malformedRetry() {
    std::atomic<std::int64_t> time{2000000000};
    const auto bad = std::vector<fog::SteamSearchResponse>{
        {429, "{\"items\":[]}"}, {302, "{\"items\":[]}"}, {200, "{"}, {200, "{}"},
        {200, R"({"items":[{"type":"app","name":"Game Name","id":0}]})"},
        {200, R"({"items":[{"type":"app","name":"Game Name","id":"100"}]})"},
        {200, R"({"items":[{"type":"app","name":"Game Name","id":100.5}]})"},
        {200, R"({"items":[{"type":"app","name":"Game Name","id":18446744073709551615}]})"},
        {200, R"({"items":[{"type":7,"name":"Game Name","id":100}]})"},
        {200, std::string(256 * 1024 + 1, 'x')},
        {200, std::string(100, '[') + "0" + std::string(100, ']')}};
    for (const auto& failure : bad) {
        std::atomic_int calls{0};
        fog::SteamArtworkResolver resolver([&](const auto&, const auto&) {
            return ++calls == 1 ? failure : fog::SteamSearchResponse{200, response("Game Name", 444)};
        }, config({}, time));
        resolver.request(1, "Game Name"); idle(resolver);
        require(resolver.get(1, "Game Name").empty(), "Malformed response produced mapping");
        resolver.request(1, "Game Name"); idle(resolver);
        require(calls.load() == 1, "Malformed response cooldown ignored");
        time += 60;
        resolver.request(1, "Game Name"); idle(resolver);
        require(calls.load() == 2 && resolver.get(1, "Game Name") == url(444), "Malformed response cached as permanent no-match");
    }
}
void boundsAndCancellation(const std::string& directory) {
    std::atomic_int calls{0};
    fog::SteamArtworkOptions options;
    options.maxRecords = 4; options.maxPending = 2;
    fog::SteamArtworkResolver resolver([&calls](const auto&, const auto& cancelled) {
        ++calls;
        while (!cancelled.load()) std::this_thread::sleep_for(1ms);
        return fog::SteamSearchResponse{};
    }, options);
    const auto begin = std::chrono::steady_clock::now();
    require(resolver.request(1, "First Game"), "First work rejected");
    entered(calls);
    require(resolver.request(2, "Second Game"), "Queue work rejected");
    require(resolver.request(1, "First Game"), "Duplicate pending rejected");
    require(!resolver.request(3, "Third Game"), "Queue bound ignored");
    require(resolver.stats().pending == 2, "Duplicate request queued twice");
    require(std::chrono::steady_clock::now() - begin < 1s, "Request blocked on network");
    resolver.stop();
    require(calls.load() == 1, "Shutdown drained cancelled queue");
    require(std::chrono::steady_clock::now() - begin < 1s, "Shutdown did not cancel");
    options.maxPending = 1; options.directory = directory;
    fog::SteamArtworkResolver bounded([](const auto& title, const auto&) {
        return fog::SteamSearchResponse{200, response(title, 555)};
    }, options);
    for (int i = 1; i <= 20; ++i) {
        bounded.request(i, "Game " + std::to_string(i)); idle(bounded);
        require(bounded.stats().records <= 4, "Memory cache bound ignored");
    }
    require(bounded.get(1, "Game 1").empty(), "Memory LRU did not evict old record");
    bounded.stop();
    std::ifstream file(directory + "/mappings.json"); Json disk; file >> disk;
    require(disk.at("records").size() <= 4, "Disk record bound ignored");
}
}
int main() {
    char temporary[] = "/tmp/fogplay-steam-tests-XXXXXX";
    const auto* created = mkdtemp(temporary);
    if (!created) return 2;
    const std::string root = created;
    try {
        matching(); std::cout << "matching/aliases/Unicode: passed\n";
        persistence(root + "/persistent"); std::cout << "disk/TTLs/offline/retry/rename: passed\n";
        clockRollback(root + "/clock-rollback"); std::cout << "clock rollback preserves mixed valid cache: passed\n";
        corruptAndDiskFailure(root + "/corrupt"); std::cout << "corrupt/oversize/disk-failure: passed\n";
        malformedRetry(); std::cout << "malformed/HTTP-error cooldown: passed\n";
        boundsAndCancellation(root + "/bounds"); std::cout << "queue/memory/disk bounds/cancellation: passed\n";
        fullBounds(root + "/full-bounds"); std::cout << "hard 256-record/256-KiB budgets: passed\n";
        directorySymlink(root + "/symlink"); std::cout << "directory symlink isolation: passed\n";
        std::filesystem::remove_all(root);
        std::cout << "Steam artwork tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << " (fixtures " << root << ")\n";
        return 1;
    }
}