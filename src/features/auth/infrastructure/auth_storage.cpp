// newlib hides descriptor truncation in strict C++ mode unless POSIX APIs are enabled.
#if defined(__SWITCH__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "features/auth/infrastructure/auth_storage.hpp"
#include "core/storage/storage.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __SWITCH__
extern "C" {
#include <switch/result.h>
#include <switch/runtime/devices/fs_dev.h>
}
#endif

namespace fog {
namespace {
constexpr std::size_t maxAuthBytes = 65536;
bool hex(const std::string& value, std::size_t size) {
    return value.size() == size && std::all_of(value.begin(), value.end(),
        [](unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
bool valid(const AuthCredentials& credentials) {
    return validAuthToken(credentials.token) && hex(credentials.account, 64) && hex(credentials.deviceId, 32);
}
int noFollow(int flags) {
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    // A malformed FIFO must not block the worker before fstat rejects it.
#ifdef O_NONBLOCK
    flags |= O_NONBLOCK;
#endif
    return flags;
}
}
bool validAuthToken(const std::string& token) noexcept {
    return !token.empty() && token.size() <= 8192 && std::all_of(token.begin(), token.end(),
        [](unsigned char c) { return c >= 32 && c != 127; });
}
std::optional<AuthCredentials> loadAuthCredentials(const std::string& path) {
    recoverStorage(path);
    const int fd = open(path.c_str(), noFollow(O_RDONLY));
    if (fd < 0) {
        if (errno == ENOENT) return std::nullopt;
        throw std::runtime_error("Cannot read saved sign-in");
    }
    struct stat info{};
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 1 ||
        info.st_size > static_cast<off_t>(maxAuthBytes)) {
        close(fd);
        throw std::runtime_error("Saved sign-in is invalid");
    }
    std::string bytes(static_cast<std::size_t>(info.st_size), '\0');
    std::size_t offset = 0;
    bool failed = false;
    while (offset < bytes.size()) {
        const auto count = read(fd, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { failed = true; break; }
        offset += static_cast<std::size_t>(count);
    }
    char extra = 0;
    ssize_t count;
    do { count = read(fd, &extra, 1); } while (count < 0 && errno == EINTR);
    if (count != 0) failed = true;
    if (close(fd) != 0) failed = true;
    if (failed) throw std::runtime_error("Cannot read saved sign-in");
    const auto data = nlohmann::json::parse(bytes, nullptr, false);
    std::fill(bytes.begin(), bytes.end(), '\0');
    try {
        if (!data.is_object() || data.size() != 4 || !data.at("version").is_number_integer() ||
            data.at("version") != 1) throw std::runtime_error("invalid");
        AuthCredentials credentials{data.at("token").get<std::string>(),
            data.at("account").get<std::string>(), data.at("deviceId").get<std::string>()};
        if (!valid(credentials)) throw std::runtime_error("invalid");
        return credentials;
    } catch (...) { throw std::runtime_error("Saved sign-in is invalid"); }
}
void saveAuthCredentials(const std::string& path, const AuthCredentials& credentials) {
    if (!valid(credentials)) throw std::runtime_error("Cannot save invalid sign-in");
    const auto bytes = nlohmann::json{{"version", 1}, {"token", credentials.token},
        {"account", credentials.account}, {"deviceId", credentials.deviceId}}.dump() + "\n";
    replaceStorage(path, bytes);
}
void removeAuthCredentials(const std::string& path) {
    // Explicit forget never restores credentials after uncertain removal.
    clearStorage(path);
}
}
