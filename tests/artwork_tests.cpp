#include "features/catalog/infrastructure/artwork.hpp"
#include <SDL.h>
#include <SDL_image.h>
#include <webp/encode.h>
#include <atomic>
#include <algorithm>
#include <cerrno>
#include <cassert>
#include <chrono>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>
#include <stdexcept>
#include <thread>
#include <vector>

#ifdef ARTWORK_TEST_WRITE_FAULTS
// Exercise direct cache writes with both interrupted syscalls and short writes.
extern "C" ssize_t __real_write(int, const void*, std::size_t);
extern "C" ssize_t __wrap_write(int descriptor, const void* data, std::size_t count) {
    static std::atomic_uint calls{0};
    if (++calls % 7 == 0) { errno = EINTR; return -1; }
    return __real_write(descriptor, data, std::min(count, std::size_t{7}));
}
#endif

// A texture upload failure must enter the same cooldown/fallback path as bad bytes.
std::atomic_bool failNextArtworkUpload{false};
extern "C" SDL_Texture* __real_SDL_CreateTextureFromSurface(SDL_Renderer*, SDL_Surface*);
extern "C" SDL_Texture* __wrap_SDL_CreateTextureFromSurface(SDL_Renderer* renderer, SDL_Surface* surface) {
    if (failNextArtworkUpload.exchange(false)) return nullptr;
    return __real_SDL_CreateTextureFromSurface(renderer, surface);
}

