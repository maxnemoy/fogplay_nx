#include "features/settings/infrastructure/connection_settings_storage.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
namespace {
int assertions = 0;
void require(bool value, const char* message) {
    ++assertions;
    if (!value) throw std::runtime_error(message);
}
bool rejects(const std::string& path) {
    try { fog::connection::loadSettings(path); return false; } catch (...) { return true; }
}
}
int main() {
    using namespace fog::connection;
    using namespace std::chrono;
    const auto dir = std::filesystem::temp_directory_path() / ("fog-connection-settings-" + std::to_string(getpid()));
    std::filesystem::create_directory(dir);
    const auto path = (dir/"connection-settings.json").string();
    try {
        require(loadSettings(path).mode == Mode::Normal, "missing settings preserve existing all policy");
        for (const auto mode : {Mode::Normal, Mode::Auto, Mode::Relay}) {
            saveSettings(path, {mode});
            require(loadSettings(path).mode == mode, "saved mode survives restart");
        }
        for (const auto* bytes : {"", "{", "null", "[]", "{}",
             "{\"version\":1,\"mode\":\"bad\"}", "{\"version\":1,\"mode\":1}",
             "{\"version\":1.0,\"mode\":\"normal\"}", "{\"version\":2,\"mode\":\"normal\"}",
             "{\"version\":1,\"mode\":\"normal\",\"extra\":true}",
             "{\"version\":1,\"mode\":\"normal\",\"mode\":\"relay\"}",
             "{\"version\":1,\"version\":1,\"mode\":\"normal\"}"}) {
            std::ofstream(path) << bytes;
            require(rejects(path), "malformed unknown or duplicate settings rejected");
        }
        std::ofstream(path) << std::string(1025, ' ');
        require(rejects(path), "storage bounded");
        std::string nul = "{\"version\":1,\"mode\":\"normal\"}"; nul += '\0';
        std::ofstream(path) << nul;
        require(rejects(path), "NUL suffix rejected");
        std::filesystem::remove(path);
        std::filesystem::create_directory(path);
        require(rejects(path), "directory rejected");
        std::filesystem::remove(path);
        const auto target = (dir/"target").string();
        saveSettings(target, {Mode::Auto});
        std::filesystem::create_symlink(target,path);
        require(rejects(path), "symlink rejected");
        std::filesystem::remove(path);
        saveSettings(path+".bak", {Mode::Relay});
        require(loadSettings(path).mode == Mode::Relay && !std::filesystem::exists(path+".bak"), "backup recovered");
        bool failed = false;
        try { saveSettings(path, {static_cast<Mode>(99)}); } catch (...) { failed = true; }
        require(failed && loadSettings(path).mode == Mode::Relay, "invalid save preserves previous settings");
        failed = false;
        try { saveSettings((dir/"missing"/"file").string(), {}); } catch (...) { failed = true; }
        require(failed, "storage failure observable");
        Settings setting;
        require(adjust(setting, -1) && setting.mode == Mode::Relay, "backward mode wraps");
        require(adjust(setting, 1) && setting.mode == Mode::Normal, "forward mode wraps");
        require(adjust(setting, 1) && setting.mode == Mode::Auto, "auto selectable");
        require(!adjust(setting, 0), "zero adjustment ignored");
        const RelaySelection::Time start{};
        RelaySelection policy;
        require(!policy.relay(), "default policy all");
        policy.reset({Mode::Relay});
        require(policy.relay(), "always TURN before initial start");
        policy.reset({Mode::Auto});
        require(!policy.relay() && !policy.broken(start), "initial connection failures do not count as breaks");
        policy.connected();
        require(policy.broken(start) && !policy.relay() && policy.breaks() == 1, "first actual break stays all");
        for (int attempt=1; attempt<5; ++attempt)
            require(!policy.broken(start+seconds(attempt)) && policy.breaks() == 1, "failed retries remain one outage");
        policy.connected();
        require(policy.broken(start+seconds(119)) && policy.relay(), "second break inside two-minute window selects TURN");
        policy.connected();
        require(policy.broken(start+seconds(600)) && policy.relay(), "relay retained even after window expires");
        policy.reset({Mode::Auto});
        require(!policy.relay(), "new rental clears relay latch");
        policy.connected(); policy.broken(start);
        policy.connected(); policy.broken(start+seconds(120));
        require(!policy.relay() && policy.breaks() == 1, "old break expires exactly at window boundary");
        policy.connected(); policy.broken(start+seconds(122));
        require(policy.relay(), "two recent independent outages select TURN");
        policy.reset();
        for (int i=0;i<4;++i) { policy.connected(); policy.broken(start+seconds(i)); }
        require(!policy.relay(), "normal retains all policy despite repeated breaks");
        std::filesystem::remove_all(dir);
        std::cout << "Connection settings tests passed: " << assertions << " assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::filesystem::remove_all(dir);
        std::cerr << "Connection settings tests failed: " << error.what() << '\n';
        return 1;
    }
}
