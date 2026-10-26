// newlib hides POSIX descriptor APIs in strict C++ mode.
#if defined(__SWITCH__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "features/auth/infrastructure/foghub_cookie_storage.hpp"
#include "core/domain/repository_error.hpp"
#include "core/storage/storage.hpp"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace fog::foghub {
namespace {
constexpr std::size_t maxFileBytes = 128 * 1024;
constexpr std::size_t maxCookieCount = 256;
constexpr std::size_t maxCookieBytes = 4096;
constexpr std::size_t maxAccountBytes = 256;

[[noreturn]] void storageFailure() {
    throw RepositoryError(RepositoryErrorKind::StorageFailure, "FogHub saved sign-in storage failed");
}

bool valid(const SavedCookies& saved) {
    if (saved.account.size() <= 6 || saved.account.size() > maxAccountBytes ||
        saved.account.compare(0, 6, "login:") != 0 ||
        !std::all_of(saved.account.begin(), saved.account.end(),
            [](unsigned char c) { return c >= 32 && c != 127; }) ||
        saved.cookies.size() > maxCookieCount) return false;
    std::size_t total = saved.account.size();
    for (const auto& cookie : saved.cookies) {
        if (cookie.empty() || cookie.size() > maxCookieBytes ||
            cookie.find_first_of("\r\n") != std::string::npos ||
            cookie.find('\0') != std::string::npos) return false;
        total += cookie.size();
        if (total > maxFileBytes) return false;
    }
    return true;
}

int readFlags() {
    int flags = O_RDONLY;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
#ifdef O_NONBLOCK
    // Reject FIFOs with fstat without waiting for a writer.
    flags |= O_NONBLOCK;
#endif
    return flags;
}

class Descriptor {
public:
    explicit Descriptor(int fd) : fd_(fd) {}
    ~Descriptor() { if (fd_ >= 0) close(fd_); }
    int get() const { return fd_; }
    bool finish() {
        const int fd = fd_;
        fd_ = -1;
        return close(fd) == 0;
    }
private:
    int fd_;
};

std::optional<std::string> readSaved(const std::string& path) {
    recoverStorage(path);
    Descriptor fd(open(path.c_str(), readFlags()));
    if (fd.get() < 0) {
        if (errno == ENOENT) return std::nullopt;
        storageFailure();
    }
    struct stat info{};
    if (fstat(fd.get(), &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 1 ||
        info.st_size > static_cast<off_t>(maxFileBytes)) storageFailure();
    std::string bytes(static_cast<std::size_t>(info.st_size), '\0');
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = read(fd.get(), bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) storageFailure();
        offset += static_cast<std::size_t>(count);
    }
    char extra = 0;
    ssize_t count;
    do { count = read(fd.get(), &extra, 1); } while (count < 0 && errno == EINTR);
    if (count != 0 || !fd.finish()) storageFailure();
    return bytes;
}

class FileCookieStore final : public CookieStore {
public:
    explicit FileCookieStore(std::string path) : path_(std::move(path)) {
        if (path_.empty() || path_.find('\0') != std::string::npos) storageFailure();
    }

    std::optional<SavedCookies> load() override {
        try {
            const auto bytes = readSaved(path_);
            if (!bytes) return std::nullopt;
            // The schema has only an object, its cookie array, and scalar values.
            // Bound nesting before building a maliciously deep JSON tree.
            const auto data = nlohmann::json::parse(*bytes,
                [](int depth, nlohmann::json::parse_event_t, nlohmann::json&) {
                    if (depth > 2) storageFailure();
                    return true;
                }, false);
            if (!data.is_object() || data.size() != 4 ||
                !data.at("version").is_number_integer() || data.at("version") != 1 ||
                !data.at("provider").is_string() || data.at("provider") != "foghub" ||
                !data.at("account").is_string() || !data.at("cookies").is_array() ||
                data.at("cookies").size() > maxCookieCount) storageFailure();
            SavedCookies saved{data.at("account").get<std::string>(), {}};
            for (const auto& cookie : data.at("cookies")) {
                if (!cookie.is_string()) storageFailure();
                saved.cookies.push_back(cookie.get<std::string>());
            }
            if (!valid(saved)) storageFailure();
            return saved;
        } catch (...) { storageFailure(); }
    }

    void save(const SavedCookies& saved) override {
        try {
            if (!valid(saved)) storageFailure();
            const auto bytes = nlohmann::json{{"version", 1}, {"provider", "foghub"},
                {"account", saved.account}, {"cookies", saved.cookies}}.dump() + "\n";
            if (bytes.size() > maxFileBytes) storageFailure();
            replaceStorage(path_, bytes);
        } catch (...) { storageFailure(); }
    }

    void clear() override {
        try { clearStorage(path_); }
        catch (...) { storageFailure(); }
    }
private:
    const std::string path_;
};
}

std::unique_ptr<CookieStore> makeCookieStore(const std::string& path) {
    return std::make_unique<FileCookieStore>(path);
}

}
