#include "features/catalog/infrastructure/artwork.hpp"
#include "core/network/http.hpp"
#include <SDL.h>
#include <SDL_image.h>
#include <curl/curl.h>
#include <webp/decode.h>
#include <algorithm>
#include <chrono>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>
#ifdef __SWITCH__
extern "C" {
#include <switch/result.h>
#include <switch/runtime/devices/fs_dev.h>
}
#endif
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace fog {
namespace {
constexpr std::size_t maxTextures = 32;
constexpr std::size_t maxPending = 32;
// Keep failures across catalog pages and all fallback candidates, not only queued downloads.
constexpr std::size_t maxFailures = 2048;
constexpr std::size_t maxReady = 4;
// Let the remaining candidates finish their bounded requests before retrying the
// first failure. Four 10-second requests (including redirects) can exceed 30s.
constexpr auto failureBackoff = std::chrono::minutes(5);
// Disk records use a stable two-word hash for filenames and retain the entire
// validated public URL. A hash collision therefore becomes a miss, never a wrong cover.
constexpr char diskMagic[] = "FPNXART1";
constexpr std::size_t diskHeaderBytes = 16;
constexpr std::size_t diskChunk = 64 * 1024;
std::string diskName(const std::string& url) {
    std::uint64_t first = 14695981039346656037ULL;
    std::uint64_t second = 7809847782465536322ULL;
    for (unsigned char c : url) {
        first = (first ^ c) * 1099511628211ULL;
        second = (second ^ c) * 14029467366897019727ULL;
    }
    char name[64];
    std::snprintf(name, sizeof(name), "artwork-v1-%016llx%016llx.cache",
        static_cast<unsigned long long>(first), static_cast<unsigned long long>(second));
    return name;
}
bool diskFileName(const std::string& name) {
    constexpr std::size_t prefix = 11;
    if (name.size() != prefix + 32 + 6 && name.size() != prefix + 32 + 10) return false;
    if (name.compare(0, prefix, "artwork-v1-") != 0 ||
        name.compare(prefix + 32, 6, ".cache") != 0) return false;
    if (name.size() > prefix + 32 + 6 && name.compare(prefix + 32 + 6, 4, ".tmp") != 0) return false;
    return std::all_of(name.begin() + prefix, name.begin() + prefix + 32, [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
void putWord(std::uint8_t* bytes, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[i] = static_cast<std::uint8_t>(value >> (8 * i));
}
std::uint32_t getWord(const std::uint8_t* bytes) {
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= static_cast<std::uint32_t>(bytes[i]) << (8 * i);
    return value;
}
struct CloseDirectory { void operator()(DIR* value) const { (void)closedir(value); } };
struct CloseFile { void operator()(FILE* value) const { (void)fclose(value); } };
class ArtworkDisk {
    struct Entry { std::string path; std::size_t size; std::time_t modified; };
    ArtworkDiskOptions options_;
    const std::atomic_bool& cancelled_;
    bool enabled_ = false;
    bool createDirectory() {
        // Reject symlinks at every component; all removals below stay in this directory.
        for (std::size_t end = 0; end <= options_.directory.size(); ++end) {
            if (end != options_.directory.size() && options_.directory[end] != '/') continue;
            const auto part = options_.directory.substr(0, end);
            if (part.empty()) continue;
            if (cancelled_.load(std::memory_order_relaxed)) return false;
            struct stat info{};
            // libnx device roots use the slash form (sdmc:/). Never attempt
            // to create a device name as if it were a normal directory.
            if (part.back() == ':') {
                const auto deviceRoot = part + '/';
                if (lstat(deviceRoot.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) return false;
                continue;
            }
            if (lstat(part.c_str(), &info) == 0) {
                if (!S_ISDIR(info.st_mode)) return false;
            } else if (errno != ENOENT || mkdir(part.c_str(), 0755) != 0) return false;
        }
        return true;
    }
    // Scan with bounded metadata, including crash leftovers. Only our exact
    // filename namespace is managed; unrelated files and symlinks are untouched.
    bool trim(std::size_t reserveBytes, std::size_t reserveFiles) {
        std::unique_ptr<DIR, CloseDirectory> directory(opendir(options_.directory.c_str()));
        if (!directory || reserveBytes > options_.maxBytes || reserveFiles > options_.maxFiles) return false;
        std::vector<Entry> kept;
        std::size_t total = 0;
        bool success = true;
        while (const auto* item = readdir(directory.get())) {
            if (cancelled_.load(std::memory_order_relaxed)) return false;
            const std::string name(item->d_name);
            if (!diskFileName(name)) continue;
            const auto path = options_.directory + '/' + name;
            struct stat info{};
            if (lstat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) continue;
            if (name.size() > 49 || info.st_size < 0 ||
                static_cast<std::uint64_t>(info.st_size) > maxArtworkBytes + diskHeaderBytes + 1024) {
                if (unlink(path.c_str()) != 0) success = false;
                continue;
            }
            kept.push_back({path, static_cast<std::size_t>(info.st_size), info.st_mtime});
            total += kept.back().size;
            while (!kept.empty() && (total > options_.maxBytes - reserveBytes ||
                    kept.size() > options_.maxFiles - reserveFiles)) {
                const auto oldest = std::min_element(kept.begin(), kept.end(), [](const auto& a, const auto& b) {
                    return a.modified < b.modified || (a.modified == b.modified && a.path < b.path);
                });
                if (unlink(oldest->path.c_str()) != 0) success = false;
                total -= oldest->size;
                kept.erase(oldest);
            }
        }
        return success;
    }
    bool readBytes(FILE* file, std::uint8_t* bytes, std::size_t size) {
        for (std::size_t offset = 0; offset < size;) {
            if (cancelled_.load(std::memory_order_relaxed)) return false;
            const auto amount = std::min(size - offset, diskChunk);
            const auto count = fread(bytes + offset, 1, amount, file);
            if (count != amount) return false;
            offset += count;
        }
        return true;
    }
    bool writeBytes(int descriptor, const void* data, std::size_t size) {
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        for (std::size_t offset = 0; offset < size;) {
            if (cancelled_.load(std::memory_order_relaxed)) return false;
            const auto count = ::write(descriptor, bytes + offset, std::min(size - offset, diskChunk));
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) return false;
            offset += static_cast<std::size_t>(count);
        }
        return true;
    }
public:
    ArtworkDisk(ArtworkDiskOptions options, const std::atomic_bool& cancelled)
        : options_(std::move(options)), cancelled_(cancelled) {
        options_.maxBytes = std::min(options_.maxBytes, std::size_t{128 * 1024 * 1024});
        options_.maxFiles = std::min(options_.maxFiles, std::size_t{256});
        while (!options_.directory.empty() && options_.directory.back() == '/') options_.directory.pop_back();
    }
    void initialize() noexcept {
        try {
            enabled_ = !options_.directory.empty() && options_.maxFiles && options_.maxBytes &&
                createDirectory() && trim(0, 0);
        } catch (...) { enabled_ = false; }
    }
    void discard(const std::string& url) noexcept {
        if (!enabled_) return;
        try {
            const auto path = options_.directory + '/' + diskName(url);
            struct stat info{};
            if (lstat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode)) (void)unlink(path.c_str());
        } catch (...) { /* The disk cache is always optional. */ }
    }
    std::vector<std::uint8_t> read(const std::string& url) noexcept {
        if (!enabled_) return {};
        try {
            const auto path = options_.directory + '/' + diskName(url);
            struct stat info{};
            if (lstat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode) ||
                info.st_size < static_cast<off_t>(diskHeaderBytes) ||
                static_cast<std::uint64_t>(info.st_size) > maxArtworkBytes + diskHeaderBytes + 1024) return {};
            std::unique_ptr<FILE, CloseFile> file(fopen(path.c_str(), "rb"));
            std::array<std::uint8_t, diskHeaderBytes> header{};
            if (!file || !readBytes(file.get(), header.data(), header.size()) ||
                std::memcmp(header.data(), diskMagic, 8) != 0) return {};
            const auto urlSize = getWord(header.data() + 8);
            const auto payloadSize = getWord(header.data() + 12);
            if (urlSize != url.size() || !payloadSize || payloadSize > maxArtworkBytes ||
                static_cast<std::uint64_t>(info.st_size) != diskHeaderBytes + urlSize + payloadSize) return {};
            std::vector<std::uint8_t> storedUrl(urlSize);
            if (!readBytes(file.get(), storedUrl.data(), storedUrl.size()) ||
                !std::equal(storedUrl.begin(), storedUrl.end(), url.begin())) return {};
            std::vector<std::uint8_t> bytes(payloadSize);
            if (!readBytes(file.get(), bytes.data(), bytes.size())) return {};
            return bytes;
        } catch (...) { return {}; }
    }
    void touch(const std::string& url) noexcept {
        if (!enabled_) return;
        try { (void)utime((options_.directory + '/' + diskName(url)).c_str(), nullptr); }
        catch (...) { /* Recency updates are best effort. */ }
    }
    void write(const std::string& url, const std::vector<std::uint8_t>& bytes) noexcept {
        if (!enabled_ || cancelled_.load(std::memory_order_relaxed)) return;
        std::string temporary;
        try {
            const auto recordSize = diskHeaderBytes + url.size() + bytes.size();
            if (recordSize > options_.maxBytes || !trim(recordSize, 1)) return;
            const auto path = options_.directory + '/' + diskName(url);
            temporary = path + ".tmp";
            // Exclusive creation never follows an existing symlink or truncates a user file.
            const int descriptor = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
            if (descriptor < 0) return;
            std::array<std::uint8_t, diskHeaderBytes> header{};
            std::memcpy(header.data(), diskMagic, 8);
            putWord(header.data() + 8, static_cast<std::uint32_t>(url.size()));
            putWord(header.data() + 12, static_cast<std::uint32_t>(bytes.size()));
            bool okay = writeBytes(descriptor, header.data(), header.size()) &&
                writeBytes(descriptor, url.data(), url.size()) &&
                writeBytes(descriptor, bytes.data(), bytes.size()) && fsync(descriptor) == 0;
            if (close(descriptor) != 0) okay = false;
            if (!okay || cancelled_.load(std::memory_order_relaxed)) { (void)unlink(temporary.c_str()); return; }
            // Horizon rename does not replace an existing target. A missing cache
            // record after interruption is harmless; readers never see partial data.
            struct stat info{};
            if (lstat(path.c_str(), &info) == 0 &&
                (!S_ISREG(info.st_mode) || unlink(path.c_str()) != 0)) {
                (void)unlink(temporary.c_str()); return;
            }
            if (std::rename(temporary.c_str(), path.c_str()) != 0) { (void)unlink(temporary.c_str()); return; }
#ifdef __SWITCH__
            FsFileSystem* device = nullptr;
            char translated[FS_MAX_PATH];
            if (fsdevTranslatePath(path.c_str(), &device, translated) >= 0 && device) (void)fsFsCommit(device);
#endif
        } catch (...) {
            if (!temporary.empty()) (void)unlink(temporary.c_str());
        }
    }
};
struct DownloadBuffer {
    std::vector<std::uint8_t> bytes;
    const std::atomic_bool* cancelled = nullptr;
};
std::size_t writeImage(char* data, std::size_t size, std::size_t count, void* userdata) noexcept {
    auto& buffer = *static_cast<DownloadBuffer*>(userdata);
    if (buffer.cancelled->load(std::memory_order_relaxed) ||
        (size != 0 && count > (std::numeric_limits<std::size_t>::max)() / size)) return 0;
    const auto bytes = size * count;
    if (bytes > maxArtworkBytes - buffer.bytes.size()) return 0;
    try { buffer.bytes.insert(buffer.bytes.end(), data, data + bytes); }
    catch (...) { return 0; }
    return bytes;
}
int imageProgress(void* data, curl_off_t total, curl_off_t, curl_off_t, curl_off_t) noexcept {
    return static_cast<const std::atomic_bool*>(data)->load(std::memory_order_relaxed) ||
        total > static_cast<curl_off_t>(maxArtworkBytes) ? 1 : 0;
}
void checkCurl(CURLcode result) {
    if (result != CURLE_OK) throw std::runtime_error("Artwork HTTPS request failed");
}
std::vector<std::uint8_t> downloadArtwork(const std::string& requested,
                                        const std::atomic_bool& cancelled) {
    // One budget for the complete chain: redirect hops must not multiply the
    // timeout and keep visible cards from reaching the next artwork candidate.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    std::string url = requested;
    for (unsigned redirect = 0; redirect <= 3; ++redirect) {
        if (cancelled.load(std::memory_order_relaxed)) throw std::runtime_error("Artwork cancelled");
        const auto remaining = static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count());
        if (remaining <= 0) throw std::runtime_error("Artwork request deadline reached");
        if (!validArtworkUrl(url)) throw std::runtime_error("Artwork URL rejected");
        std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle(curl_easy_init(), &curl_easy_cleanup);
        if (!handle) throw std::runtime_error("Cannot initialize artwork HTTPS request");
        DownloadBuffer buffer{{}, &cancelled};
        checkCurl(curl_easy_setopt(handle.get(), CURLOPT_URL, url.c_str()));
        checkCurl(curl_easy_setopt(handle.get(), CURLOPT_USERAGENT, "FogPlayNX/0.4.19"));
#if LIBCURL_VERSION_NUM >= 0x075500
        checkCurl(curl_easy_setopt(handle.get(), CURLOPT_PROTOCOLS_STR, "https"));
#else
        checkCurl(curl_easy_setopt(handle.get(), CURLOPT_PROTOCOLS, CURLPROTO_HTTPS));
#endif
#ifdef __SWITCH__
        checkCurl(curl_easy_setopt(handle.get(), CURLOPT_CAINFO, "romfs:/cacert.pem"));
#endif
        checkCurl(curl_easy_setopt(handle.get(), CURLOPT_SSL_VERIFYPEER, 1L));
        checkCurl(curl_easy_setopt(handle.get(), CURLOPT_SSL_VERIFYHOST, 2L));
        // Never let curl follow an unvalidated Location (even for HTTPS).
        checkCurl(curl_easy_setopt(handle.get(), CURLOPT_FOLLOWLOCATION, 0L));
        checkCurl(curl_easy_setopt(handle.get(), CURLOPT_CONNECTTIMEOUT_MS, std::min(4000L, remaining)));
        checkCurl(curl_easy_setopt(handle.get(), CURLOPT_TIMEOUT_MS, remaining));
        checkCurl(curl_easy_setopt(handle.get(), CURLOPT_NOSIGNAL, 1L));
        checkCurl(curl_easy_setopt(handle.get(), CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(maxArtworkBytes)));
        checkCurl(curl_easy_setopt(handle.get(), CURLOPT_NOPROGRESS, 0L));
        checkCurl(curl_easy_setopt(handle.get(), CURLOPT_XFERINFOFUNCTION, &imageProgress));
        checkCurl(curl_easy_setopt(handle.get(), CURLOPT_XFERINFODATA, &cancelled));
        checkCurl(curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, &writeImage));
        checkCurl(curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &buffer));
        checkCurl(curl_easy_perform(handle.get()));
        long status = 0;
        checkCurl(curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &status));
        if (status >= 300 && status < 400) {
            char* location = nullptr;
            checkCurl(curl_easy_getinfo(handle.get(), CURLINFO_REDIRECT_URL, &location));
            if (!location || redirect == 3) throw std::runtime_error("Artwork redirect rejected");
            url = location;
            continue;
        }
        if (status != 200) throw std::runtime_error("Artwork unavailable");
        return std::move(buffer.bytes);
    }
    throw std::runtime_error("Artwork redirect limit reached");
}
}

