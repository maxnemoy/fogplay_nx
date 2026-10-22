#include "features/catalog/infrastructure/steam_artwork.hpp"
#include "core/network/http.hpp"
#include "core/storage/storage.hpp"
#include <nlohmann/json.hpp>
#include <curl/curl.h>
#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <fcntl.h>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace fog {
namespace {
using Json = nlohmann::json;
constexpr std::size_t maxTitleBytes = 256;
constexpr std::size_t maxResponseBytes = 256 * 1024;
// Three files (primary, temporary and backup) fit below 256 KiB mid-swap.
constexpr std::size_t maxMappingBytes = 80 * 1024;
constexpr std::int64_t positiveTtl = 30 * 24 * 60 * 60;
constexpr std::int64_t negativeTtl = 24 * 60 * 60;
constexpr std::int64_t errorCooldown = 60;
using Key = std::pair<int, std::string>;
// ASCII case folding and punctuation removal deliberately leave UTF-8 intact.
std::string normalize(const std::string& title) {
    std::string result;
    std::size_t characters = 0;
    bool hasLetter = false;
    for (std::size_t offset = 0; offset < title.size();) {
        const auto c = static_cast<unsigned char>(title[offset]);
        if (c < 128) {
            ++offset;
            if (c < 32 || c == 127) return {};
            if (c == '&') { result += "and"; characters += 3; hasLetter = true; }
            else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
                result += static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
                ++characters;
                if (c < '0' || c > '9') hasLetter = true;
            }
            continue;
        }
        std::size_t width = 0;
        std::uint32_t codepoint = 0;
        if (c >= 0xc2 && c <= 0xdf) { width = 2; codepoint = c & 0x1f; }
        else if (c >= 0xe0 && c <= 0xef) { width = 3; codepoint = c & 0x0f; }
        else if (c >= 0xf0 && c <= 0xf4) { width = 4; codepoint = c & 0x07; }
        else return {};
        if (width > title.size() - offset) return {};
        for (std::size_t i = 1; i < width; ++i) {
            const auto next = static_cast<unsigned char>(title[offset + i]);
            if ((next & 0xc0) != 0x80) return {};
            codepoint = (codepoint << 6) | (next & 0x3f);
        }
        if ((width == 3 && codepoint < 0x800) || (width == 4 && codepoint < 0x10000) ||
            codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff) ||
            (codepoint >= 0x80 && codepoint <= 0x9f)) return {};
        if (codepoint != 0x2122 && codepoint != 0x00ae) {
            result.append(title, offset, width);
            ++characters;
            hasLetter = true;
        }
        offset += width;
    }
    return characters >= 3 && hasLetter ? result : std::string{};
}
bool validTitle(const std::string& title) {
    return !title.empty() && title.size() <= maxTitleBytes && !normalize(title).empty();
}
std::int64_t integer(const Json& value) {
    if (!value.is_number_integer()) throw std::runtime_error("Invalid Steam integer");
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()))
            throw std::runtime_error("Invalid Steam integer");
        return static_cast<std::int64_t>(number);
    }
    return value.get<std::int64_t>();
}
Json parseJson(const std::string& bytes) {
    std::size_t events = 0;
    return Json::parse(bytes, [&events](int depth, Json::parse_event_t, Json&) {
        if (depth > 16 || ++events > 16384) throw std::runtime_error("Steam JSON is too complex");
        return true;
    });
}
std::int64_t matchedAppId(const std::string& title, const std::string& body) {
    if (body.size() > maxResponseBytes) throw std::runtime_error("Steam response is too large");
    const auto data = parseJson(body);
    if (!data.is_object() || !data.contains("items") || !data.at("items").is_array() ||
        data.at("items").size() > 512) throw std::runtime_error("Invalid Steam search response");
    const auto expected = normalize(title);
    std::int64_t result = 0;
    bool ambiguous = false;
    for (const auto& item : data.at("items")) {
        if (!item.is_object()) throw std::runtime_error("Invalid Steam item");
        const auto type = item.find("type");
        if (type == item.end()) continue;
        if (!type->is_string()) throw std::runtime_error("Invalid Steam item type");
        // Search also returns packages and bundles; their ids are not app ids.
        if (*type != "app") continue;
        if (!item.contains("id") || !item.contains("name") ||
            !item.at("name").is_string()) throw std::runtime_error("Invalid Steam item");
        const auto id = integer(item.at("id"));
        if (id <= 0 || id > 2147483647) throw std::runtime_error("Invalid Steam app id");
        const auto name = item.at("name").get<std::string>();
        if (name.size() > maxTitleBytes || normalize(name) != expected) continue;
        if (result != 0 && result != id) ambiguous = true;
        result = id;
    }
    return ambiguous ? 0 : result;
}
std::string headerUrl(std::int64_t appId) {
    return appId > 0 ? "https://shared.cloudflare.steamstatic.com/store_item_assets/steam/apps/" +
        std::to_string(appId) + "/header.jpg" : std::string{};
}
bool fresh(std::int64_t saved, std::int64_t now, std::int64_t ttl) {
    return saved >= 0 && now >= saved && now - saved < ttl;
}
struct ResponseBuffer { std::string body; };
std::size_t receive(char* data, std::size_t size, std::size_t count, void* userdata) noexcept {
    auto& body = static_cast<ResponseBuffer*>(userdata)->body;
    if (size != 0 && count > (std::numeric_limits<std::size_t>::max)() / size) return 0;
    const auto length = size * count;
    if (length > maxResponseBytes - body.size()) return 0;
    try { body.append(data, length); } catch (...) { return 0; }
    return length;
}
int progress(void* userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t) noexcept {
    return static_cast<const std::atomic_bool*>(userdata)->load(std::memory_order_relaxed) ? 1 : 0;
}
void check(CURLcode code) {
    if (code != CURLE_OK) throw std::runtime_error("Steam transport configuration failed");
}
SteamSearchResponse download(const std::string& title, const std::atomic_bool& cancelled) {
    HttpClient runtime;
    if (cancelled.load(std::memory_order_relaxed)) return {};
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle(curl_easy_init(), &curl_easy_cleanup);
    if (!handle) return {};
    std::unique_ptr<char, decltype(&curl_free)> encoded(
        curl_easy_escape(handle.get(), title.data(), static_cast<int>(title.size())), &curl_free);
    if (!encoded) return {};
    const auto url = std::string("https://store.steampowered.com/api/storesearch/?term=") +
        encoded.get() + "&cc=us&l=en";
    ResponseBuffer buffer;
    check(curl_easy_setopt(handle.get(), CURLOPT_URL, url.c_str()));
    check(curl_easy_setopt(handle.get(), CURLOPT_USERAGENT, "FogPlayNX/0.4.25"));
#if LIBCURL_VERSION_NUM >= 0x075500
    check(curl_easy_setopt(handle.get(), CURLOPT_PROTOCOLS_STR, "https"));
#else
    check(curl_easy_setopt(handle.get(), CURLOPT_PROTOCOLS, CURLPROTO_HTTPS));
#endif
    check(curl_easy_setopt(handle.get(), CURLOPT_FOLLOWLOCATION, 0L));
    check(curl_easy_setopt(handle.get(), CURLOPT_SSL_VERIFYPEER, 1L));
    check(curl_easy_setopt(handle.get(), CURLOPT_SSL_VERIFYHOST, 2L));
#ifdef __SWITCH__
    check(curl_easy_setopt(handle.get(), CURLOPT_CAINFO, "romfs:/cacert.pem"));
#endif
    check(curl_easy_setopt(handle.get(), CURLOPT_NOSIGNAL, 1L));
    check(curl_easy_setopt(handle.get(), CURLOPT_TIMEOUT_MS, 8000L));
    check(curl_easy_setopt(handle.get(), CURLOPT_CONNECTTIMEOUT_MS, 3000L));
    check(curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, receive));
    check(curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &buffer));
    check(curl_easy_setopt(handle.get(), CURLOPT_NOPROGRESS, 0L));
    check(curl_easy_setopt(handle.get(), CURLOPT_XFERINFOFUNCTION, progress));
    check(curl_easy_setopt(handle.get(), CURLOPT_XFERINFODATA, &cancelled));
    if (curl_easy_perform(handle.get()) != CURLE_OK) return {};
    long status = 0;
    if (curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &status) != CURLE_OK) return {};
    return {status, std::move(buffer.body)};
}
void ensureDirectory(const std::string& directory) {
    std::size_t start = 0;
    while (start < directory.size()) {
        const auto end = directory.find('/', start);
        const auto part = directory.substr(start, end == std::string::npos ? end : end - start);
        if (part == ".." || part == ".") throw std::runtime_error("Invalid Steam directory");
        const auto path = directory.substr(0, end);
        if (!path.empty() && path.back() != ':') {
            if (mkdir(path.c_str(), 0700) != 0 && errno != EEXIST)
                throw std::runtime_error("Cannot create Steam cache directory");
            struct stat info{};
            if (lstat(path.c_str(), &info) != 0 || !S_ISDIR(info.st_mode))
                throw std::runtime_error("Invalid Steam cache directory");
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
}
std::string readMappings(const std::string& path) {
    int flags = O_RDONLY;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
#ifdef O_NONBLOCK
    flags |= O_NONBLOCK;
#endif
    const int fd = open(path.c_str(), flags);
    if (fd < 0) throw std::runtime_error("Cannot read Steam mappings");
    struct stat info{};
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 1 ||
        static_cast<std::uint64_t>(info.st_size) > maxMappingBytes) {
        close(fd); throw std::runtime_error("Invalid Steam mappings");
    }
    std::string bytes(static_cast<std::size_t>(info.st_size), '\0');
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = read(fd, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { close(fd); throw std::runtime_error("Cannot read Steam mappings"); }
        offset += static_cast<std::size_t>(count);
    }
    char extra{};
    ssize_t count;
    do { count = read(fd, &extra, 1); } while (count < 0 && errno == EINTR);
    const auto closed = close(fd);
    if (count != 0 || closed != 0) throw std::runtime_error("Cannot read Steam mappings");
    return bytes;
}
}
struct SteamArtworkResolver::Impl {
    struct Record {
        std::int64_t appId = 0;
        std::int64_t saved = -1;
        std::int64_t failed = -1;
        std::uint64_t used = 0;
        bool pending = false;
    };
    Downloader downloader;
    SteamArtworkOptions options;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::map<Key, Record> records;
    std::deque<Key> queue;
    std::atomic_bool cancelled{false};
    std::thread worker;
    std::uint64_t sequence = 0;
    std::size_t pending = 0;
    bool ready = false;
    bool stopped = false;
    Impl(Downloader input, SteamArtworkOptions config)
        : downloader(input ? std::move(input) : Downloader(download)), options(std::move(config)) {
        options.maxRecords = std::clamp<std::size_t>(options.maxRecords, 1, 256);
        options.maxPending = std::clamp<std::size_t>(options.maxPending, 1, options.maxRecords);
        worker = std::thread([this] { run(); });
    }
    std::int64_t now() const noexcept {
        try { return (std::max<std::int64_t>)(0, options.clock ? options.clock() : std::time(nullptr)); }
        catch (...) { return 0; }
    }
    bool current(const Record& record, std::int64_t time) const {
        return fresh(record.saved, time, record.appId ? positiveTtl : negativeTtl);
    }
    bool reserve() {
        if (records.size() < options.maxRecords) return true;
        auto victim = records.end();
        for (auto it = records.begin(); it != records.end(); ++it) {
            if (!it->second.pending && (victim == records.end() || it->second.used < victim->second.used)) victim = it;
        }
        if (victim == records.end()) return false;
        records.erase(victim);
        return true;
    }
    void load() noexcept {
        if (options.directory.empty()) return;
        const auto path = options.directory + "/mappings.json";
        bool safeDirectory = false;
        try {
            ensureDirectory(options.directory);
            safeDirectory = true;
            recoverStorage(path);
            const auto data = parseJson(readMappings(path));
            if (!data.is_object() || data.size() != 2 || !data.contains("version") ||
                integer(data.at("version")) != 2 || !data.contains("records") ||
                !data.at("records").is_array() || data.at("records").size() > 256)
                throw std::runtime_error("Invalid Steam mappings");
            std::map<Key, Record> loaded;
            for (const auto& item : data.at("records")) {
                if (!item.is_object() || item.size() != 4 || !item.contains("mtsId") ||
                    !item.contains("title") || !item.at("title").is_string() ||
                    !item.contains("appId") || !item.contains("saved"))
                    throw std::runtime_error("Invalid Steam mapping");
                const auto id = integer(item.at("mtsId"));
                const auto title = item.at("title").get<std::string>();
                const auto appId = integer(item.at("appId"));
                const auto saved = integer(item.at("saved"));
                if (id <= 0 || id > 2147483647 || !validTitle(title) || appId < 0 ||
                    appId > 2147483647 || saved < 0)
                    throw std::runtime_error("Invalid Steam mapping");
                // Clock rollback makes future timestamps stale, not corrupt.
                // Retain positive mappings for offline display and let current()
                // schedule refresh without discarding unrelated valid records.
                if (!loaded.emplace(Key{static_cast<int>(id), title}, Record{appId, saved, -1, 0, false}).second)
                    throw std::runtime_error("Duplicate Steam mapping");
            }
            std::lock_guard<std::mutex> lock(mutex);
            for (const auto& item : loaded) {
                auto existing = records.find(item.first);
                if (existing != records.end()) {
                    existing->second.appId = item.second.appId;
                    existing->second.saved = item.second.saved;
                } else if (reserve()) {
                    auto record = item.second;
                    record.used = ++sequence;
                    records.emplace(item.first, record);
                }
            }
        } catch (...) {
            // Only clean disposable cache files after verifying their directory.
            if (safeDirectory) { try { clearStorage(path); } catch (...) {} }
        }
    }
    void save() noexcept {
        if (options.directory.empty()) return;
        try {
            std::vector<std::pair<Key, Record>> snapshot;
            {
                std::lock_guard<std::mutex> lock(mutex);
                for (const auto& item : records) if (item.second.saved >= 0) snapshot.push_back(item);
            }
            std::sort(snapshot.begin(), snapshot.end(), [](const auto& a, const auto& b) {
                return a.second.used > b.second.used;
            });
            std::string bytes = "{\"version\":2,\"records\":[";
            bool first = true;
            for (const auto& item : snapshot) {
                const auto entry = Json{{"mtsId", item.first.first}, {"title", item.first.second},
                    {"appId", item.second.appId}, {"saved", item.second.saved}}.dump();
                if (bytes.size() + entry.size() + (first ? 0 : 1) + 2 > maxMappingBytes) break;
                if (!first) bytes += ',';
                bytes += entry;
                first = false;
            }
            bytes += "]}";
            ensureDirectory(options.directory);
            replaceStorage(options.directory + "/mappings.json", bytes);
        } catch (...) { /* Optional disk cache failure never blocks resolution. */ }
    }
    void run() noexcept {
        load();
        {
            std::lock_guard<std::mutex> lock(mutex);
            ready = true;
        }
        for (;;) {
            Key key;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock, [this] { return stopped || !queue.empty(); });
                if (stopped) return;
                key = std::move(queue.front());
                queue.pop_front();
                auto& record = records.at(key);
                if (current(record, now())) { record.pending = false; --pending; continue; }
            }
            bool success = false;
            std::int64_t appId = 0;
            try {
                // Researched bundle alias; never infer Steam ids from MTS ids.
                if (key.second.find('+') != std::string::npos && normalize(key.second) == "cyberpunk2077dlc") {
                    appId = 1091500;
                    success = true;
                } else {
                    const auto response = downloader(key.second, cancelled);
                    if (response.status == 200) {
                        appId = matchedAppId(key.second, response.body);
                        success = true;
                    }
                }
            } catch (...) { /* Malformed replies use the transient cooldown. */ }
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (stopped) return;
                auto& record = records.at(key);
                record.pending = false;
                --pending;
                if (success) { record.appId = appId; record.saved = now(); record.failed = -1; }
                else record.failed = now();
            }
            if (success) save();
        }
    }
};
SteamArtworkResolver::SteamArtworkResolver(Downloader downloader, SteamArtworkOptions options)
    : impl_(std::make_unique<Impl>(std::move(downloader), std::move(options))) {}
