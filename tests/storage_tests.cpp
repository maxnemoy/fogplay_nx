#include "core/storage/storage.hpp"
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C" int __real_rename(const char*, const char*);
extern "C" int __real_unlink(const char*);
extern "C" int __real_fsync(int);
namespace {
int assertions = 0;
int renameCalls = 0;
int failRename = 0;
int exitRename = 0;
int directorySyncs = 0;
int failDirectorySync = 0;
int exitDirectorySync = 0;
std::string failUnlink;
void resetFaults() {
    renameCalls = failRename = exitRename = directorySyncs = failDirectorySync = exitDirectorySync = 0;
    failUnlink.clear();
}
void require(bool ok, const char* label) {
    ++assertions;
    if (!ok) throw std::runtime_error(label);
}
template<class Action> void rejects(Action action, const char* operation) {
    bool caught = false;
    try { action(); }
    catch (const fog::StorageError& error) {
        caught = true;
        const std::string message = error.what();
        require(message.find(operation) != std::string::npos && message.find("errno ") != std::string::npos,
            "fixed operation and numeric errno diagnostic");
        require(message.find("PRIVATE") == std::string::npos && message.find("/tmp/") == std::string::npos,
            "diagnostic contains no path or payload");
    }
    require(caught, "storage failure reported");
}
void writeFile(const std::string& path, const std::string& data) {
    std::ofstream file(path); file << data;
    if (!file) throw std::runtime_error("fixture write failed");
}
std::string readFile(const std::string& path) {
    std::ifstream file(path);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
struct Fixture {
    std::filesystem::path root;
    std::string path;
    Fixture() {
        resetFaults();
        char pattern[] = "/tmp/fogplay-storage-tests-XXXXXX";
        const auto created = mkdtemp(pattern);
        if (!created) throw std::runtime_error("fixture directory failed");
        root = created;
        path = (root / "PRIVATE-record.json").string();
    }
    ~Fixture() { resetFaults(); std::error_code ignored; std::filesystem::remove_all(root, ignored); }
};
void interruptedSwap(int renames, int syncs) {
    Fixture fixture;
    fog::replaceStorage(fixture.path, "old");
    const auto child = fork();
    if (child < 0) throw std::runtime_error("fork failed");
    if (child == 0) {
        resetFaults(); exitRename = renames; exitDirectorySync = syncs;
        fog::replaceStorage(fixture.path, "new");
        _exit(1);
    }
    int status = 0;
    require(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 77,
        "injected interruption reached swap boundary");
    resetFaults();
    fog::recoverStorage(fixture.path);
    const auto expected = renames == 1 || syncs == 1 ? "old" : "new";
    require(readFile(fixture.path) == expected, "restart retains last available primary or restores backup");
    fog::replaceStorage(fixture.path, "latest");
    require(readFile(fixture.path) == "latest" && !std::filesystem::exists(fixture.path + ".bak"),
        "retry safely removes obsolete backup and completes replacement");
    fog::clearStorage(fixture.path);
    fog::recoverStorage(fixture.path);
    require(!std::filesystem::exists(fixture.path), "clear cannot resurrect old record");
}
}
extern "C" int __wrap_rename(const char* from, const char* to) {
    ++renameCalls;
    if (renameCalls == failRename) { errno = EIO; return -1; }
    struct stat info{};
    if (lstat(to, &info) == 0) { errno = EEXIST; return -1; }
    const auto result = __real_rename(from, to);
    if (result == 0 && renameCalls == exitRename) _exit(77);
    return result;
}
extern "C" int __wrap_unlink(const char* path) {
    if (failUnlink == path) { errno = EACCES; return -1; }
    return __real_unlink(path);
}
extern "C" int __wrap_fsync(int fd) {
    struct stat info{};
    const bool directory = fstat(fd, &info) == 0 && S_ISDIR(info.st_mode);
    if (directory && ++directorySyncs == failDirectorySync) { errno = EIO; return -1; }
    const auto result = __real_fsync(fd);
    if (directory && result == 0 && directorySyncs == exitDirectorySync) _exit(77);
    return result;
}
int main() {
    try {
        {
            Fixture f;
            fog::replaceStorage(f.path, "PRIVATE-old");
            fog::replaceStorage(f.path, "PRIVATE-new");
            require(readFile(f.path) == "PRIVATE-new", "non-overwriting rename updates primary");
            require(!std::filesystem::exists(f.path + ".bak") && !std::filesystem::exists(f.path + ".tmp"),
                "successful replacement leaves no auxiliaries");
            struct stat info{};
            require(stat(f.path.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600, "restricted primary permissions");
        }
        for (int step : {1, 2}) interruptedSwap(step, 0);
        for (int step : {1, 2, 3}) interruptedSwap(0, step);
        for (int step : {1, 2}) {
            Fixture f; fog::replaceStorage(f.path, "old"); resetFaults(); failRename = step;
            rejects([&] { fog::replaceStorage(f.path, "new"); }, step == 1 ? "backup-primary" : "install-primary");
            resetFaults(); fog::recoverStorage(f.path);
            require(readFile(f.path) == "old", "failed rename preserves sole previous marker");
        }
        for (int step : {1, 2, 3}) {
            Fixture f; fog::replaceStorage(f.path, "old"); resetFaults(); failDirectorySync = step;
            rejects([&] { fog::replaceStorage(f.path, "new"); }, "commit");
            resetFaults(); fog::recoverStorage(f.path);
            require(readFile(f.path) == (step == 1 ? "old" : "new"), "failed commit retains recoverable primary");
        }
        {
            Fixture f; writeFile(f.path + ".bak", "PRIVATE-malformed"); writeFile(f.path + ".tmp", "new");
            fog::recoverStorage(f.path);
            require(readFile(f.path) == "PRIVATE-malformed", "backup restored unchanged for caller validation");
            require(readFile(f.path + ".tmp") == "new", "temporary content never promoted during recovery");
        }
        for (const auto& suffix : {std::string(".bak"), std::string(".tmp"), std::string("")}) {
            Fixture f; fog::replaceStorage(f.path, "marker");
            writeFile(f.path + ".bak", "old"); writeFile(f.path + ".tmp", "partial");
            failUnlink = f.path + suffix;
            rejects([&] { fog::clearStorage(f.path); }, "clear-");
            require(readFile(f.path) == "marker", "clear unlink failure retains primary marker");
            resetFaults(); fog::clearStorage(f.path); fog::recoverStorage(f.path);
            require(!std::filesystem::exists(f.path) && !std::filesystem::exists(f.path + ".bak") &&
                !std::filesystem::exists(f.path + ".tmp"), "successful clear removes every recovery copy");
        }
        {
            Fixture f; fog::replaceStorage(f.path, "marker"); writeFile(f.path + ".bak", "old");
            resetFaults(); failDirectorySync = 1;
            rejects([&] { fog::clearStorage(f.path); }, "commit");
            require(readFile(f.path) == "marker", "auxiliary clear commit failure retains primary");
        }
        {
            Fixture f; fog::replaceStorage(f.path, "marker");
            resetFaults(); failDirectorySync = 1;
            rejects([&] { fog::clearStorage(f.path); }, "commit");
            require(!std::filesystem::exists(f.path), "primary unlink can precede failed final commit");
            resetFaults(); failDirectorySync = 1;
            rejects([&] { fog::clearStorage(f.path); }, "commit");
            require(directorySyncs == 1, "absent-file clear retry still attempts directory commit");
            resetFaults(); fog::clearStorage(f.path);
            require(directorySyncs == 1, "clear succeeds only after absent-file commit succeeds");
            fog::clearStorage((f.root / "missing" / "never-created.json").string());
            require(directorySyncs == 1, "missing-parent never-created store needs no removal commit");
        }
        {
            Fixture f; fog::replaceStorage(f.path, "new"); writeFile(f.path + ".bak", "old");
            fog::recoverStorage(f.path);
            require(readFile(f.path) == "new", "primary authoritative despite leftover backup");
            resetFaults(); failDirectorySync = 1;
            rejects([&] { fog::replaceStorage(f.path, "latest"); }, "commit");
            require(readFile(f.path + ".bak") == "old" && readFile(f.path) == "new",
                "retry establishes primary commit before deleting fallback");
        }
        {
            Fixture f; std::filesystem::create_directory(f.path + ".bak");
            rejects([&] { fog::recoverStorage(f.path); }, "file-type");
            require(!std::filesystem::exists(f.path), "nonregular sole backup preserved and not promoted");
        }
        std::cout << "Storage fault-injection tests passed: " << assertions << " assertions. No network or credentials.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Storage test failed: " << error.what() << '\n';
        return 1;
    }
}
