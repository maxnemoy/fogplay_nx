#include "features/settings/infrastructure/connection_settings_storage.hpp"
#include "core/storage/storage.hpp"
#include <nlohmann/json.hpp>
#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace fog::connection {
Settings loadSettings(const std::string& path) {
    recoverStorage(path);
    int flags = O_RDONLY;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
#ifdef O_NONBLOCK
    flags |= O_NONBLOCK;
#endif
    const int fd = open(path.c_str(), flags);
    if (fd < 0) {
        if (errno == ENOENT) return {};
        throw std::runtime_error("Cannot read connection settings");
    }
    struct stat info{};
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 1 || info.st_size > 1024) {
        close(fd);
        throw std::runtime_error("Invalid connection settings");
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
    if (failed) throw std::runtime_error("Cannot read connection settings");
    if (bytes.find('\0') != std::string::npos) throw std::runtime_error("Invalid connection settings");
    bool seenVersion = false, seenMode = false, duplicate = false;
    const auto data = nlohmann::json::parse(bytes,
        [&](int depth, nlohmann::json::parse_event_t event, nlohmann::json& parsed) {
            if (depth == 1 && event == nlohmann::json::parse_event_t::key) {
                const auto key = parsed.get<std::string>();
                if (key == "version") { duplicate |= seenVersion; seenVersion = true; }
                if (key == "mode") { duplicate |= seenMode; seenMode = true; }
            }
            return true;
        }, false);
    try {
        if (duplicate || !data.is_object() || data.size() != 2 ||
            !data.at("version").is_number_integer() || data.at("version") != 1 ||
            !data.at("mode").is_string()) throw std::runtime_error("invalid");
        const auto mode = data.at("mode").get<std::string>();
        if (mode == "normal") return {Mode::Normal};
        if (mode == "auto") return {Mode::Auto};
        if (mode == "relay") return {Mode::Relay};
        throw std::runtime_error("invalid");
    } catch (...) { throw std::runtime_error("Invalid connection settings"); }
}
void saveSettings(const std::string& path, const Settings& settings) {
    if (!valid(settings)) throw std::runtime_error("Cannot save invalid connection settings");
    const auto mode = settings.mode == Mode::Auto ? "auto" : settings.mode == Mode::Relay ? "relay" : "normal";
    const auto bytes = nlohmann::json{{"version", 1}, {"mode", mode}}.dump() + "\n";
    replaceStorage(path, bytes);
}
}