SteamArtworkResolver::~SteamArtworkResolver() { stop(); }
bool SteamArtworkResolver::request(int mtsId, const std::string& title) {
    if (mtsId <= 0 || !validTitle(title)) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->stopped) return false;
    const Key key{mtsId, title};
    auto found = impl_->records.find(key);
    if (found != impl_->records.end()) {
        auto& record = found->second;
        record.used = ++impl_->sequence;
        if (record.pending || impl_->current(record, impl_->now()) ||
            fresh(record.failed, impl_->now(), errorCooldown)) return true;
    }
    if (impl_->pending >= impl_->options.maxPending) return false;
    if (found == impl_->records.end()) {
        if (!impl_->reserve()) return false;
        found = impl_->records.emplace(key, Impl::Record{}).first;
    }
    found->second.pending = true;
    found->second.used = ++impl_->sequence;
    impl_->queue.push_back(key);
    ++impl_->pending;
    impl_->wake.notify_one();
    return true;
}
std::string SteamArtworkResolver::get(int mtsId, const std::string& title) const {
    if (mtsId <= 0 || title.size() > maxTitleBytes) return {};
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->stopped) return {};
    const auto found = impl_->records.find(Key{mtsId, title});
    return found == impl_->records.end() ? std::string{} : headerUrl(found->second.appId);
}
SteamArtworkStats SteamArtworkResolver::stats() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return {impl_->records.size(), impl_->pending, impl_->ready};
}
void SteamArtworkResolver::stop() noexcept {
    if (!impl_) return;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->stopped = true;
        impl_->cancelled.store(true, std::memory_order_relaxed);
        impl_->queue.clear();
        impl_->pending = 0;
    }
    impl_->wake.notify_one();
    if (impl_->worker.joinable()) impl_->worker.join();
}
}