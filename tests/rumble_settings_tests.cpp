#include "features/settings/infrastructure/rumble_settings_storage.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
namespace {
int assertions = 0;
void require(bool value, const char* message) {
    ++assertions;
    if (!value) throw std::runtime_error(message);
}
bool rejects(const std::string& path) {
    try { fog::rumble::loadSettings(path); return false; } catch (...) { return true; }
}
}
int main() {
    using namespace fog::rumble;
    const auto dir = std::filesystem::temp_directory_path() / ("fog-rumble-settings-" + std::to_string(getpid()));
    std::filesystem::create_directory(dir);
    const auto path = (dir/"rumble-settings.json").string();
    try {
        require(loadSettings(path).mode == Mode::Balanced, "missing settings preserve tested balanced default");
        for (const auto mode : {Mode::Balanced, Mode::DualBand, Mode::Chromium, Mode::Disabled}) {
            saveSettings(path, {mode});
            require(loadSettings(path).mode == mode, "all saved modes including disabled survive restart");
        }
        for (const auto* bytes : {"", "{", "null", "[]", "{}",
             "{\"version\":1,\"mode\":\"bad\"}", "{\"version\":1,\"mode\":1}",
             "{\"version\":1.0,\"mode\":\"balanced\"}", "{\"version\":2,\"mode\":\"balanced\"}",
             "{\"version\":true,\"mode\":\"balanced\"}",
             "{\"version\":1,\"mode\":\"balanced\",\"extra\":true}",
             "{\"version\":1,\"mode\":\"balanced\",\"mode\":\"disabled\"}",
             "{\"version\":1,\"version\":1,\"mode\":\"balanced\"}"}) {
            std::ofstream(path) << bytes;
            require(rejects(path), "malformed unknown or duplicate settings rejected");
        }
        std::ofstream(path) << std::string(1025, ' ');
        require(rejects(path), "storage bounded");
        std::string nul = "{\"version\":1,\"mode\":\"balanced\"}"; nul += '\0';
        std::ofstream(path) << nul;
        require(rejects(path), "NUL suffix rejected");
        std::filesystem::remove(path);
        std::filesystem::create_directory(path);
        require(rejects(path), "directory rejected");
        std::filesystem::remove(path);
        require(mkfifo(path.c_str(), 0600) == 0, "FIFO created");
        require(rejects(path), "FIFO rejected without blocking");
        std::filesystem::remove(path);
        const auto target = (dir/"target").string();
        saveSettings(target, {Mode::Chromium});
        std::filesystem::create_symlink(target,path);
        require(rejects(path), "symlink rejected");
        std::filesystem::remove(path);
        saveSettings(path+".bak", {Mode::Disabled});
        require(loadSettings(path).mode == Mode::Disabled && !std::filesystem::exists(path+".bak"), "backup recovered preserving disabled mode");
        bool failed = false;
        try { saveSettings(path, {static_cast<Mode>(99)}); } catch (...) { failed = true; }
        require(failed && loadSettings(path).mode == Mode::Disabled, "invalid save preserves previous settings");
        failed = false;
        try { saveSettings((dir/"missing"/"file").string(), {}); } catch (...) { failed = true; }
        require(failed, "storage failure observable");
        Settings setting;
        require(adjust(setting, -1) && setting.mode == Mode::Disabled, "backward cycle wraps to disabled");
        require(adjust(setting, 1) && setting.mode == Mode::Balanced, "forward cycle wraps to balanced");
        require(adjust(setting, 1) && setting.mode == Mode::DualBand, "dual band selectable");
        require(adjust(setting, 1) && setting.mode == Mode::Chromium, "Chromium selectable");
        require(adjust(setting, 1) && setting.mode == Mode::Disabled, "disabled selectable");
        require(!adjust(setting, 0) && setting.mode == Mode::Disabled, "zero adjustment ignored");
        setting.mode = static_cast<Mode>(99);
        require(!adjust(setting, 1), "invalid enum adjustment rejected");
        std::filesystem::remove_all(dir);
        std::cout << "Rumble settings tests passed: " << assertions << " assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::filesystem::remove_all(dir);
        std::cerr << "Rumble settings tests failed: " << error.what() << '\n';
        return 1;
    }
}