bool validArtworkUrl(const std::string& url) noexcept {
    if (url.size() > 1024) return false;
    // The resolver generates only canonical app headers. Do not accept general
    // Steam URLs, extra path segments, auth, queries, or arbitrary CDN subdomains.
    for (const std::string_view prefix : {
            "https://shared.cloudflare.steamstatic.com/store_item_assets/steam/apps/",
            "https://shared.akamai.steamstatic.com/store_item_assets/steam/apps/",
            "https://shared.steamstatic.com/store_item_assets/steam/apps/"}) {
        constexpr std::string_view suffix = "/header.jpg";
        if (url.compare(0, prefix.size(), prefix) != 0) continue;
        if (url.size() <= prefix.size() + suffix.size() ||
            url.compare(url.size() - suffix.size(), suffix.size(), suffix) != 0) return false;
        const auto id = std::string_view(url).substr(prefix.size(),
            url.size() - prefix.size() - suffix.size());
        return id.front() >= '1' && id.front() <= '9' &&
            std::all_of(id.begin(), id.end(), [](unsigned char c) { return c >= '0' && c <= '9'; });
    }
    constexpr std::string_view prefix = "https://fogplay.mts.ru/image/preview/";
    if (url.size() <= prefix.size() || url.size() > 1024 ||
        url.compare(0, prefix.size(), prefix) != 0 ||
        (!(url.size() >= 5 && url.compare(url.size() - 5, 5, ".webp") == 0) &&
        !(url.size() >= 4 && (url.compare(url.size() - 4, 4, ".jpg") == 0 ||
                            url.compare(url.size() - 4, 4, ".png") == 0)) &&
        !(url.size() >= 5 && url.compare(url.size() - 5, 5, ".jpeg") == 0))) return false;
    const auto path = std::string_view(url).substr(prefix.size());
    if (path.find("..") != std::string_view::npos || path.front() == '/') return false;
    return std::all_of(path.begin(), path.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '/' || c == '.' || c == '-' || c == '_';
    });
}
namespace {
void validateDimensions(int width, int height) {
    if (width < 1 || height < 1 || width > 2048 || height > 2048 ||
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) > maxArtworkPixels)
        throw std::runtime_error("Invalid artwork dimensions");
}
std::uint32_t bigEndian32(const std::uint8_t* data) {
    return (std::uint32_t(data[0]) << 24) | (std::uint32_t(data[1]) << 16) |
        (std::uint32_t(data[2]) << 8) | data[3];
}
std::pair<int, int> jpegDimensions(const std::vector<std::uint8_t>& bytes) {
    std::size_t offset = 2;
    while (offset + 1 < bytes.size()) {
        if (bytes[offset++] != 0xff) break;
        while (offset < bytes.size() && bytes[offset] == 0xff) ++offset;
        if (offset >= bytes.size()) break;
        const auto marker = bytes[offset++];
        if (marker == 0xda || marker == 0xd9 || marker == 0x00) break;
        if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) continue;
        if (offset + 2 > bytes.size()) break;
        const auto length = (std::size_t(bytes[offset]) << 8) | bytes[offset + 1];
        if (length < 2 || length > bytes.size() - offset) break;
        if (marker == 0xc0 || marker == 0xc1 || marker == 0xc2) {
            if (length < 8) break;
            const int height = (int(bytes[offset + 3]) << 8) | bytes[offset + 4];
            const int width = (int(bytes[offset + 5]) << 8) | bytes[offset + 6];
            validateDimensions(width, height);
            return {width, height};
        }
        offset += length;
    }
    throw std::runtime_error("Invalid JPEG artwork header");
}
}
ArtworkPixels decodeArtwork(const std::vector<std::uint8_t>& bytes) {
    if (bytes.empty() || bytes.size() > maxArtworkBytes) throw std::runtime_error("Invalid artwork size");
    const bool webp = bytes.size() >= 12 && std::memcmp(bytes.data(), "RIFF", 4) == 0 &&
        std::memcmp(bytes.data() + 8, "WEBP", 4) == 0;
    if (webp) {
        WebPBitstreamFeatures info{};
        if (WebPGetFeatures(bytes.data(), bytes.size(), &info) != VP8_STATUS_OK || info.has_animation)
            throw std::runtime_error("Invalid WebP artwork");
        validateDimensions(info.width, info.height);
        ArtworkPixels pixels{info.width, info.height, {}};
        pixels.rgba.resize(static_cast<std::size_t>(info.width) * static_cast<std::size_t>(info.height) * 4);
        if (!WebPDecodeRGBAInto(bytes.data(), bytes.size(), pixels.rgba.data(), pixels.rgba.size(), info.width * 4))
            throw std::runtime_error("Artwork decode failed");
        return pixels;
    }
    constexpr std::uint8_t pngSignature[] = {137,80,78,71,13,10,26,10};
    int width = 0, height = 0;
    const char* format = nullptr;
    if (bytes.size() >= 33 && std::memcmp(bytes.data(), pngSignature, sizeof(pngSignature)) == 0 &&
        bigEndian32(bytes.data() + 8) == 13 && std::memcmp(bytes.data() + 12, "IHDR", 4) == 0) {
        const auto w = bigEndian32(bytes.data() + 16), h = bigEndian32(bytes.data() + 20);
        if (w > 2048 || h > 2048) throw std::runtime_error("Invalid PNG artwork dimensions");
        width = static_cast<int>(w); height = static_cast<int>(h); format = "PNG";
        validateDimensions(width, height);
    } else if (bytes.size() >= 4 && bytes[0] == 0xff && bytes[1] == 0xd8) {
        const auto dimensions = jpegDimensions(bytes);
        width = dimensions.first; height = dimensions.second; format = "JPG";
    } else throw std::runtime_error("Unsupported artwork format");
    // Explicit loaders prevent SDL_image from attempting unrelated formats such as SVG.
    auto* input = SDL_RWFromConstMem(bytes.data(), static_cast<int>(bytes.size()));
    if (!input) throw std::runtime_error("Artwork memory stream failed");
    std::unique_ptr<SDL_Surface, decltype(&SDL_FreeSurface)> decoded(
        format[0] == 'P' ? IMG_LoadPNG_RW(input) : IMG_LoadJPG_RW(input), &SDL_FreeSurface);
    SDL_RWclose(input);
    if (!decoded || decoded->w != width || decoded->h != height)
        throw std::runtime_error("Artwork decode failed");
    std::unique_ptr<SDL_Surface, decltype(&SDL_FreeSurface)> rgba(
        SDL_ConvertSurfaceFormat(decoded.get(), SDL_PIXELFORMAT_RGBA32, 0), &SDL_FreeSurface);
    if (!rgba) throw std::runtime_error("Artwork pixel conversion failed");
    ArtworkPixels pixels{width, height, {}};
    pixels.rgba.resize(static_cast<std::size_t>(width) * height * 4);
    for (int row = 0; row < height; ++row)
        std::memcpy(pixels.rgba.data() + static_cast<std::size_t>(row) * width * 4,
            static_cast<const std::uint8_t*>(rgba->pixels) + static_cast<std::size_t>(row) * rgba->pitch,
            static_cast<std::size_t>(width) * 4);
    return pixels;
}

