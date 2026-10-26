#include "../host_client.hpp"

#include <Settings.hpp>
#include <gamestream/errors.h>

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>

namespace {
int currentApp = 0;
int initCode = GS_OK;
int launchCode = GS_OK;
int quitCode = GS_OK;
int launches = 0;
int quits = 0;
int pairings = 0;
bool isPaired = true;
bool provideRtsp = true;

void reset() {
    currentApp = 0;
    initCode = launchCode = quitCode = GS_OK;
    launches = quits = pairings = 0;
    isPaired = provideRtsp = true;
}

void writeConfig(const std::string& value) {
    std::filesystem::create_directories("sdmc:/switch/fogplay_nx");
    std::ofstream file(moonlight_probe::kConfigPath);
    file << value;
}

moonlight_probe::ProbeConfig config() {
    moonlight_probe::ProbeConfig value;
    value.host = "192.168.1.42";
    return value;
}
} // namespace

// No network or identity crypto in this deterministic host-lifecycle harness.
void LiInitializeServerInformation(PSERVER_INFORMATION value) { std::memset(value, 0, sizeof(*value)); }
void LiInitializeStreamConfiguration(PSTREAM_CONFIGURATION value) { std::memset(value, 0, sizeof(*value)); }

int gs_init(PSERVER_DATA server, const std::string address) {
    server->address = address;
    server->serverInfo.address = server->address.c_str();
    server->serverInfoAppVersion = "7.1.0.0";
    server->serverInfo.serverInfoAppVersion = server->serverInfoAppVersion.c_str();
    server->paired = isPaired;
    server->currentGame = currentApp;
    return initCode;
}

int gs_pair(PSERVER_DATA server, char* pin) {
    assert(std::strcmp(pin, "1234") == 0);
    ++pairings;
    server->paired = isPaired = true;
    return GS_OK;
}

int gs_applist(PSERVER_DATA, PAPP_LIST* out) {
    *out = static_cast<APP_LIST*>(std::calloc(1, sizeof(APP_LIST)));
    (*out)->name = ::strdup("Desktop");
    (*out)->id = 42;
    return GS_OK;
}

int gs_start_app(PSERVER_DATA server, PSTREAM_CONFIGURATION stream, int appId, bool sops, bool localAudio, int gamepadMask) {
    assert(server->currentGame == 0); // Never permit upstream /resume branch.
    assert(stream->supportedVideoFormats == VIDEO_FORMAT_H264);
    assert(!sops && !localAudio && gamepadMask == 1);
    ++launches;
    server->currentGame = currentApp = appId; // Even failed response may mutate upstream state.
    if (provideRtsp) {
        server->serverInfo.rtspSessionUrl = new char[20];
        std::strcpy(const_cast<char*>(server->serverInfo.rtspSessionUrl), "rtsp://192.168.1.42");
    }
    return launchCode;
}

int gs_quit_app(PSERVER_DATA server) {
    assert(!Settings::instance().request_cancelled());
    assert(server->currentGame == currentApp);
    ++quits;
    if (quitCode == GS_OK) currentApp = 0;
    return quitCode;
}

