#include "features/settings/infrastructure/video_settings_storage.hpp"
#include "core/storage/storage.hpp"
#include <nlohmann/json.hpp>
#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace fog::video {
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
        throw std::runtime_error("Cannot read video settings");
    }
    struct stat info{};
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 1 || info.st_size > 1024) {
        close(fd);
        throw std::runtime_error("Invalid video settings");
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
    if (failed) throw std::runtime_error("Cannot read video settings");
    const auto data = nlohmann::json::parse(bytes, nullptr, false);
    try {
        if (!data.is_object() || data.size() != 5 ||
            !data.at("version").is_number_integer() || data.at("version") != 1 ||
            !data.at("codec").is_string() || !data.at("bitrateMbps").is_number_integer() ||
            !data.at("keyframeSeconds").is_number_integer() || !data.at("quality").is_number_integer())
            throw std::runtime_error("invalid");
        const auto codec = data.at("codec").get<std::string>();
        if (codec != "H264" && codec != "H265") throw std::runtime_error("invalid");
        const auto& bitrate = data.at("bitrateMbps");
        const auto& interval = data.at("keyframeSeconds");
        const auto& quality = data.at("quality");
        if (std::none_of(Bitrates.begin(), Bitrates.end(), [&](int choice) { return bitrate == choice; }) ||
            std::none_of(KeyframeIntervals.begin(), KeyframeIntervals.end(), [&](int choice) { return interval == choice; }) ||
            quality < 0 || quality > 4) throw std::runtime_error("invalid");
        Settings value;
        value.codec = codec == "H265" ? ::stream::VideoCodec::H265 : ::stream::VideoCodec::H264;
        value.bitrateMbps = data.at("bitrateMbps").get<int>();
        value.keyframeSeconds = data.at("keyframeSeconds").get<int>();
        value.quality = data.at("quality").get<int>();
        if (!valid(value)) throw std::runtime_error("invalid");
        return value;
    } catch (...) { throw std::runtime_error("Invalid video settings"); }
}
void saveSettings(const std::string& path, const Settings& value) {
    if (!valid(value)) throw std::runtime_error("Cannot save invalid video settings");
    const auto bytes = nlohmann::json{{"version", 1},
        {"codec", value.codec == ::stream::VideoCodec::H265 ? "H265" : "H264"},
        {"bitrateMbps", value.bitrateMbps}, {"keyframeSeconds", value.keyframeSeconds},
        {"quality", value.quality}}.dump() + "\n";
    replaceStorage(path, bytes);
}
}