struct ArtworkCache::Impl {
    // Initializing this guard on the UI thread creates the shared static curl runtime
    // before any download starts; it has process lifetime and cannot race worker teardown.
    HttpClient runtimeGuard;
    Downloader download;
    std::atomic_bool cancelled{false};
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::string> queue;
    std::unordered_set<std::string> pending;
    struct Ready { std::string url; ArtworkPixels pixels; };
    std::deque<Ready> ready;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> failed;
    struct Texture { SDL_Texture* value; std::uint64_t used; };
    std::unordered_map<std::string, Texture> textures;
    std::uint64_t clock = 0;
    std::thread worker;
    ArtworkDisk disk;
    explicit Impl(Downloader supplied, ArtworkDiskOptions options)
        : download(supplied ? std::move(supplied) : downloadArtwork), disk(std::move(options), cancelled) {
        // SDL_image's optional loaders initialize once on the UI thread, before the
        // worker can decode and concurrently with no UI asset initialization.
        // Retain the process-wide reference: UI branding uses these loaders too.
        static const int imageLoaders = IMG_Init(IMG_INIT_JPG | IMG_INIT_PNG);
        (void)imageLoaders;
        worker = std::thread([this] { run(); });
    }
    void failedRequest(const std::string& url) noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        pending.erase(url);
        try {
            if (failed.size() >= maxFailures) {
                const auto oldest = std::min_element(failed.begin(), failed.end(),
                    [](const auto& a, const auto& b) { return a.second < b.second; });
                failed.erase(oldest);
            }
            failed[url] = std::chrono::steady_clock::now();
        } catch (...) { /* Preserve the placeholder if memory is exhausted. */ }
    }
    void run() noexcept {
        disk.initialize();
        while (!cancelled.load(std::memory_order_relaxed)) {
            std::string url;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock, [this] { return cancelled.load(std::memory_order_relaxed) ||
                    (!queue.empty() && ready.size() < maxReady); });
                if (cancelled.load(std::memory_order_relaxed)) return;
                url = std::move(queue.front());
                queue.pop_front();
            }
            try {
                auto bytes = disk.read(url);
                ArtworkPixels pixels;
                bool hit = false;
                if (!bytes.empty()) {
                    try { pixels = decodeArtwork(bytes); hit = true; }
                    catch (...) { disk.discard(url); }
                }
                if (cancelled.load(std::memory_order_relaxed)) return;
                if (hit) disk.touch(url);
                else {
                    bytes = download(url, cancelled);
                    if (cancelled.load(std::memory_order_relaxed)) return;
                    pixels = decodeArtwork(bytes);
                    disk.write(url, bytes);
                }
                std::lock_guard<std::mutex> lock(mutex);
                if (!cancelled.load(std::memory_order_relaxed)) ready.push_back({url, std::move(pixels)});
            } catch (...) {
                failedRequest(url);
            }
        }
    }
};
ArtworkCache::ArtworkCache(Downloader downloader, ArtworkDiskOptions disk)
    : impl_(std::make_unique<Impl>(std::move(downloader), std::move(disk))) {}