namespace {
const std::string prefix = "https://fogplay.mts.ru/image/preview/";
const std::string steamPrefix = "https://shared.cloudflare.steamstatic.com/store_item_assets/steam/apps/";
const std::string steamHeader = steamPrefix + "570/header.jpg";
std::vector<std::uint8_t> webp(int width, int height) {
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * height * 4, 255);
    std::uint8_t* encoded = nullptr;
    const auto count = WebPEncodeLosslessRGBA(rgba.data(), width, height, width * 4, &encoded);
    if (!encoded || !count) throw std::runtime_error("Test WebP encoding failed");
    std::vector<std::uint8_t> bytes(encoded, encoded + count);
    WebPFree(encoded);
    return bytes;
}
std::vector<std::uint8_t> raster(bool png, int width = 3, int height = 2) {
    auto* surface = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_RGBA32);
    if (!surface) throw std::runtime_error(SDL_GetError());
    SDL_FillRect(surface, nullptr, SDL_MapRGBA(surface->format, 32, 128, 200, 255));
    std::vector<std::uint8_t> bytes(65536);
    auto* output = SDL_RWFromMem(bytes.data(), static_cast<int>(bytes.size()));
    const auto result = png ? IMG_SavePNG_RW(surface, output, 0) : IMG_SaveJPG_RW(surface, output, 0, 95);
    const auto size = SDL_RWtell(output);
    SDL_RWclose(output); SDL_FreeSurface(surface);
    if (result != 0 || size <= 0) throw std::runtime_error("Test raster encoding failed");
    bytes.resize(static_cast<std::size_t>(size));
    return bytes;
}
bool rejected(const std::vector<std::uint8_t>& bytes) {
    try { (void)fog::decodeArtwork(bytes); return false; }
    catch (const std::runtime_error&) { return true; }
}
SDL_Texture* awaitTexture(fog::ArtworkCache& cache, SDL_Renderer* renderer, const std::string& url) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        cache.update(renderer);
        if (auto* texture = cache.get(url)) return texture;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    throw std::runtime_error("Texture was not uploaded");
}
void validation() {
    assert(fog::validArtworkUrl(prefix + "73/8a/cover-425x267.webp"));
    const std::vector<std::string> invalid = {
        "http://fogplay.mts.ru/image/preview/a.webp", "https://fogplay.mts.ru.evil/image/preview/a.webp",
        "https://fogplay.mts.ru@evil/image/preview/a.webp", "https://fogplay.mts.ru:443/image/preview/a.webp",
        "https://fogplay.mts.ru/image/preview/../private/a.webp", prefix + "a.webp?token=secret",
        prefix + "a.webp#x", prefix + "a%2f.webp", prefix + "a.webp\n", prefix + "a.svg",
        prefix + "/a.webp", prefix + std::string(1024, 'a') + ".webp",
        std::string(prefix + "a.webp") + std::string(1, '\0'),
        steamPrefix + "0/header.jpg", steamPrefix + "000/header.jpg", steamPrefix + "0570/header.jpg",
        steamPrefix + "-570/header.jpg", steamPrefix + "+570/header.jpg", steamPrefix + "570.0/header.jpg",
        steamPrefix + "570x/header.jpg", steamPrefix + "/header.jpg", steamPrefix + "570/header.png",
        steamPrefix + "570/../570/header.jpg", steamPrefix + "570%2f/header.jpg",
        steamHeader + "?token=secret", steamHeader + "#x", steamHeader + "/extra",
        steamHeader + "\n", steamHeader + std::string(1, '\0'),
        "http://shared.cloudflare.steamstatic.com/store_item_assets/steam/apps/570/header.jpg",
        "https://shared.cloudflare.steamstatic.com.evil/store_item_assets/steam/apps/570/header.jpg",
        "https://shared.cloudflare.steamstatic.com@evil/store_item_assets/steam/apps/570/header.jpg",
        "https://user@shared.cloudflare.steamstatic.com/store_item_assets/steam/apps/570/header.jpg",
        "https://shared.cloudflare.steamstatic.com:443/store_item_assets/steam/apps/570/header.jpg",
        "https://cdn.cloudflare.steamstatic.com/store_item_assets/steam/apps/570/header.jpg",
        "https://evil.shared.cloudflare.steamstatic.com/store_item_assets/steam/apps/570/header.jpg",
        "https://shared.akamai.steamstatic.com.evil/store_item_assets/steam/apps/570/header.jpg",
        "https://shared.steamstatic.com.evil/store_item_assets/steam/apps/570/header.jpg",
        "https://evil.shared.steamstatic.com/store_item_assets/steam/apps/570/header.jpg",
        "https://user@shared.steamstatic.com/store_item_assets/steam/apps/570/header.jpg",
        "https://shared.steamstatic.com:443/store_item_assets/steam/apps/570/header.jpg",
        "https://shared.steamstatic.com/store_item_assets/steam/apps/570/header.jpg?token=secret",
        "https://shared.steamstatic.com/store_item_assets/steam/apps/570/../header.jpg",
        "https://steamcdn-a.akamaihd.net/store_item_assets/steam/apps/570/header.jpg",
        "https://shared.cloudflare.steamstatic.com/steam/apps/570/header.jpg",
        steamPrefix + std::string(1024, '1') + "/header.jpg"
    };
    for (const auto& url : invalid) assert(!fog::validArtworkUrl(url));
    assert(fog::validArtworkUrl(steamHeader));
    assert(fog::validArtworkUrl(steamPrefix + "1/header.jpg"));
    assert(fog::validArtworkUrl("https://shared.akamai.steamstatic.com/store_item_assets/steam/apps/570/header.jpg"));
    assert(fog::validArtworkUrl("https://shared.steamstatic.com/store_item_assets/steam/apps/570/header.jpg"));
    for (const auto* extension : {"jpg", "jpeg", "png", "webp"})
        assert(fog::validArtworkUrl(prefix + "cover-425x267." + extension));
    for (const auto png : {false, true}) {
        const auto decoded = fog::decodeArtwork(raster(png));
        assert(decoded.width == 3 && decoded.height == 2 && decoded.rgba.size() == 24);
        assert(decoded.rgba[1] > 110 && decoded.rgba[1] < 145 && decoded.rgba[3] == 255);
        assert(rejected(raster(png, 2049, 1)));
        auto truncated = raster(png); truncated.resize(16); assert(rejected(truncated));
    }
    // A hostile PNG IHDR is rejected before a decoder can allocate its declared image.
    auto png = raster(true); png[16] = 0x7f; assert(rejected(png));
    assert(rejected({}));
    assert(rejected({1, 2, 3, 4}));
    assert(rejected(std::vector<std::uint8_t>(fog::maxArtworkBytes + 1, 0)));
    assert(rejected(webp(2049, 1)));
    assert(rejected(webp(1025, 1025)));
    const auto pixels = fog::decodeArtwork(webp(3, 2));
    assert(pixels.width == 3 && pixels.height == 2 && pixels.rgba.size() == 24);
}
void cancellation() {
    std::atomic_bool entered{false};
    fog::ArtworkCache cache([&entered](const std::string&, const std::atomic_bool& cancelled) {
        entered.store(true);
        while (!cancelled.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return std::vector<std::uint8_t>{};
    });
    assert(cache.exhausted({}));
    assert(cache.exhausted({"https://evil.test/a.jpg", ""}));
    assert(!cache.exhausted({prefix + "cancel.webp"}));
    assert(cache.request(prefix + "cancel.webp"));
    assert(!cache.exhausted({prefix + "cancel.webp"}));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!entered.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(entered.load());
    for (int i = 0; i < 31; ++i) assert(cache.request(prefix + std::to_string(i) + ".webp"));
    const std::vector<std::string> untried{prefix + "overflow.webp", steamHeader};
    assert(!cache.request(untried));
    assert(!cache.exhausted(untried)); // A full queue has not failed either image.
    assert(!cache.exhausted({prefix + "cancel.webp", prefix + "0.webp"}));
    const auto start = std::chrono::steady_clock::now();
    cache.stop();
    assert(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(250));
    assert(!cache.request(prefix + "after-stop.webp"));
    cache.stop();
}
void idleWorkerStop() {
    // Exercise stop while the worker is entering its condition-variable wait.
    for (int i = 0; i < 250; ++i) {
        fog::ArtworkCache cache;
        if (i % 2) std::this_thread::yield();
        cache.stop();
    }
}
void cacheAndEviction(SDL_Renderer* renderer) {
    const auto bytes = webp(3, 2);
    std::atomic_int calls{0};
    const auto ui = std::this_thread::get_id();
    fog::ArtworkCache cache([&](const std::string&, const std::atomic_bool&) {
        assert(std::this_thread::get_id() != ui);
        ++calls;
        return bytes;
    });
    const auto first = prefix + "0.webp";
    assert(cache.request(first) && cache.request(first));
    assert(!cache.exhausted({first}));
    auto* texture = awaitTexture(cache, renderer, first);
    int width = 0, height = 0;
    assert(SDL_QueryTexture(texture, nullptr, nullptr, &width, &height) == 0);
    assert(width == 3 && height == 2 && calls == 1);
    assert(!cache.exhausted({first})); // Resident texture is available.
    for (int i = 1; i <= 32; ++i) {
        const auto url = prefix + std::to_string(i) + ".webp";
        assert(cache.request(url));
        (void)awaitTexture(cache, renderer, url);
    }
    assert(cache.get(first) == nullptr && cache.get(prefix + "32.webp") != nullptr);
    cache.stop();
    assert(cache.get(prefix + "32.webp") == nullptr);
}
struct TestDirectory {
    std::string path;
    TestDirectory() {
        char name[] = "/tmp/fogplay-artwork-XXXXXX";
        const auto created = mkdtemp(name);
        if (!created) throw std::runtime_error("Cannot create artwork test directory");
        path = created;
    }
    ~TestDirectory() { std::filesystem::remove_all(path); }
    fog::ArtworkDiskOptions options(std::size_t files = 256, std::size_t bytes = 128 * 1024 * 1024) const {
        return {path + "/cache/artwork", bytes, files};
    }
};
std::vector<std::filesystem::path> records(const std::string& directory) {
    std::vector<std::filesystem::path> files;
    for (const auto& file : std::filesystem::directory_iterator(directory))
        if (file.path().extension() == ".cache") files.push_back(file.path());
    return files;
}
void populate(SDL_Renderer* renderer, const fog::ArtworkDiskOptions& options,
              const std::string& url, const std::vector<std::uint8_t>& bytes) {
    fog::ArtworkCache cache([&](const std::string&, const std::atomic_bool&) { return bytes; }, options);
    assert(cache.request(url));
    (void)awaitTexture(cache, renderer, url);
}
void diskRestartAndIsolation(SDL_Renderer* renderer) {
    TestDirectory directory;
    const auto options = directory.options();
    const auto url = prefix + "first.webp";
    const auto other = prefix + "other.webp";
    const auto bytes = webp(3, 2);
    populate(renderer, options, url, bytes);
    const auto files = records(options.directory);
    assert(files.size() == 1);
    assert(std::filesystem::file_size(files.front()) == 16 + url.size() + bytes.size());
    std::atomic_int calls{0};
    fog::ArtworkCache cache([&](const std::string& requested, const std::atomic_bool&) {
        assert(requested == other);
        ++calls;
        return webp(7, 4);
    }, options);
    assert(cache.request(url));
    auto* first = awaitTexture(cache, renderer, url);
    int width = 0;
    assert(SDL_QueryTexture(first, nullptr, nullptr, &width, nullptr) == 0 && width == 3);
    assert(calls == 0);
    assert(cache.request(other));
    auto* second = awaitTexture(cache, renderer, other);
    assert(SDL_QueryTexture(second, nullptr, nullptr, &width, nullptr) == 0 && width == 7);
    assert(calls == 1);
    cache.stop();
    assert(records(options.directory).size() == 2);
    // Simulate a filename hash collision: the full public URL in the header must match.
    const auto both = records(options.directory);
    std::filesystem::copy_file(both[0], both[1], std::filesystem::copy_options::overwrite_existing);
    std::atomic_int repaired{0};
    fog::ArtworkCache collision([&](const std::string&, const std::atomic_bool&) {
        ++repaired; return bytes;
    }, options);
    assert(collision.request(url));
    (void)awaitTexture(collision, renderer, url);
    assert(collision.request(other));
    (void)awaitTexture(collision, renderer, other);
    assert(repaired == 1);
}
void diskCorruption(SDL_Renderer* renderer) {
    const auto bytes = webp(3, 2);
    const auto url = prefix + "broken.webp";
    for (int kind = 0; kind < 6; ++kind) {
        TestDirectory directory;
        const auto options = directory.options();
        populate(renderer, options, url, bytes);
        const auto file = records(options.directory).front();
        if (kind == 0) std::filesystem::resize_file(file, 9); // Truncated header.
        if (kind == 1) std::filesystem::resize_file(file, 16 + url.size() + bytes.size() - 1);
        if (kind == 2) std::filesystem::resize_file(file, fog::maxArtworkBytes + 1041);
        if (kind == 3) {
            std::fstream output(file, std::ios::in | std::ios::out | std::ios::binary);
            output.seekp(16 + url.size());
            output.put('\0'); // Valid header, invalid WebP.
        }
        if (kind == 4) {
            // A complete decodable WebP still has to pass the pixel limit.
            const auto huge = webp(2049, 1);
            std::fstream output(file, std::ios::in | std::ios::out | std::ios::binary);
            output.seekp(12);
            for (unsigned i = 0; i < 4; ++i) output.put(static_cast<char>(huge.size() >> (8 * i)));
            output.seekp(16 + url.size());
            output.write(reinterpret_cast<const char*>(huge.data()), huge.size());
            output.close();
            std::filesystem::resize_file(file, 16 + url.size() + huge.size());
        }
        if (kind == 5) {
            std::fstream output(file, std::ios::in | std::ios::out | std::ios::binary);
            output.seekp(12);
            output.put('\xff'); output.put('\xff'); output.put('\xff'); output.put('\x7f');
        }
        std::atomic_int calls{0};
        fog::ArtworkCache cache([&](const std::string&, const std::atomic_bool&) { ++calls; return bytes; }, options);
        assert(cache.request(url));
        (void)awaitTexture(cache, renderer, url);
        assert(calls == 1);
        cache.stop();
        assert(records(options.directory).size() == 1);
        // Replacement also works under the wrapped Horizon no-replace rename behavior.
        fog::ArtworkCache restart([](const std::string&, const std::atomic_bool&) -> std::vector<std::uint8_t> {
            throw std::runtime_error("Repaired artwork must survive restart");
        }, options);
        assert(restart.request(url));
        (void)awaitTexture(restart, renderer, url);
    }
}
void diskUnavailable(SDL_Renderer* renderer) {
    TestDirectory directory;
    const auto bytes = webp(3, 2);
    std::ofstream(directory.path + "/file") << "user file";
    auto options = directory.options();
    options.directory = directory.path + "/file/subdirectory";
    populate(renderer, options, prefix + "unavailable.webp", bytes);
    assert(std::filesystem::file_size(directory.path + "/file") == 9);
    std::filesystem::create_directory(directory.path + "/readonly");
    chmod((directory.path + "/readonly").c_str(), 0555);
    options.directory = directory.path + "/readonly";
    populate(renderer, options, prefix + "readonly.webp", bytes);
    assert(records(options.directory).empty());
    chmod(options.directory.c_str(), 0755);
    // A cache directory symlink must never cause writes into its target.
    std::filesystem::create_directory(directory.path + "/outside");
    std::filesystem::create_directory_symlink(directory.path + "/outside", directory.path + "/linked");
    options.directory = directory.path + "/linked";
    populate(renderer, options, prefix + "symlink.webp", bytes);
    assert(std::filesystem::is_empty(directory.path + "/outside"));
    options = directory.options();
    const auto url = prefix + "protected.webp";
    populate(renderer, options, url, bytes);
    const auto record = records(options.directory).front();
    const auto external = directory.path + "/outside-user";
    std::ofstream(external) << "user data";
    std::filesystem::remove(record);
    std::filesystem::create_symlink(external, record);
    populate(renderer, options, url, bytes);
    assert(std::filesystem::is_symlink(record));
    assert(std::filesystem::file_size(external) == 9);
}
void diskBounds(SDL_Renderer* renderer) {
    const auto bytes = webp(3, 2);
    TestDirectory directory;
    const auto large = directory.options();
    for (int i = 0; i < 6; ++i) populate(renderer, large, prefix + std::to_string(i) + ".webp", bytes);
    auto old = records(large.directory).front();
    // A stale interrupted write counts as managed storage and is reclaimed.
    std::ofstream(old.string() + ".tmp") << "partial";
    std::ofstream(large.directory + "/user-notes.txt") << "keep";
    std::filesystem::create_symlink(directory.path + "/outside-user", large.directory + "/" + old.filename().string() + ".symlink");
    const auto small = directory.options(2, 220);
    populate(renderer, small, prefix + "new.webp", bytes);
    std::size_t total = 0, count = 0;
    for (const auto& item : std::filesystem::directory_iterator(small.directory)) {
        if (item.path().filename().string().find("artwork-v1-") != 0 || item.is_symlink()) continue;
        total += item.file_size(); ++count;
        assert(item.path().extension() == ".cache");
    }
    assert(count > 0 && count <= small.maxFiles && total <= small.maxBytes);
    assert(std::filesystem::file_size(small.directory + "/user-notes.txt") == 4);
    assert(std::filesystem::is_symlink(large.directory + "/" + old.filename().string() + ".symlink"));
    // A record larger than the entire configured budget is simply not cached.
    const auto tiny = directory.options(1, 1);
    populate(renderer, tiny, prefix + "tiny.webp", bytes);
    assert(records(tiny.directory).empty());
}
void diskCancellation(SDL_Renderer* renderer) {
    (void)renderer;
    TestDirectory directory;
    const auto options = directory.options();
    std::atomic_bool entered{false};
    const auto bytes = webp(3, 2);
    fog::ArtworkCache cache([&](const std::string&, const std::atomic_bool& cancelled) {
        entered = true;
        while (!cancelled.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return bytes; // A downloader completing after cancellation must not persist anything.
    }, options);
    assert(cache.request(prefix + "cancel-disk.webp"));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!entered && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(entered);
    cache.stop();
    assert(std::filesystem::is_empty(options.directory));
}
void candidateFallback(SDL_Renderer* renderer) {
    const std::vector<std::string> urls{prefix + "missing.webp", prefix + "broken.png", prefix + "usable.jpg"};
    std::atomic_int calls{0};
    TestDirectory directory;
    const auto options = directory.options();
    {
        fog::ArtworkCache cache([&](const std::string& url, const std::atomic_bool&) {
            ++calls;
            if (url == urls[0]) throw std::runtime_error("HTTP 404");
            if (url == urls[1]) return std::vector<std::uint8_t>{1, 2, 3};
            assert(url == urls[2]); return raster(false);
        }, options);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!cache.get(urls) && std::chrono::steady_clock::now() < deadline) {
            cache.request(urls); cache.update(renderer);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        assert(cache.get(urls) && calls == 3);
        for (int i = 0; i < 100; ++i) assert(cache.request(urls));
        assert(calls == 3); // No repeated failing first image after a fallback is available.
    }
    assert(records(options.directory).size() == 1); // Only successful validated data persists.
    fog::ArtworkCache restarted([&](const std::string& url, const std::atomic_bool&) {
        ++calls; assert(url != urls[2]); throw std::runtime_error("Unavailable");
        return std::vector<std::uint8_t>{};
    }, options);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!restarted.get(urls) && std::chrono::steady_clock::now() < deadline) {
        restarted.request(urls); restarted.update(renderer);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(restarted.get(urls) && calls == 5); // JPEG cache hit remains usable after restart.
}
void steamAfterExhaustion(SDL_Renderer* renderer) {
    TestDirectory directory;
    const auto options = directory.options();
    const std::vector<std::string> mts{
        prefix + "missing.webp", prefix + "broken.png", prefix + "corrupt-cache.webp", prefix + "last.jpg"};
    // A corrupt on-disk MTS record must be retried, then fail before Steam starts.
    populate(renderer, options, mts[2], webp(3, 2));
    {
        std::fstream output(records(options.directory).front(), std::ios::in | std::ios::out | std::ios::binary);
        output.seekp(16 + mts[2].size()); output.put('x');
    }
    std::atomic_int calls{0};
    std::atomic_bool release{false};
    const auto jpeg = raster(false);
    {
        fog::ArtworkCache cache([&](const std::string& url, const std::atomic_bool& cancelled) {
            const auto call = calls.fetch_add(1);
            if (call < 4) {
                assert(url == mts[static_cast<std::size_t>(call)]);
                while (!release.load() && !cancelled.load())
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                if (call == 1) return std::vector<std::uint8_t>{1, 2, 3};
                throw std::runtime_error("MTS image unavailable");
            }
            assert(call == 4 && url == steamHeader);
            return jpeg;
        }, options);
        assert(!cache.exhausted(mts)); // Nothing has been attempted.
        assert(cache.request(mts));
        assert(!cache.exhausted(mts)); // In progress, even before any renderer update.
        release = true;
        auto capped = mts;
        capped.push_back(steamHeader);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!cache.exhausted(capped) && std::chrono::steady_clock::now() < deadline) {
            cache.request(capped); cache.update(renderer);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        assert(cache.exhausted(mts) && cache.exhausted(capped));
        assert(!cache.get(mts) && calls == 4); // A fifth URL cannot bypass the four-candidate cap.
        assert(cache.exhausted({mts[0], "https://evil.test/image.jpg"}));
        assert(!cache.exhausted({mts[0], prefix + "untried.jpg"}));
        for (int i = 0; i < 100; ++i) assert(!cache.request(mts));
        assert(calls == 4); // Definitive failures remain cooled down while fallback resolves.
        assert(cache.request(steamHeader)); // Resolver result uses its own request.
        assert(!cache.exhausted({steamHeader}));
        auto* texture = awaitTexture(cache, renderer, steamHeader);
        int width = 0, height = 0;
        assert(SDL_QueryTexture(texture, nullptr, nullptr, &width, &height) == 0 && width == 3 && height == 2);
        assert(!cache.exhausted({steamHeader}) && calls == 5);
    }
    assert(records(options.directory).size() == 1);
    std::atomic_int restartedCalls{0};
    fog::ArtworkCache restarted([&](const std::string&, const std::atomic_bool&) -> std::vector<std::uint8_t> {
        ++restartedCalls;
        throw std::runtime_error("Steam JPEG must load from SD cache");
    }, options);
    assert(restarted.request(steamHeader));
    (void)awaitTexture(restarted, renderer, steamHeader);
    assert(restartedCalls == 0 && !restarted.exhausted({steamHeader}));
    // Only positions within the existing candidate cap are considered.
    assert(restarted.exhausted({"", "invalid", "http://invalid", "bad", steamHeader}));
}
void uploadFailureFallback(SDL_Renderer* renderer) {
    const std::vector<std::string> urls{prefix + "upload-failure.png", prefix + "upload-fallback.jpg"};
    std::atomic_int calls{0};
    fog::ArtworkCache cache([&](const std::string&, const std::atomic_bool&) {
        ++calls; return raster(true);
    });
    failNextArtworkUpload = true;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!cache.get(urls) && std::chrono::steady_clock::now() < deadline) {
        cache.request(urls); cache.update(renderer);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(cache.get(urls) && !cache.get(urls[0]) && cache.get(urls[1]) && calls == 2);
    for (int i = 0; i < 100; ++i) assert(cache.request(urls));
    assert(calls == 2);
}
void malformedDownload(SDL_Renderer* renderer) {
    std::atomic_int calls{0};
    fog::ArtworkCache cache([&](const std::string&, const std::atomic_bool&) {
        ++calls;
        return std::vector<std::uint8_t>(fog::maxArtworkBytes + 1, 0);
    });
    const auto url = prefix + "invalid.webp";
    assert(!cache.request("https://evil.test/a.webp"));
    assert(cache.request(url));
    for (int i = 0; i < 100; ++i) {
        cache.update(renderer);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(cache.get(url) == nullptr && calls == 1);
    assert(!cache.request(url)); // Failed responses do not trigger a request every frame.
    assert(cache.exhausted({url}));
}
void manyFailedCandidates() {
    std::atomic_int calls{0};
    fog::ArtworkCache cache([&](const std::string&, const std::atomic_bool&) -> std::vector<std::uint8_t> {
        ++calls; throw std::runtime_error("fixture missing image");
    });
    std::vector<std::string> urls;
    for (int i=0;i<80;++i) {
        const auto url = prefix + "missing-" + std::to_string(i) + ".png";
        urls.push_back(url);
        assert(cache.request(url));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!cache.exhausted({url}) && std::chrono::steady_clock::now()<deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        assert(cache.exhausted({url}));
    }
    for (const auto& url:urls) { assert(!cache.request(url)); assert(cache.exhausted({url})); }
    assert(calls == 80);
}

}
int main(int argc, char**) {
    validation();
    manyFailedCandidates();
    cancellation();
    idleWorkerStop();
    SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    if (SDL_Init(SDL_INIT_VIDEO) != 0) throw std::runtime_error(SDL_GetError());
    auto* surface = SDL_CreateRGBSurfaceWithFormat(0, 640, 480, 32, SDL_PIXELFORMAT_RGBA32);
    auto* renderer = SDL_CreateSoftwareRenderer(surface);
    if (!surface || !renderer) throw std::runtime_error(SDL_GetError());
    cacheAndEviction(renderer);
    malformedDownload(renderer);
    candidateFallback(renderer);
    steamAfterExhaustion(renderer);
    uploadFailureFallback(renderer);
    diskRestartAndIsolation(renderer);
    diskCorruption(renderer);
    diskUnavailable(renderer);
    diskBounds(renderer);
    diskCancellation(renderer);
    if (argc > 1) {
        fog::ArtworkCache cache;
        const std::vector<std::string> liveUrls{
            prefix + "73/8a/738a26b0f0df8ba5165de481efaa7092-425x267.webp",
            prefix + "a1/94/a194cb172edf51735c9f2a9eae796981-425x267.jpg",
            prefix + "44/65/4465d5263c022d8b89a4580d59ea038a-425x267.png"};
        for (const auto& url : liveUrls) {
            assert(cache.request(url));
            // Allow the real anonymous HTTPS transport's full timeout.
            SDL_Texture* texture = nullptr;
            for (int i = 0; i < 1500 && !texture; ++i) {
                cache.update(renderer); texture = cache.get(url);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            assert(texture);
            int width = 0, height = 0;
            assert(SDL_QueryTexture(texture, nullptr, nullptr, &width, &height) == 0);
            assert((url.back() == 'g' && url.find(".png") != std::string::npos) ?
                (width == 283 && height == 178) : (width == 425 && height == 267));
        }
        std::cout << "Anonymous FogPlay WebP + JPEG + PNG HTTPS/decode/upload passed (including real PNG smaller than URL dimensions).\n";
    }
    SDL_DestroyRenderer(renderer);
    SDL_FreeSurface(surface);
    SDL_Quit();
    std::cout << "Persistent artwork cache tests passed: restart, URL collision isolation, truncated/corrupt/oversize/pixel fallback, unwritable SD, symlinks, byte/file bounds, cancellation, Horizon rename.\n";
    std::cout << "Steam fallback tests passed: canonical URL allowlist, four-MTS exhaustion, queue pressure, decode/corrupt-cache failure, separate Steam request, JPEG SD restart without network.\n";
    std::cout << "Artwork tests passed: URL restrictions, byte/pixel bounds, worker cancellation, queue limit, main-thread upload, dedup, LRU and failed-request cooldown.\n";
}
