#include "core/logging/run_log.hpp"
#include <cerrno>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

extern "C" ssize_t __real_write(int, const void*, std::size_t);
extern "C" int __real_fsync(int);
namespace {
int assertions = 0;
std::atomic<int> writeCalls{0}, failWriteCall{0}, fileSyncCalls{0}, failFileSyncCall{0};
std::atomic<bool> failDirectorySync{false}, interruptWrite{false};
std::atomic<std::size_t> partialWrite{0};
std::mutex gateMutex;
std::condition_variable gateChanged;
bool blockedWrite = false, writeEntered = false;
struct ReleaseWriter {
    ~ReleaseWriter() {
        { std::lock_guard<std::mutex> lock(gateMutex); blockedWrite = false; }
        gateChanged.notify_all();
    }
};
void resetFaults() {
    writeCalls = 0; failWriteCall = 0; fileSyncCalls = 0; failFileSyncCall = 0;
    partialWrite = 0; interruptWrite = false;
    failDirectorySync = false;
}
void require(bool condition, const char* label) {
    ++assertions;
    if (!condition) throw std::runtime_error(label);
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
    Fixture() {
        resetFaults();
        char pattern[] = "/tmp/fogplay-run-log-tests-XXXXXX";
        const auto created = mkdtemp(pattern);
        if (!created) throw std::runtime_error("fixture directory failed");
        root = created;
    }
    std::string path(const char* name) const { return (root / name).string(); }
    ~Fixture() { resetFaults(); std::error_code ignored; std::filesystem::remove_all(root, ignored); }
};
void collisionAndMirror() {
    Fixture fixture;
    std::filesystem::create_directory(fixture.root / "logs");
    writeFile(fixture.path("logs/run-123-0.log"), "existing archive");
    fog::RunLog first;
    require(first.start(fixture.root.string(), 123), "start with identical timestamp collision");
    require(first.archivePath() == fixture.path("logs/run-123-1.log"), "next exclusive filename");
    require(first.append("session=1 gameId=123"), "first session append");
    require(first.append("session=2 gameId=456"), "second session same run");
    const auto firstPath = first.archivePath();
    require(first.flush(), "flush first run");
    const auto contents = readFile(firstPath);
    require(contents == "session=1 gameId=123\nsession=2 gameId=456\n", "all sessions readable before close");
    require(readFile(fixture.path("last-run.log")) == contents, "archive mirror match");
    first.close();
    fog::RunLog second;
    require(second.start(fixture.root.string(), 123), "same epoch restart");
    require(second.archivePath() == fixture.path("logs/run-123-2.log"), "restart never overwrites");
    require(readFile(fixture.path("logs/legacy-last-123-0.log")) == contents, "previous mirror preserved");
    require(second.append("new run"), "new run append");
    require(second.flush(), "flush restarted run");
    require(readFile(firstPath) == contents, "earlier archive untouched");
    require(readFile(fixture.path("last-run.log")) == "new run\n", "current mirror");
    require(readFile(fixture.path("logs/run-123-0.log")) == "existing archive", "collision file untouched");
    second.close();
    fog::RunLog rollback;
    require(rollback.start(fixture.root.string(), -1), "clock rollback safe");
    require(rollback.archivePath() == fixture.path("logs/run--1-0.log"), "numeric timestamp inside logs");
    require(rollback.append("rollback"), "rollback persists");
}
void legacyMigration() {
    Fixture fixture;
    writeFile(fixture.path("previous-run.log"), "previous\n");
    writeFile(fixture.path("last-run.log"), "last\n");
    fog::RunLog log;
    require(log.start(fixture.root.string(), 10), "migration succeeds");
    require(readFile(fixture.path("logs/legacy-previous-10-0.log")) == "previous\n", "previous archived");
    require(readFile(fixture.path("logs/legacy-last-10-0.log")) == "last\n", "last archived");
    require(readFile(fixture.path("previous-run.log")) == "previous\n", "original previous remains");
    require(readFile(fixture.path("last-run.log")).empty(), "mirror reset after preservation");
}
void missingSources() {
    Fixture fixture;
    fog::RunLog log;
    require(log.start(fixture.root.string(), 1), "missing legacy normal");
    require(log.error().empty(), "missing sources no error");
    require(!std::filesystem::exists(fixture.root / "logs/legacy-last-1-0.log"), "no fabricated legacy last");
    require(!std::filesystem::exists(fixture.root / "logs/legacy-previous-1-0.log"), "no fabricated legacy previous");
    require(log.append("persisted"), "fresh append");
    require(log.flush(), "explicit durability barrier");
    require(readFile(log.archivePath()) == "persisted\n", "flushed persisted line");
}
void migrationWriteFailure() {
    Fixture fixture;
    writeFile(fixture.path("previous-run.log"), "previous-private-payload\n");
    writeFile(fixture.path("last-run.log"), "old-last\n");
    failWriteCall = 1;
    fog::RunLog log;
    require(!log.start(fixture.root.string(), 5), "copy failure reported");
    require(log.archiveHealthy() && log.mirrorHealthy(), "copy failure leaves logging active");
    require(readFile(fixture.path("last-run.log")) == "old-last\n", "failed copy prevents truncate");
    require(readFile(fixture.path("previous-run.log")) == "previous-private-payload\n", "source retained");
    require(log.error().find("preserve-previous") != std::string::npos, "fixed operation diagnostic");
    require(log.error().find("private-payload") == std::string::npos && log.error().find("/tmp/") == std::string::npos,
        "no payload path diagnostic");
    resetFaults();
    require(log.append("current"), "continue after migration failure");
    log.flush();
    require(readFile(fixture.path("last-run.log")) == "old-last\ncurrent\n", "mirror safely appends");
    require(readFile(log.archivePath()) == "current\n", "new archive useful");
    log.close();
    fog::RunLog retry;
    require(retry.start(fixture.root.string(), 5), "copy retry succeeds");
    require(readFile(fixture.path("logs/legacy-previous-5-1.log")) == "previous-private-payload\n", "retry exclusive copy");
}
void failedArchiveDirectory() {
    Fixture fixture;
    writeFile(fixture.path("logs"), "occupied");
    writeFile(fixture.path("last-run.log"), "old\n");
    fog::RunLog log;
    require(!log.start(fixture.root.string(), 1), "invalid logs directory reported");
    require(!log.archiveHealthy() && log.mirrorHealthy(), "mirror survives");
    require(!log.append("new"), "append missing archive reports false");
    require(!log.flush(), "missing archive visible at barrier");
    require(readFile(fixture.path("last-run.log")) == "old\nnew\n", "mirror preserves old logs");
    require(readFile(fixture.path("logs")) == "occupied", "occupied path untouched");
}
void syncFailures() {
    Fixture fixture;
    writeFile(fixture.path("last-run.log"), "old\n");
    failDirectorySync = true;
    fog::RunLog log;
    require(!log.start(fixture.root.string(), 1), "directory sync failure reported");
    require(readFile(fixture.path("last-run.log")) == "old\n", "uncommitted copies prevent truncation");
    resetFaults();
    failFileSyncCall = 1;
    require(log.append("first"), "archive batch accepted before I/O");
    require(!log.flush(), "archive sync failure reported at barrier");
    require(!log.archiveHealthy() && log.mirrorHealthy(), "mirror survives archive sync failure");
    require(readFile(fixture.path("last-run.log")) == "old\nfirst\n", "mirror persisted after archive failure");
    const auto firstError = log.error();
    resetFaults();
    require(!log.append("second"), "disabled archive returns false");
    require(!log.flush(), "sticky archive failure at next barrier");
    require(readFile(fixture.path("last-run.log")) == "old\nfirst\nsecond\n", "mirror writes continue");
    require(log.error() == firstError, "first diagnostic sticky");
}
void mirrorFailure() {
    Fixture fixture;
    fog::RunLog log;
    require(log.start(fixture.root.string(), 1), "mirror test start");
    resetFaults();
    failFileSyncCall = 2;
    require(log.append("first"), "mirror batch accepted before I/O");
    require(!log.flush(), "mirror sync failure reported at barrier");
    require(log.archiveHealthy() && !log.mirrorHealthy(), "archive survives mirror failure");
    resetFaults();
    require(!log.append("second"), "missing mirror next append reports false");
    require(!log.flush(), "mirror failure remains observable");
    require(readFile(log.archivePath()) == "first\nsecond\n", "archive retains sessions");
}
bool rejectCommit() { return false; }
void invalidLegacyAndCommit() {
    Fixture fixture;
    std::filesystem::create_directory(fixture.root / "previous-run.log");
    writeFile(fixture.path("last-run.log"), "old\n");
    fog::RunLog log;
    require(!log.start(fixture.root.string(), 1), "nonregular legacy rejected");
    require(readFile(fixture.path("last-run.log")) == "old\n", "invalid previous prevents truncation");
    require(log.append("new"), "logging after invalid legacy");
    log.close();
    std::filesystem::remove(fixture.root / "previous-run.log");
    require(!log.start(fixture.root.string(), 1, rejectCommit), "platform commit failure reported");
    require(log.error().find("commit-device") != std::string::npos, "commit diagnostic fixed");
    require(!log.flush(), "missing archive visible at barrier");
    require(readFile(fixture.path("last-run.log")) == "old\nnew\n", "failed commit prevents truncation");
    require(log.append("after commit failure"), "continue after platform commit failure");
}
void queuedBatchAndShutdown() {
    Fixture fixture;
    fog::RunLog log;
    require(log.start(fixture.root.string(), 11), "batch start");
    resetFaults();
    for (int i = 0; i < 100; ++i) require(log.append("line " + std::to_string(i)), "queued line accepted");
    log.close();
    const auto stats = log.stats();
    require(stats.acceptedLines == 100 && stats.writtenLines == 100, "shutdown drains every accepted line");
    require(stats.batches <= 4 && fileSyncCalls <= 8, "ordinary lines batched instead of per-line sync");
    std::string expected;
    for (int i = 0; i < 100; ++i) expected += "line " + std::to_string(i) + "\n";
    require(readFile(log.archivePath()) == expected, "shutdown preserves FIFO order");
    require(readFile(fixture.path("last-run.log")) == expected, "shutdown mirror equals archive");
    require(!log.append("closed") && !log.flush(), "closed logger rejects new work");
    log.close();
}
void queueBoundsAndNoIoOnCaller() {
    Fixture fixture;
    fog::RunLog log;
    require(log.start(fixture.root.string(), 12), "bounded start");
    ReleaseWriter release;
    {
        std::lock_guard<std::mutex> lock(gateMutex);
        blockedWrite = true; writeEntered = false;
    }
    require(log.append("first batch"), "blocked batch queued");
    {
        std::unique_lock<std::mutex> lock(gateMutex);
        require(gateChanged.wait_for(lock, std::chrono::seconds(3), [] { return writeEntered; }), "worker entered blocked filesystem");
    }
    // I/O is intentionally blocked: completing these enqueues proves the caller
    // never performs write/fsync/commit and is independent of SD completion.
    for (std::size_t i = 0; i < fog::RunLog::maxQueuedLines; ++i)
        require(log.append("pending " + std::to_string(i)), "caller enqueues while worker I/O blocked");
    require(!log.append("over capacity"), "line bound rejects excess");
    require(!log.append(std::string(fog::RunLog::maxLineBytes + 1, 'x')), "oversize line rejected");
    const auto before = log.stats();
    require(before.queuedLines == fog::RunLog::maxQueuedLines && before.queuedBytes <= fog::RunLog::maxQueuedBytes,
        "queue memory bounded");
    require(before.droppedLines == 2 && log.error().find("dropped 2") != std::string::npos, "drops observable without filesystem access");
    {
        std::lock_guard<std::mutex> lock(gateMutex); blockedWrite = false;
    }
    gateChanged.notify_all();
    require(!log.flush(), "barrier reports previously lost lines");
    log.close();
    const auto contents = readFile(log.archivePath());
    require(contents.find("[RunLog] dropped 2 diagnostic lines (queue capacity)\n") != std::string::npos, "drop summary persisted");
    require(contents == readFile(fixture.path("last-run.log")), "drop summaries mirror consistently");
    require(log.stats().writtenLines == fog::RunLog::maxQueuedLines + 1, "accepted lines survive saturation");
}
void byteBoundAndShutdownFailure() {
    Fixture fixture;
    fog::RunLog log;
    require(log.start(fixture.root.string(), 16), "byte bound start");
    ReleaseWriter release;
    {
        std::lock_guard<std::mutex> lock(gateMutex);
        blockedWrite = true; writeEntered = false;
    }
    require(log.append("block"), "byte bound initial batch");
    {
        std::unique_lock<std::mutex> lock(gateMutex);
        require(gateChanged.wait_for(lock, std::chrono::seconds(3), [] { return writeEntered; }), "byte bound writer blocked");
    }
    const std::string large(fog::RunLog::maxLineBytes, 'x');
    const auto count = fog::RunLog::maxQueuedBytes / (large.size() + 1);
    for (std::size_t i = 0; i < count; ++i) require(log.append(large), "large line below memory bound");
    require(!log.append(large), "byte bound rejects line before count bound");
    require(log.stats().queuedBytes == count * (large.size() + 1), "exact bounded byte accounting");
    {
        std::lock_guard<std::mutex> lock(gateMutex); blockedWrite = false;
    }
    gateChanged.notify_all();
    log.close();
    require(log.stats().writtenLines == count + 1, "shutdown drains byte-full queue");
    require(log.start(fixture.root.string(), 17), "shutdown-fault restart");
    resetFaults(); failWriteCall = 1;
    require(log.append("write fails during close"), "shutdown-fault accepted before write");
    log.close();
    require(log.error().find("write-archive") != std::string::npos, "shutdown drain I/O failure remains observable");
    require(readFile(fixture.path("last-run.log")) == "write fails during close\n", "surviving mirror drains on close");
}
void partialAndInterruptedWrites() {
    Fixture fixture;
    fog::RunLog log;
    require(log.start(fixture.root.string(), 13), "partial start");
    partialWrite = 7; interruptWrite = true;
    require(log.append("partial write payload must remain complete"), "partial queued");
    require(log.flush(), "partial and EINTR retry succeeds");
    require(readFile(log.archivePath()) == "partial write payload must remain complete\n", "complete archive after short writes");
    require(readFile(fixture.path("last-run.log")) == readFile(log.archivePath()), "complete mirror after short writes");
}
std::atomic<int> commitCalls{0};
bool laterCommitFailure() { return ++commitCalls != 2; }
void backgroundFailureAndConcurrency() {
    Fixture fixture;
    fog::RunLog log;
    commitCalls = 0;
    require(log.start(fixture.root.string(), 14, laterCommitFailure), "startup commit succeeds");
    require(log.append("line"), "batch before failed commit accepted");
    require(!log.flush(), "worker commit failure reaches barrier");
    require(log.error().find("commit-device") != std::string::npos, "worker failure operation sanitized");
    log.close();
    require(commitCalls >= 2, "commit callback runs on ordinary batch");
    require(log.start(fixture.root.string(), 15), "restart resets errors and counters");
    std::thread producers[4];
    std::atomic<bool> accepted{true};
    for (int producer = 0; producer < 4; ++producer) producers[producer] = std::thread([&, producer] {
        for (int i = 0; i < 100; ++i)
            if (!log.append(std::to_string(producer) + ":" + std::to_string(i))) accepted = false;
    });
    for (auto& producer : producers) producer.join();
    require(accepted && log.flush(), "concurrent producers accepted and flushed");
    require(log.stats().acceptedLines == 400 && log.stats().writtenLines == 400, "concurrent accounting exact");
    for (int producer = 0; producer < 4; ++producer) {
        const auto contents = readFile(log.archivePath());
        std::size_t previous = 0;
        for (int i = 0; i < 100; ++i) {
            const auto found = contents.find(std::to_string(producer) + ":" + std::to_string(i) + "\n", previous);
            require(found != std::string::npos, "each producer retains order");
            previous = found + 1;
        }
    }
}
}
extern "C" ssize_t __wrap_write(int fd, const void* data, std::size_t size) {
    {
        std::unique_lock<std::mutex> lock(gateMutex);
        if (blockedWrite) {
            writeEntered = true; gateChanged.notify_all();
            gateChanged.wait(lock, [] { return !blockedWrite; });
        }
    }
    if (++writeCalls == failWriteCall) { errno = ENOSPC; return -1; }
    if (interruptWrite.exchange(false)) { errno = EINTR; return -1; }
    const auto maximum = partialWrite.load();
    return __real_write(fd, data, maximum && size > maximum ? maximum : size);
}
extern "C" int __wrap_fsync(int fd) {
    struct stat info{};
    const bool directory = fstat(fd, &info) == 0 && S_ISDIR(info.st_mode);
    if ((directory && failDirectorySync) || (!directory && ++fileSyncCalls == failFileSyncCall)) {
        errno = EIO; return -1;
    }
    return __real_fsync(fd);
}
int main() {
    try {
        collisionAndMirror(); legacyMigration(); missingSources(); migrationWriteFailure();
        failedArchiveDirectory(); syncFailures(); mirrorFailure(); invalidLegacyAndCommit();
        queuedBatchAndShutdown(); queueBoundsAndNoIoOnCaller(); byteBoundAndShutdownFailure(); partialAndInterruptedWrites(); backgroundFailureAndConcurrency();
        std::cout << "run_log_tests: " << assertions << " assertions passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "run_log_tests failed: " << error.what() << '\n';
        return 1;
    }
}
