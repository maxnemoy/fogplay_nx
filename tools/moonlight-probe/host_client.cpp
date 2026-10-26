#include "host_client.hpp"

#include <Settings.hpp>
#include <gamestream/errors.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>

namespace moonlight_probe {
namespace {

HostResult success(const std::string& message) { return {true, GS_OK, message}; }

HostResult failure(int code, const std::string& message) {
    // Never expose gs_error(), raw URLs, PINs, certificates or input keys.
    return {false, code, message + " (code " + std::to_string(code) + ")"};
}

bool privateIpv4(const std::string& address) {
    unsigned octets[4]{};
    std::size_t pos = 0;
    for (unsigned i = 0; i < 4; ++i) {
        const auto start = pos;
        while (pos < address.size() && address[pos] >= '0' && address[pos] <= '9') {
            octets[i] = octets[i] * 10 + unsigned(address[pos++] - '0');
            if (octets[i] > 255 || pos - start > 3) return false;
        }
        if (start == pos || (pos - start > 1 && address[start] == '0')) return false;
        if (i < 3) {
            if (pos == address.size() || address[pos++] != '.') return false;
        } else if (pos != address.size()) return false;
    }
    return octets[0] == 10 ||
           (octets[0] == 172 && octets[1] >= 16 && octets[1] <= 31) ||
           (octets[0] == 192 && octets[1] == 168);
}

void freeAppList(APP_LIST* list) {
    while (list) {
        auto* next = list->next;
        std::free(list->name);
        std::free(list);
        list = next;
    }
}

} // namespace

HostClient::HostClient() {
    LiInitializeServerInformation(&server_.serverInfo);
    LiInitializeStreamConfiguration(&stream_);
    Settings::instance().reset_cancel();
}

HostClient::~HostClient() {
    // Caller explicitly stops its connection and owned app on the worker.
    // Destruction performs no network work.
    delete[] server_.serverInfo.rtspSessionUrl;
}

HostResult HostClient::validateConfig(const ProbeConfig& config) {
    if (!privateIpv4(config.host))
        return failure(GS_INVALID, "host must be a private LAN IPv4 address without a port");
    if (config.appId < 0 || config.width < 640 || config.width > 1920 ||
        config.height < 360 || config.height > 1080 || config.width % 2 != 0 ||
        config.height % 2 != 0 || (config.fps != 30 && config.fps != 60) ||
        config.bitrateKbps < 1000 || config.bitrateKbps > 30000)
        return failure(GS_INVALID, "Invalid probe settings: even 640-1920 x 360-1080 dimensions, 30/60 fps, 1000-30000 kbps and nonnegative app_id required");
    return success("Probe settings valid");
}

HostResult HostClient::loadConfig(ProbeConfig& config) {
    std::ifstream file(kConfigPath, std::ios::binary);
    if (!file) return failure(GS_IO_ERROR, "Create switch/fogplay_nx/moonlight-test.json on SD card");
    std::string contents(4097, '\0');
    file.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    contents.resize(static_cast<std::size_t>(file.gcount()));
    if (file.bad() || contents.size() > 4096)
        return failure(GS_INVALID, "Probe config must be readable and at most 4096 bytes");
    try {
        const auto json = nlohmann::json::parse(contents);
        if (!json.is_object() || !json.contains("host") || !json["host"].is_string())
            return failure(GS_INVALID, "Probe config requires a string host");
        ProbeConfig parsed;
        parsed.host = json["host"].get<std::string>();
        for (auto it = json.begin(); it != json.end(); ++it) {
            if (it.key() == "host") continue;
            if (it.key() != "app_id" && it.key() != "width" && it.key() != "height" &&
                it.key() != "fps" && it.key() != "bitrate_kbps")
                return failure(GS_INVALID, "Unknown field in probe config");
            if (!it.value().is_number_integer())
                return failure(GS_INVALID, "Probe numeric settings must be integers");
            // Preserve the full signed-int application ID range. Bound before
            // narrowing, including JSON unsigned integers above INT64_MAX.
            if (it.value().is_number_unsigned() && it.value().get<std::uint64_t>() > 2147483647)
                return failure(GS_INVALID, "Probe numeric setting out of range");
            const auto number = it.value().get<std::int64_t>();
            if (number < 0 || number > 2147483647)
                return failure(GS_INVALID, "Probe numeric setting out of range");
            const int value = static_cast<int>(number);
            if (it.key() == "app_id") parsed.appId = value;
            else if (it.key() == "width") parsed.width = value;
            else if (it.key() == "height") parsed.height = value;
            else if (it.key() == "fps") parsed.fps = value;
            else parsed.bitrateKbps = value;
        }
        auto result = validateConfig(parsed);
        if (result.ok) config = parsed;
        return result;
    } catch (const std::exception&) {
        return failure(GS_INVALID, "Invalid probe JSON config");
    }
}

HostResult HostClient::initialize(const ProbeConfig& config) {
    if (initialized_ || launchAttempted_) return failure(GS_WRONG_STATE, "Probe host already initialized");
    if (cancelled_.load()) return failure(GS_WRONG_STATE, "Probe cancelled");
    auto valid = validateConfig(config);
    if (!valid.ok) return valid;
    std::error_code error;
    std::filesystem::create_directories(kIdentityPath, error);
    if (error) return failure(GS_IO_ERROR, "Cannot create probe identity folder on SD card");
    config_ = config;
    Settings::instance().set_key_dir(kIdentityPath);
    const int code = gs_init(&server_, config_.host);
    if (code != GS_OK) return failure(code, "Cannot initialize local Moonlight host");
    if (server_.currentGame != 0)
        return failure(GS_WRONG_STATE, "Host has a running app. Close its session on Sunshine before this probe");
    initialized_ = true;
    stream_.width = config.width;
    stream_.height = config.height;
    stream_.fps = config.fps;
    stream_.bitrate = config.bitrateKbps;
    stream_.packetSize = 1392;
    stream_.streamingRemotely = STREAM_CFG_LOCAL;
    stream_.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    stream_.supportedVideoFormats = VIDEO_FORMAT_H264;
    stream_.clientRefreshRateX100 = config.fps * 100;
    stream_.colorSpace = COLORSPACE_REC_709;
    stream_.colorRange = COLOR_RANGE_LIMITED;
    return success(server_.paired ? "Local host paired" : "Local host requires pairing");
}

bool HostClient::paired() const noexcept { return initialized_ && server_.paired; }

HostResult HostClient::ready() const {
    if (cancelled_.load()) return failure(GS_WRONG_STATE, "Probe cancelled");
    if (!initialized_) return failure(GS_WRONG_STATE, "Initialize local host first");
    if (launchUncertain_)
        return failure(GS_WRONG_STATE, "Launch outcome unknown. Close app manually on Sunshine before restarting probe");
    return success("Host ready");
}

HostResult HostClient::refresh(SERVER_DATA& status) {
    const int code = gs_init(&status, config_.host);
    if (code != GS_OK) return failure(code, "Cannot refresh local host status");
    return success("Host status refreshed");
}

HostResult HostClient::pair(const std::string& pin) {
    auto result = ready();
    if (!result.ok) return result;
    if (pin.size() != 4 || pin.find_first_not_of("0123456789") != std::string::npos)
        return failure(GS_INVALID, "Pairing PIN must contain exactly four digits");
    if (server_.paired) return success("Local host already paired");
    SERVER_DATA status{};
    result = refresh(status);
    if (!result.ok) return result;
    if (status.currentGame != 0)
        return failure(GS_WRONG_STATE, "Close running app on Sunshine before pairing");
    char mutablePin[5]{pin[0], pin[1], pin[2], pin[3], '\0'};
    const int code = gs_pair(&server_, mutablePin);
    for (volatile char& digit : mutablePin) digit = 0;
    if (code != GS_OK) return failure(code, "Local host pairing failed");
    return success("Local host paired");
}

HostResult HostClient::apps(std::vector<App>& result) {
    auto status = ready();
    if (!status.ok) return status;
    if (!server_.paired) return failure(GS_WRONG_STATE, "Pair local host first");
    APP_LIST* raw = nullptr;
    const int code = gs_applist(&server_, &raw);
    std::unique_ptr<APP_LIST, decltype(&freeAppList)> list(raw, freeAppList);
    if (code != GS_OK) return failure(code, "Cannot read local host app list");
    std::vector<App> parsed;
    for (auto* app = raw; app; app = app->next)
        if (app->id > 0 && app->name) parsed.push_back({app->id, app->name});
    result = std::move(parsed);
    return success("Local host app list loaded");
}

HostResult HostClient::launch(int appId) {
    auto result = ready();
    if (!result.ok) return result;
    if (!server_.paired) return failure(GS_WRONG_STATE, "Pair local host first");
    if (launchAttempted_) return failure(GS_WRONG_STATE, "Only one launch attempt allowed per probe");
    if (appId <= 0) return failure(GS_INVALID, "Select Desktop or valid app_id before launch");
    SERVER_DATA status{};
    result = refresh(status);
    if (!result.ok) return result;
    if (status.currentGame != 0)
        return failure(GS_WRONG_STATE, "Host has a running app. Close its session on Sunshine before this probe");
    if (cancelled_.load()) return failure(GS_WRONG_STATE, "Probe cancelled");
    // gs_start_app otherwise silently resumes an existing app. Refreshed idle
    // status is mandatory. Stable server_/stream_ remain live for Limelight.
    server_.currentGame = 0;
    launchAttempted_ = true;
    const int code = gs_start_app(&server_, &stream_, appId, false, false, 1);
    if (code != GS_OK) {
        server_.currentGame = 0;
        launchUncertain_ = true;
        return failure(code, "Launch outcome unknown. Close app manually on Sunshine; do not retry");
    }
    ownedAppId_ = appId;
    if (!server_.serverInfo.rtspSessionUrl || !*server_.serverInfo.rtspSessionUrl)
        return failure(GS_INVALID, "Host acknowledged launch without RTSP URL; owned app requires cleanup");
    return success("Local host app launched");
}

void HostClient::cancel() noexcept {
    cancelled_.store(true);
    Settings::instance().request_cancel();
}

HostResult HostClient::stopOwnedApp() {
    if (launchUncertain_)
        return failure(GS_WRONG_STATE, "Launch outcome unknown. Close app manually on Sunshine");
    if (!ownedAppId_) return success("No probe-owned host app to stop");
    // Caller has joined active work and stopped Limelight. Cancellation gets
    // reset only here; refresh and cancel share a five-second total deadline.
    Settings::instance().reset_cancel();
    Settings::instance().set_timeout_ms(5000);
    SERVER_DATA status{};
    auto result = refresh(status);
    if (!result.ok)
        return failure(result.code, "Cannot confirm owned app. Close app manually on Sunshine");
    if (status.currentGame == 0) {
        ownedAppId_ = 0;
        return success("Probe-owned app already stopped");
    }
    if (status.currentGame != ownedAppId_) {
        ownedAppId_ = 0;
        return failure(GS_WRONG_STATE, "Host app changed. Probe did not cancel other session");
    }
    const int code = gs_quit_app(&status);
    if (code != GS_OK)
        return failure(code, "Host stop not acknowledged. Close app manually on Sunshine");
    ownedAppId_ = 0;
    return success("Probe-owned host app stop acknowledged");
}

} // namespace moonlight_probe