ArtworkCache::~ArtworkCache() { stop(); }
bool ArtworkCache::request(const std::string& url) {
    if (!validArtworkUrl(url) || impl_->cancelled.load(std::memory_order_relaxed)) return false;
    if (auto found = impl_->textures.find(url); found != impl_->textures.end()) {
        found->second.used = ++impl_->clock;
        return true;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (const auto failed = impl_->failed.find(url); failed != impl_->failed.end()) {
        if (std::chrono::steady_clock::now() - failed->second < failureBackoff) return false;
        impl_->failed.erase(failed);
    }
    if (impl_->pending.count(url)) return true;
    if (impl_->pending.size() >= maxPending) return false;
    impl_->pending.insert(url);
    try { impl_->queue.push_back(url); }
    catch (...) { impl_->pending.erase(url); throw; }
    impl_->wake.notify_one();
    return true;
}
bool ArtworkCache::request(const std::vector<std::string>& urls) {
    if (get(urls)) return true;
    const auto count = std::min(urls.size(), maxArtworkCandidates);
    for (std::size_t i = 0; i < count; ++i)
        if (request(urls[i])) return true;
    return false;
}
bool ArtworkCache::exhausted(const std::vector<std::string>& urls) const {
    const auto count = std::min(urls.size(), maxArtworkCandidates);
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (std::size_t i = 0; i < count; ++i) {
        const auto& url = urls[i];
        if (!validArtworkUrl(url)) continue;
        if (impl_->textures.count(url) || impl_->pending.count(url)) return false;
        const auto failure = impl_->failed.find(url);
        if (failure == impl_->failed.end() || now - failure->second >= failureBackoff) return false;
    }
    return true;
}
SDL_Texture* ArtworkCache::get(const std::vector<std::string>& urls) {
    const auto count = std::min(urls.size(), maxArtworkCandidates);
    for (std::size_t i = 0; i < count; ++i)
        if (auto* texture = get(urls[i])) return texture;
    return nullptr;
}
void ArtworkCache::update(SDL_Renderer* renderer) {
    if (!renderer || impl_->cancelled.load(std::memory_order_relaxed)) return;
    // At most one upload per frame keeps the UI responsive.
    Impl::Ready item;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->ready.empty()) return;
        item = std::move(impl_->ready.front());
        impl_->ready.pop_front();
        impl_->pending.erase(item.url);
    }
    impl_->wake.notify_one();
    auto* surface = SDL_CreateRGBSurfaceWithFormatFrom(item.pixels.rgba.data(), item.pixels.width,
        item.pixels.height, 32, item.pixels.width * 4, SDL_PIXELFORMAT_RGBA32);
    if (!surface) { impl_->failedRequest(item.url); return; }
    auto* texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_FreeSurface(surface);
    if (!texture) { impl_->failedRequest(item.url); return; }
    if (impl_->textures.size() >= maxTextures) {
        const auto oldest = std::min_element(impl_->textures.begin(), impl_->textures.end(),
            [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
        SDL_DestroyTexture(oldest->second.value);
        impl_->textures.erase(oldest);
    }
    try { impl_->textures.emplace(item.url, Impl::Texture{texture, ++impl_->clock}); }
    catch (...) { SDL_DestroyTexture(texture); throw; }
}
SDL_Texture* ArtworkCache::get(const std::string& url) {
    const auto found = impl_->textures.find(url);
    if (found == impl_->textures.end()) return nullptr;
    found->second.used = ++impl_->clock;
    return found->second.value;
}
void ArtworkCache::stop() noexcept {
    // Match the condition-variable predicate's mutex so cancellation cannot be
    // notified between its last predicate check and entering the wait.
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->cancelled.store(true, std::memory_order_relaxed);
    }
    impl_->wake.notify_all();
    if (impl_->worker.joinable()) impl_->worker.join();
    for (const auto& item : impl_->textures) SDL_DestroyTexture(item.second.value);
    impl_->textures.clear();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->queue.clear();
    impl_->ready.clear();
    impl_->pending.clear();
    impl_->failed.clear();
}
}