int main() {
    using moonlight_probe::HostClient;
    const auto original = std::filesystem::current_path();
    char temporary[] = "/tmp/fogplay-moonlight-host-test-XXXXXX";
    const char* directory = ::mkdtemp(temporary);
    assert(directory);
    std::filesystem::current_path(directory);

    // Strict fixed-location config, RFC1918 only, range/type/unknown-key checks.
    moonlight_probe::ProbeConfig parsed;
    assert(!HostClient::loadConfig(parsed).ok);
    writeConfig(R"({"host":"192.168.1.42","app_id":2147483647})");
    assert(HostClient::loadConfig(parsed).ok && parsed.appId == 2147483647);
    for (const auto* invalid : {
        R"({"host":"8.8.8.8"})", R"({"host":"127.0.0.1"})",
        R"({"host":"192.168.1.1:47989"})", R"({"host":"192.168.01.1"})",
        R"({"host":"172.32.0.1"})", R"({"host":"192.168.1.42","fps":30.5})",
        R"({"host":"192.168.1.42","app_id":18446744073709551615})",
        R"({"host":"192.168.1.42","password":"must-not-appear-in-error"})",
        R"({"host":"192.168.1.42","width":1921})", R"({"host":"192.168.1.42","fps":120})"
    }) {
        writeConfig(invalid);
        auto result = HostClient::loadConfig(parsed);
        assert(!result.ok && result.message.find("must-not-appear") == std::string::npos);
    }
    writeConfig(std::string(4097, ' '));
    assert(!HostClient::loadConfig(parsed).ok);

    reset();
    {
        HostClient host;
        currentApp = 11;
        assert(!host.initialize(config()).ok);
        assert(!host.launch(42).ok);
        assert(host.stopOwnedApp().ok && quits == 0 && launches == 0);
    }
    reset();
    {
        HostClient host;
        assert(host.initialize(config()).ok);
        auto* info = &host.serverInfo();
        auto* stream = &host.streamConfig();
        std::vector<moonlight_probe::App> list;
        assert(host.apps(list).ok && list.size() == 1 && list[0].id == 42);
        assert(host.launch(42).ok && host.ownsApp());
        const auto* url = host.serverInfo().rtspSessionUrl;
        assert(!host.launch(42).ok && launches == 1);
        host.cancel();
        assert(host.stopOwnedApp().ok && quits == 1 && !host.ownsApp());
        assert(info == &host.serverInfo() && stream == &host.streamConfig());
        assert(url == host.serverInfo().rtspSessionUrl);
        assert(host.stopOwnedApp().ok && quits == 1);
    }
    reset();
    {
        HostClient host;
        assert(host.initialize(config()).ok);
        currentApp = 99; // Host becomes occupied between init and launch.
        assert(!host.launch(42).ok && launches == 0);
        assert(host.stopOwnedApp().ok && quits == 0);
    }
    reset();
    {
        HostClient host;
        assert(host.initialize(config()).ok);
        launchCode = GS_IO_ERROR;
        assert(!host.launch(42).ok && host.launchUncertain() && !host.ownsApp());
        assert(!host.launch(42).ok && launches == 1);
        assert(!host.stopOwnedApp().ok && quits == 0);
    }
    reset();
    {
        HostClient host;
        assert(host.initialize(config()).ok && host.launch(42).ok);
        currentApp = 99; // Never cancel a replacement app.
        assert(!host.stopOwnedApp().ok && quits == 0 && !host.ownsApp());
    }
    reset();
    {
        HostClient host;
        assert(host.initialize(config()).ok && host.launch(42).ok);
        initCode = GS_IO_ERROR; // No blind cancellation when status refresh fails.
        assert(!host.stopOwnedApp().ok && quits == 0 && host.ownsApp());
    }
    reset();
    {
        HostClient host;
        assert(host.initialize(config()).ok);
        provideRtsp = false;
        assert(!host.launch(42).ok && host.ownsApp());
        assert(host.stopOwnedApp().ok && quits == 1);
    }
    reset();
    {
        HostClient host;
        assert(host.initialize(config()).ok && host.launch(42).ok);
        quitCode = GS_IO_ERROR;
        auto stopped = host.stopOwnedApp();
        assert(!stopped.ok && host.ownsApp());
        assert(stopped.message.find("not acknowledged") != std::string::npos);
    }
    reset();
    {
        HostClient host;
        isPaired = false;
        assert(host.initialize(config()).ok && !host.paired());
        assert(!host.pair("123").ok && !host.pair("12a4").ok && pairings == 0);
        assert(host.pair("1234").ok && host.paired() && pairings == 1);
    }
    reset();
    {
        HostClient host;
        host.cancel();
        assert(!host.initialize(config()).ok);
    }

    std::filesystem::current_path(original);
    std::filesystem::remove_all(directory);
    std::cout << "Moonlight host adapter config/lifecycle tests passed\n";
}
