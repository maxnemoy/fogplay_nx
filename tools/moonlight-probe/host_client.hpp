#pragma once

#include <gamestream/client.h>
#include <atomic>
#include <string>
#include <vector>

namespace moonlight_probe {

inline constexpr const char* kConfigPath = "sdmc:/switch/fogplay_nx/moonlight-test.json";
inline constexpr const char* kIdentityPath = "sdmc:/switch/fogplay_nx/moonlight-test-identity";

struct ProbeConfig {
    std::string host;
    int appId = 0;
    int width = 1280;
    int height = 720;
    int fps = 60;
    int bitrateKbps = 10000;
};

struct HostResult {
    bool ok;
    int code;
    std::string message;
};

struct App {
    int id;
    std::string name;
};

// Single worker owns all methods except thread-safe cancel(). Keep alive until
// LiStopConnection() returns. No host operations during LiStart/StopConnection.
class HostClient final {
public:
    HostClient();
    ~HostClient();
    HostClient(const HostClient&) = delete;
    HostClient& operator=(const HostClient&) = delete;
    HostClient(HostClient&&) = delete;
    HostClient& operator=(HostClient&&) = delete;

    static HostResult loadConfig(ProbeConfig& config);
    HostResult initialize(const ProbeConfig& config);
    bool paired() const noexcept;
    HostResult pair(const std::string& pin);
    HostResult apps(std::vector<App>& result);
    HostResult launch(int appId);
    // Invoke on worker after active operation and LiStopConnection() finish.
    HostResult stopOwnedApp();
    void cancel() noexcept;

    SERVER_INFORMATION& serverInfo() noexcept { return server_.serverInfo; }
    STREAM_CONFIGURATION& streamConfig() noexcept { return stream_; }
    bool ownsApp() const noexcept { return ownedAppId_ != 0; }
    bool launchUncertain() const noexcept { return launchUncertain_; }

private:
    HostResult ready() const;
    HostResult refresh(SERVER_DATA& status);
    static HostResult validateConfig(const ProbeConfig& config);

    SERVER_DATA server_{};
    STREAM_CONFIGURATION stream_{};
    ProbeConfig config_;
    std::atomic<bool> cancelled_{false};
    bool initialized_ = false;
    bool launchAttempted_ = false;
    bool launchUncertain_ = false;
    int ownedAppId_ = 0;
};

} // namespace moonlight_probe
