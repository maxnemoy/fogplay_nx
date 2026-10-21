#include "features/settings/domain/controller_layout.hpp"
#include "features/settings/infrastructure/controller_layout_storage.hpp"
#include <nlohmann/json.hpp>
#include <array>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace {
int assertions = 0;
bool failInstall = false;
unsigned failFlushAt = 0;
std::string installPath;
std::string growingReadPath;
enum class ReadFault { None, Interrupt, Partial, Failure, Truncate, Grow };
ReadFault readFault = ReadFault::None;
void require(bool value, const char* message) {
    ++assertions;
    if (!value) throw std::runtime_error(message);
}
void writeFile(const std::string& path, const std::string& bytes) {
    std::ofstream(path, std::ios::binary) << bytes;
}
std::string readFile(const std::string& path) {
    std::ifstream input(path);
    return {(std::istreambuf_iterator<char>(input)), {}};
}
bool rejectsLoad(const std::string& path) {
    try { fog::controller::loadSettings(path); return false; }
    catch (const std::runtime_error& error) {
        const std::string message = error.what();
        require(message.find(path) == std::string::npos && message.find("private") == std::string::npos,
            "load errors must not reveal file paths or payloads");
        return true;
    }
}
bool rejectsSave(const std::string& path, const fog::controller::Settings& settings) {
    try { fog::controller::saveSettings(path, settings); return false; }
    catch (const std::runtime_error&) { return true; }
}
void checkMappings() {
    using namespace fog::controller;
    constexpr std::uint64_t a = 1ULL, b = 1ULL << 7, x = 1ULL << 16, y = 1ULL << 33;
    constexpr FaceButtonMasks masks{a, b, x, y};
    constexpr std::uint64_t untouched = ~(a | b | x | y);
    constexpr std::array<std::uint64_t, 16> physical{
        0, a, b, a | b, x, a | x, b | x, a | b | x,
        y, a | y, b | y, a | b | y, x | y, a | x | y, b | x | y, a | b | x | y};
    constexpr std::array<std::uint64_t, 16> swapped{
        0, b, a, a | b, y, b | y, a | y, a | b | y,
        x, b | x, a | x, a | b | x, x | y, b | x | y, a | x | y, a | b | x | y};
    for (std::size_t i = 0; i < physical.size(); ++i) {
        for (const auto extra : {std::uint64_t{0}, untouched}) {
            const auto input = physical[i] | extra;
            const auto expected = swapped[i] | extra;
            require(mapUiButtons(input, {true}, masks) == expected, "Xbox UI swaps both pairs and chords");
            require(mapStreamButtons(input, {true}, masks) == input, "Xbox stream preserves existing protocol input");
            require(mapUiButtons(input, {false}, masks) == input, "Nintendo UI preserves physical labels");
            require(mapStreamButtons(input, {false}, masks) == expected, "Nintendo stream swaps both pairs and chords");
        }
    }
    require(mapStreamButtons(a, {}, masks) == a, "default preserves current remote mapping");
    require(mapUiButtons(a, {}, masks) == b, "default switches UI confirm to physical B");
    require(uiHints("A B X Y", {true}) == "B A Y X", "hints swap simultaneously without cascading");
    require(uiHints("A B X Y", {false}) == "A B X Y", "Nintendo hint labels unchanged");
    require(uiHints("(A) [B] X/Y + -", {true}) == "(B) [A] Y/X + -", "punctuation delimits button hints");
    require(uiHints("AB XY Xbox BAY X_A A1 2B a b x y", {true}) ==
        "AB XY Xbox BAY X_A A1 2B a b x y", "words and lowercase text unchanged");
    require(uiHints("A Принять  B Назад  X Сброс  Y Поиск", {true}) ==
        "B Принять  A Назад  Y Сброс  X Поиск", "Cyrillic hint descriptions unchanged");
    require(uiHints("А В Х У AБ БB XЯ ЯY", {true}) == "А В Х У AБ БB XЯ ЯY",
        "Cyrillic letters and embedded ASCII letters unchanged");
    require(uiHints("", {}) == "", "empty hints safe");
}
void checkPersistence(const std::string& path) {
    using namespace fog::controller;
    require(loadSettings(path).xbox, "missing settings default to Xbox");
    require(!std::filesystem::exists(path), "loading defaults does not write a file");
    for (const bool xbox : {false, true, false}) {
        saveSettings(path, {xbox});
        require(loadSettings(path) == Settings{xbox}, "both layouts survive save and restart");
        const auto data = nlohmann::json::parse(readFile(path));
        require(data.size() == 2 && data.at("version") == 1 && data.at("xbox").is_boolean() &&
            data.at("xbox") == xbox, "saved schema is bounded and authentication-free");
        require(!std::filesystem::exists(path + ".bak") && !std::filesystem::exists(path + ".tmp"),
            "successful save removes auxiliary files");
    }
    const std::string valid = "{\"version\":1,\"xbox\":false}";
    const std::array<std::string, 24> invalid{
        "", "{", "null", "[]", "{}", "true", "{\"version\":1}", "{\"xbox\":false}",
        "{\"version\":2,\"xbox\":false}", "{\"version\":1.0,\"xbox\":false}",
        "{\"version\":true,\"xbox\":false}", "{\"version\":4294967297,\"xbox\":false}",
        "{\"version\":1,\"xbox\":0}", "{\"version\":1,\"xbox\":\"false\"}",
        "{\"version\":1,\"xbox\":null}", "{\"version\":1,\"xbox\":[]}",
        "{\"version\":1,\"xbox\":{}}", "{\"version\":1,\"xbox\":false,\"token\":\"private\"}",
        "{\"version\":1,\"xbox\":false,\"xbox\":true}",
        "{\"version\":1,\"version\":1,\"xbox\":false}",
        valid + "{}", valid + std::string(1, '\0'), std::string(1025, ' '),
        "{\"version\":1,\"xbox\":false,\"private\":\"" + std::string(1, static_cast<char>(0xff)) + "\"}"};
    for (const auto& bytes : invalid) {
        writeFile(path, bytes);
        Settings retained;
        try { retained = loadSettings(path); } catch (const std::runtime_error&) {}
        require(retained.xbox, "invalid load retains caller's safe default");
        require(rejectsLoad(path), "malformed or unsupported schema rejected");
    }
    writeFile(path, valid + std::string(1024 - valid.size(), ' '));
    require(!loadSettings(path).xbox, "maximum bounded payload accepted");
    writeFile(path, valid);
    readFault = ReadFault::Interrupt;
    require(!loadSettings(path).xbox, "interrupted read retries");
    readFault = ReadFault::Partial;
    require(!loadSettings(path).xbox, "partial reads complete the payload");
    readFault = ReadFault::None;
    growingReadPath = path;
    for (const auto fault : {ReadFault::Failure, ReadFault::Truncate, ReadFault::Grow}) {
        readFault = fault;
        const auto rejected = rejectsLoad(path);
        readFault = ReadFault::None;
        require(rejected, fault == ReadFault::Failure ? "read failure rejected" :
            fault == ReadFault::Truncate ? "truncated read rejected" : "growing read rejected");
    }
    std::filesystem::remove(path);
    std::filesystem::create_directory(path);
    require(rejectsLoad(path), "directories rejected");
    require(rejectsSave(path, {}), "cannot overwrite directory");
    std::filesystem::remove(path);
    std::filesystem::create_symlink("missing-target", path);
    require(rejectsLoad(path), "symlinks rejected without following target");
    require(rejectsSave(path, {}), "save rejects symlink primary");
    std::filesystem::remove(path);
    require(mkfifo(path.c_str(), 0600) == 0, "FIFO fixture created");
    require(rejectsLoad(path), "FIFO rejected without blocking");
    std::filesystem::remove(path);
    require(rejectsSave(path + "/missing-parent.json", {}), "unavailable parent failure observable");
}
void checkRecovery(const std::string& path) {
    using namespace fog::controller;
    writeFile(path + ".tmp", "private-uncommitted");
    require(loadSettings(path).xbox && !std::filesystem::exists(path), "orphan temporary never promoted");
    saveSettings(path, {false});
    std::filesystem::rename(path, path + ".bak");
    writeFile(path + ".tmp", "{\"version\":1,\"xbox\":true}");
    require(!loadSettings(path).xbox && !std::filesystem::exists(path + ".bak"), "interrupted swap recovers backup");
    writeFile(path + ".bak", "{\"version\":1,\"xbox\":true}");
    require(!loadSettings(path).xbox, "existing primary stays authoritative over backup");
    writeFile(path, "private-corrupt-primary");
    require(rejectsLoad(path), "corrupt primary cannot silently revert to a backup");
    writeFile(path, "{\"version\":1,\"xbox\":false}");
    saveSettings(path, {false});
    installPath = path;
    failInstall = true;
    require(rejectsSave(path, {true}), "install failure observable");
    failInstall = false;
    require(!loadSettings(path).xbox, "install failure restores old committed setting");
    failFlushAt = 1;
    require(rejectsSave(path, {true}), "temporary flush failure observable");
    require(!loadSettings(path).xbox, "temporary flush failure retains old setting");
    failFlushAt = 2;
    require(rejectsSave(path, {true}), "backup directory commit failure observable");
    require(!loadSettings(path).xbox, "backup commit failure recovers old setting");
    failFlushAt = 3;
    require(rejectsSave(path, {true}), "installed primary commit failure observable");
    require(loadSettings(path).xbox, "present primary authoritative after final commit failure");
    saveSettings(path, {false});
    require(!loadSettings(path).xbox && !std::filesystem::exists(path + ".bak"), "retry cleans backup and saves normally");
    const auto target = path + ".private-target";
    writeFile(target, "private-target-content");
    std::filesystem::create_symlink(target, path + ".tmp");
    require(rejectsSave(path, {true}), "symlink temporary rejected");
    require(!loadSettings(path).xbox && readFile(target) == "private-target-content", "failed save leaves primary and target intact");
}
}
extern "C" int __real_rename(const char*, const char*);
extern "C" int __wrap_rename(const char* from, const char* to) {
    if (failInstall && from == installPath + ".tmp" && to == installPath) { errno = EIO; return -1; }
    struct stat info{};
    if (lstat(to, &info) == 0) { errno = EEXIST; return -1; }
    return __real_rename(from, to);
}
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd) {
    if (failFlushAt && --failFlushAt == 0) { errno = EIO; return -1; }
    return __real_fsync(fd);
}
extern "C" ssize_t __real_read(int, void*, std::size_t);
extern "C" ssize_t __wrap_read(int fd, void* bytes, std::size_t count) {
    if (readFault == ReadFault::Interrupt) { readFault = ReadFault::None; errno = EINTR; return -1; }
    if (readFault == ReadFault::Failure) { readFault = ReadFault::None; errno = EIO; return -1; }
    if (readFault == ReadFault::Truncate) { readFault = ReadFault::None; return 0; }
    if (readFault == ReadFault::Partial && count > 2) count = 2;
    const auto result = __real_read(fd, bytes, count);
    if (readFault == ReadFault::Grow && result > 0) {
        // Grow the real file after its bounded read, including when the final
        // stack-buffer probe is redirected to __read_chk by glibc fortification.
        readFault = ReadFault::None;
        std::ofstream(growingReadPath, std::ios::app) << ' ';
    }
    return result;
}
int main() {
    const auto dir = std::filesystem::temp_directory_path() / ("fog-controller-layout-" + std::to_string(getpid()));
    std::filesystem::create_directory(dir);
    const auto path = (dir / "controller-settings.json").string();
    try {
        checkMappings();
        checkPersistence(path);
        checkRecovery(path);
        std::filesystem::remove_all(dir);
        std::cout << "Controller layout tests passed: " << assertions << " assertions\n";
        return 0;
    } catch (const std::exception& error) {
        readFault = ReadFault::None;
        std::filesystem::remove_all(dir);
        std::cerr << "Controller layout tests failed: " << error.what() << '\n';
        return 1;
    }
}
