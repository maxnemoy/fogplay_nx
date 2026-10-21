// newlib requires POSIX visibility for descriptor truncation.
#if defined(__SWITCH__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "core/logging/run_log.hpp"
#include <cerrno>
#include <algorithm>
#include <chrono>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

namespace fog {
namespace {
int protectedFlags(int flags) {
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
#ifdef O_NONBLOCK
    flags |= O_NONBLOCK;
#endif
    return flags;
}
bool regularFile(int fd) {
    struct stat info{};
    if (fstat(fd, &info) != 0) return false;
    if (!S_ISREG(info.st_mode)) { errno = EINVAL; return false; }
    return true;
}
bool writeBytes(int fd, const char* bytes, std::size_t size) {
    std::size_t offset = 0;
    while (offset < size) {
        const auto count = write(fd, bytes + offset, size - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { if (count == 0) errno = EIO; return false; }
        offset += static_cast<std::size_t>(count);
    }
    return true;
}
int createArchive(const std::string& directory, const std::string& prefix,
    std::time_t timestamp, std::string& path) {
    const auto base = directory + "/" + prefix + "-" + std::to_string(timestamp) + "-";
    for (unsigned long long collision = 0;; ++collision) {
        path = base + std::to_string(collision) + ".log";
        const int fd = open(path.c_str(), protectedFlags(O_WRONLY | O_CREAT | O_EXCL),
            S_IRUSR | S_IWUSR);
        if (fd >= 0) return fd;
        if (errno != EEXIST) return -1;
        if (collision == std::numeric_limits<unsigned long long>::max()) {
            errno = EOVERFLOW; return -1;
        }
    }
}
// Keep original legacy files until their copies have been written and synced.
// Partial copies are deliberately retained; a later start uses another name.
bool preserveLegacy(const std::string& source, const std::string& directory,
    const std::string& prefix, std::time_t timestamp) {
    const int input = open(source.c_str(), protectedFlags(O_RDONLY));
    if (input < 0) return errno == ENOENT;
    int output = -1;
    int error = 0;
    try {
        if (!regularFile(input)) error = errno;
        std::string path;
        if (!error) {
            output = createArchive(directory, prefix, timestamp, path);
            if (output < 0) error = errno;
        }
        char buffer[4096];
        while (!error) {
            const auto count = read(input, buffer, sizeof(buffer));
            if (count < 0 && errno == EINTR) continue;
            if (count < 0) { error = errno; break; }
            if (count == 0) break;
            if (!writeBytes(output, buffer, static_cast<std::size_t>(count))) error = errno;
        }
        if (!error && fsync(output) != 0) error = errno;
    } catch (...) { error = ENOMEM; }
    if (output >= 0 && ::close(output) != 0 && !error) error = errno;
    if (::close(input) != 0 && !error) error = errno;
    if (error) { errno = error; return false; }
    return true;
}
bool syncDirectory(const std::string& directory) {
#ifdef __SWITCH__
    // libnx does not provide POSIX directory fsync; the platform callback
    // commits startup migration and each ordinary diagnostic batch.
    (void)directory;
    return true;
#else
    const int fd = open(directory.c_str(), O_RDONLY);
    if (fd < 0) return false;
    int error = fsync(fd) == 0 ? 0 : errno;
    if (::close(fd) != 0 && !error) error = errno;
    if (error) { errno = error; return false; }
    return true;
#endif
}
}
RunLog::~RunLog() { close(); }
void RunLog::fail(const char* operation, int code) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (errorOperation_) return;
    errorOperation_ = operation;
    errorCode_ = code ? code : EIO;
}
std::string RunLog::error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (errorOperation_) return std::string("Run log ") + errorOperation_ + " failed (errno " + std::to_string(errorCode_) + ")";
    if (stats_.droppedLines) return "Run log queue dropped " + std::to_string(stats_.droppedLines) + " diagnostic lines";
    return {};
}
RunLog::Stats RunLog::stats() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}
void RunLog::closeSink(int& fd, const char* operation) noexcept {
    if (&fd == &archiveFd_) archiveHealthy_ = false;
    if (&fd == &mirrorFd_) mirrorHealthy_ = false;
    if (fd < 0) return;
    const int old = fd;
    fd = -1;
    if (::close(old) != 0) fail(operation, errno);
}
void RunLog::close() noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    ready_.notify_all();
    if (worker_.joinable()) worker_.join();
    closeSink(archiveFd_, "close-archive");
    closeSink(mirrorFd_, "close-mirror");
}
bool RunLog::start(const std::string& dataDir, std::time_t timestamp, bool (*commit)()) noexcept {
    close();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        errorOperation_ = nullptr;
        errorCode_ = 0;
        stats_ = {};
        queue_.clear();
        pendingDrops_ = requestedBarrier_ = completedBarrier_ = 0;
        stopping_ = false;
        commit_ = commit;
    }
    archivePath_.clear();
    try {
        const auto directory = dataDir + "/logs";
        bool directoryReady = mkdir(directory.c_str(), S_IRWXU) == 0;
        if (!directoryReady && errno == EEXIST) {
            struct stat info{};
            directoryReady = lstat(directory.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
            if (!directoryReady) errno = EINVAL;
        }
        if (!directoryReady) fail("archive-directory", errno);
        bool preserved = directoryReady;
        if (directoryReady) {
            archiveFd_ = createArchive(directory, "run", timestamp, archivePath_);
            if (archiveFd_ < 0) fail("open-archive", errno);
            if (!preserveLegacy(dataDir + "/previous-run.log", directory, "legacy-previous", timestamp)) {
                fail("preserve-previous", errno); preserved = false;
            }
            if (!preserveLegacy(dataDir + "/last-run.log", directory, "legacy-last", timestamp)) {
                fail("preserve-last", errno); preserved = false;
            }
            if (!syncDirectory(directory)) { fail("commit-archives", errno); preserved = false; }
            if (commit && !commit()) { fail("commit-device", EIO); preserved = false; }
        }
        mirrorFd_ = open((dataDir + "/last-run.log").c_str(),
            protectedFlags(O_WRONLY | O_CREAT | O_APPEND), S_IRUSR | S_IWUSR);
        if (mirrorFd_ < 0) fail("open-mirror", errno);
        else if (!regularFile(mirrorFd_)) {
            fail("inspect-mirror", errno); closeSink(mirrorFd_, "close-mirror");
        } else if (preserved && ftruncate(mirrorFd_, 0) != 0) {
            fail("truncate-mirror", errno); closeSink(mirrorFd_, "close-mirror");
        }
    } catch (...) { fail("prepare", ENOMEM); }
    archiveHealthy_ = archiveFd_ >= 0;
    mirrorHealthy_ = mirrorFd_ >= 0;
    if (archiveFd_ >= 0 || mirrorFd_ >= 0) {
        try {
            { std::lock_guard<std::mutex> lock(mutex_); running_ = true; }
            worker_ = std::thread(&RunLog::writer, this);
        } catch (...) {
            { std::lock_guard<std::mutex> lock(mutex_); running_ = false; }
            fail("start-writer", EAGAIN);
            closeSink(archiveFd_, "close-archive");
            closeSink(mirrorFd_, "close-mirror");
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return !errorOperation_;
}
bool RunLog::writeSink(int& fd, const std::string& batch, const char* operation) noexcept {
    if (fd < 0) return false;
    if ((!batch.empty() && !writeBytes(fd, batch.data(), batch.size())) || fsync(fd) != 0) {
        fail(operation, errno);
        closeSink(fd, "close-failed-sink");
        return false;
    }
    return true;
}
bool RunLog::append(const std::string& preparedLine) noexcept {
    bool accepted = false;
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ || stopping_) return false;
        if (preparedLine.size() > maxLineBytes || queue_.size() >= maxQueuedLines ||
            preparedLine.size() + 1 > maxQueuedBytes - stats_.queuedBytes) {
            ++stats_.droppedLines; ++pendingDrops_;
        } else {
            queue_.push_back(preparedLine);
            ++stats_.acceptedLines;
            stats_.queuedLines = queue_.size();
            stats_.queuedBytes += preparedLine.size() + 1;
            stats_.highWaterBytes = std::max(stats_.highWaterBytes, stats_.queuedBytes);
            accepted = true;
        }
    } catch (...) {
        fail("enqueue", ENOMEM);
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.droppedLines; ++pendingDrops_;
    }
    ready_.notify_one();
    return accepted && archiveHealthy() && mirrorHealthy();
}
bool RunLog::flush() noexcept {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!running_ || stopping_) return false;
    const auto barrier = ++requestedBarrier_;
    ready_.notify_one();
    drained_.wait(lock, [&] { return completedBarrier_ >= barrier || !running_; });
    return completedBarrier_ >= barrier && !errorOperation_ && !stats_.droppedLines && archiveHealthy() && mirrorHealthy();
}
void RunLog::writer() noexcept {
    using Clock = std::chrono::steady_clock;
    std::size_t inFlightLines = 0;
    try {
        for (;;) {
            std::deque<std::string> lines;
            std::uint64_t barrier = 0, dropped = 0;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                ready_.wait(lock, [&] { return stopping_ || !queue_.empty() || pendingDrops_ || requestedBarrier_ > completedBarrier_; });
                if (!stopping_ && requestedBarrier_ == completedBarrier_) {
                    ready_.wait_for(lock, std::chrono::milliseconds(250), [&] {
                        return stopping_ || requestedBarrier_ > completedBarrier_ || stats_.queuedBytes >= 32 * 1024;
                    });
                }
                if (stopping_ && queue_.empty() && !pendingDrops_ && requestedBarrier_ == completedBarrier_) break;
                lines.swap(queue_);
                inFlightLines = lines.size();
                stats_.queuedLines = stats_.queuedBytes = 0;
                dropped = pendingDrops_; pendingDrops_ = 0;
                barrier = requestedBarrier_;
            }
            const auto began = Clock::now();
            std::string batch;
            for (const auto& line : lines) { batch += line; batch += '\n'; }
            if (dropped) batch += "[RunLog] dropped " + std::to_string(dropped) + " diagnostic lines (queue capacity)\n";
            const bool archive = writeSink(archiveFd_, batch, "write-archive");
            const bool mirror = writeSink(mirrorFd_, batch, "write-mirror");
            if (commit_) {
                try { if (!commit_()) fail("commit-device", EIO); }
                catch (...) { fail("commit-device", EIO); }
            }
            const auto duration = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-began).count());
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (archive || mirror) stats_.writtenLines += lines.size();
                ++stats_.batches;
                stats_.lastBatchUs = duration;
                stats_.maxBatchUs = std::max(stats_.maxBatchUs, duration);
                completedBarrier_ = barrier;
                inFlightLines = 0;
            }
            drained_.notify_all();
        }
    } catch (...) { fail("writer", ENOMEM); }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.droppedLines += queue_.size() + inFlightLines;
        queue_.clear();
        stats_.queuedLines = stats_.queuedBytes = 0;
        running_ = false;
    }
    drained_.notify_all();
}
}
