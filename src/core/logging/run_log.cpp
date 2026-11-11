// newlib requires POSIX visibility for descriptor truncation.
#if defined(__SWITCH__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "core/logging/run_log.hpp"
#include "core/constants/stage1_options.hpp"
#include "core/diagnostics/thread_observation.hpp"
#include <cerrno>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdio>
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
// A marker is only a hint to one exact archive, never evidence that bytes match.
// Keep its input bounded and reject path components before opening that archive.
std::string trackedArchive(const std::string& dataDir) {
    const int fd = open((dataDir + "/.last-run-archive").c_str(), protectedFlags(O_RDONLY));
    if (fd < 0) return {};
    char name[128];
    std::size_t size = 0;
    bool valid = regularFile(fd);
    while (valid && size < sizeof(name)) {
        const auto count = read(fd, name + size, sizeof(name) - size);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { valid = false; break; }
        if (count == 0) break;
        size += static_cast<std::size_t>(count);
    }
    if (::close(fd) != 0) valid = false;
    if (!valid || size < 11 || size == sizeof(name)) return {};
    const std::string result(name, size);
    if (result.compare(0, 4, "run-") != 0 || result.compare(size - 4, 4, ".log") != 0) return {};
    for (std::size_t i = 4; i < size - 4; ++i)
        if ((result[i] < '0' || result[i] > '9') && result[i] != '-') return {};
    return result;
}
ssize_t readChunk(int fd, char* buffer, std::size_t size, RunLog::Stats& stats) {
    std::size_t total = 0;
    while (total < size) {
        const auto count = read(fd, buffer + total, size - total);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) return -1;
        if (!count) break;
        total += static_cast<std::size_t>(count);
        stats.startupComparedBytes += static_cast<std::uint64_t>(count);
    }
    return static_cast<ssize_t>(total);
}
bool sameArchive(int input, const std::string& archive, RunLog::Stats& stats) {
    const int other = open(archive.c_str(), protectedFlags(O_RDONLY));
    if (other < 0) return false;
    struct stat sourceInfo{}, archiveInfo{};
    bool same = regularFile(other) && fstat(input, &sourceInfo) == 0 &&
        fstat(other, &archiveInfo) == 0 && sourceInfo.st_size == archiveInfo.st_size;
    // A user-created hard link must be renamed, since truncating the mirror
    // would otherwise truncate its archive too. FAT has no hard links and may
    // report inode zero, so zero is not treated as a meaningful identity.
    if (same && sourceInfo.st_ino != 0 && sourceInfo.st_dev == archiveInfo.st_dev &&
        sourceInfo.st_ino == archiveInfo.st_ino) same = false;
    char sourceBytes[16384], archiveBytes[16384];
    while (same) {
        const auto sourceCount = readChunk(input, sourceBytes, sizeof(sourceBytes), stats);
        const auto archiveCount = readChunk(other, archiveBytes, sizeof(archiveBytes), stats);
        if (sourceCount < 0 || archiveCount < 0 || sourceCount != archiveCount ||
            std::memcmp(sourceBytes, archiveBytes, static_cast<std::size_t>(sourceCount)) != 0) {
            same = false; break;
        }
        if (!sourceCount) break;
    }
    if (::close(other) != 0) same = false;
    return same;
}
// Owner-thread lifecycle operation: no simultaneous start on the same dataDir.
// Rename preserves the source inode without bulk writes. libnx rename does not
// replace an existing target; explicitly skip collisions on POSIX hosts too.
// On a failed sync/commit the file remains at one of the two names, never deleted.
bool preserveLegacy(const std::string& source, const std::string& directory,
    const std::string& prefix, std::time_t timestamp, const std::string& tracked,
    RunLog::Stats& stats) {
    const auto trackedPath = tracked.empty() ? std::string{} : directory + "/" + tracked;
    const int input = open(source.c_str(), protectedFlags(O_RDONLY));
    if (input < 0) return errno == ENOENT;
    int error = 0;
    struct stat info{};
    if (!regularFile(input) || fstat(input, &info) != 0 || info.st_size < 0) error = errno ? errno : EIO;
    const bool reused = !error && !tracked.empty() && sameArchive(input, trackedPath, stats);
    if (::close(input) != 0 && !error) error = errno;
    if (error) { errno = error; return false; }
    const auto bytes = static_cast<std::uint64_t>(info.st_size);
    if (reused) {
        stats.startupPreservedBytes += bytes;
        stats.startupReusedBytes += bytes;
        return true;
    }
    const auto base = directory + "/" + prefix + "-" + std::to_string(timestamp) + "-";
    for (unsigned long long collision = 0;; ++collision) {
        const auto path = base + std::to_string(collision) + ".log";
        struct stat existing{};
        if (lstat(path.c_str(), &existing) != 0) {
            if (errno != ENOENT) return false;
            if (rename(source.c_str(), path.c_str()) == 0) {
                stats.startupPreservedBytes += bytes;
                stats.startupRenamedBytes += bytes;
                return true;
            }
            if (errno != EEXIST && errno != ENOTEMPTY) return false;
        }
        if (collision == std::numeric_limits<unsigned long long>::max()) {
            errno = EOVERFLOW; return false;
        }
    }
}
bool syncDirectory(const std::string& directory) {
#ifdef __SWITCH__
    // libnx does not provide POSIX directory fsync; the platform callback
    // commits startup migration, periodic diagnostic syncs and explicit barriers.
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
// Marker rotation deliberately removes only metadata before rename: libnx cannot
// rename over an existing file. A crash in that gap simply disables dedup for the
// next startup, which then conservatively archives the whole mirror by rename.
bool storeTrackedArchive(const std::string& dataDir, const std::string& archivePath) {
    const auto marker = dataDir + "/.last-run-archive";
    const auto temporary = marker + ".tmp";
    if (unlink(temporary.c_str()) != 0 && errno != ENOENT) return false;
    const auto name = archivePath.substr(archivePath.find_last_of('/') + 1);
    const int fd = open(temporary.c_str(), protectedFlags(O_WRONLY | O_CREAT | O_EXCL), S_IRUSR | S_IWUSR);
    if (fd < 0) return false;
    int error = 0;
    if (!writeBytes(fd, name.data(), name.size()) || fsync(fd) != 0) error = errno;
    if (::close(fd) != 0 && !error) error = errno;
    if (!error && unlink(marker.c_str()) != 0 && errno != ENOENT) error = errno;
    if (!error && rename(temporary.c_str(), marker.c_str()) != 0) error = errno;
    if (!error && !syncDirectory(dataDir)) error = errno;
    if (error) { errno = error; return false; }
    return true;
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
    bool track = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        track = archiveFd_ >= 0 && mirrorFd_ >= 0 && !errorOperation_;
    }
    if (track) {
        try {
            if (!storeTrackedArchive(dataDir_, archivePath_)) fail("track-archive", errno);
            else if (commit_ && !commit_()) fail("commit-device", EIO);
        } catch (...) { fail("track-archive", ENOMEM); }
    }
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
        pendingRejections_ = {};
        queue_.clear();
        residentLines_ = 0;
        pendingDrops_ = requestedBarrier_ = completedBarrier_ = 0;
        stopping_ = false;
        commit_ = commit;
    }
    archivePath_.clear();
    const auto startupBegan = std::chrono::steady_clock::now();
    try {
        dataDir_ = dataDir;
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
            if (!preserveLegacy(dataDir + "/previous-run.log", directory, "legacy-previous", timestamp, {}, stats_)) {
                fail("preserve-previous", errno); preserved = false;
            }
            if (!preserveLegacy(dataDir + "/last-run.log", directory, "legacy-last", timestamp, trackedArchive(dataDir), stats_)) {
                fail("preserve-last", errno); preserved = false;
            }
            if (!syncDirectory(directory) || !syncDirectory(dataDir)) { fail("commit-archives", errno); preserved = false; }
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
    stats_.startupUs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - startupBegan).count());
    return !errorOperation_;
}
bool RunLog::writeSink(int& fd, std::string_view batch, const char* operation, bool sync) noexcept {
    if (fd < 0) return false;
    if ((!batch.empty() && !writeBytes(fd, batch.data(), batch.size())) || (sync && fsync(fd) != 0)) {
        fail(operation, errno);
        closeSink(fd, "close-failed-sink");
        return false;
    }
    return true;
}
void RunLog::reject(RejectionReason reason, std::size_t lines, bool deferred) noexcept {
    stats_.droppedLines += lines; pendingDrops_ += lines;
    if (deferred) ++stats_.droppedDeferred;
    for (auto* counts : {&stats_.rejected, &pendingRejections_}) {
        switch (reason) {
        case RejectionReason::Records: ++counts->records; break;
        case RejectionReason::Bytes: ++counts->bytes; break;
        case RejectionReason::Lines: ++counts->lines; break;
        case RejectionReason::Raw: ++counts->raw; break;
        case RejectionReason::Rendered: ++counts->rendered; break;
        case RejectionReason::Invalid: ++counts->invalidOrProducerLoss; break;
        case RejectionReason::Allocation: ++counts->allocation; break;
        }
    }
}
bool RunLog::append(const std::string& preparedLine) noexcept {
    bool accepted = false;
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ || stopping_) return false;
        if (preparedLine.size() > maxLineBytes) reject(RejectionReason::Rendered, 1, false);
        else if (residentLines_ >= maxQueuedLines) reject(RejectionReason::Lines, 1, false);
        else if (stats_.residentBytes >= maxQueuedBytes ||
            preparedLine.size() + 1 > maxQueuedBytes - stats_.residentBytes) reject(RejectionReason::Bytes, 1, false);
        else {
            queue_.push_back({preparedLine, {}, preparedLine.size() + 1, 0});
            stats_.residentBytes += preparedLine.size() + 1;
            ++stats_.acceptedLines;
            ++stats_.queuedLines; ++residentLines_;
            stats_.queuedBytes += preparedLine.size() + 1;
            stats_.highWaterBytes = std::max(stats_.highWaterBytes, stats_.residentBytes);
            accepted = true;
        }
    } catch (...) {
        fail("enqueue", ENOMEM);
        std::lock_guard<std::mutex> lock(mutex_);
        reject(RejectionReason::Allocation, 1, false);
    }
    ready_.notify_one();
    return accepted && archiveHealthy() && mirrorHealthy();
}
bool RunLog::appendDeferred(std::unique_ptr<const DeferredRecord> record, Admission admission) noexcept {
    bool accepted = false;
    std::size_t declaredLines = 1;
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ || stopping_) return false;
        const auto raw = record ? record->rawBytes() : 0;
        const auto rendered = record ? record->maxOutputBytes() : 0;
        const auto requestedLines = record ? record->lineCount() : 0;
        const bool validLines = requestedLines && requestedLines <= maxDeferredLines;
        if (validLines) declaredLines = requestedLines;
        const bool terminal = admission == Admission::Terminal;
        const auto maximumRecords = terminal ? maxTerminalDeferredRecords : maxDeferredRecords;
        const auto maximumBytes = terminal ? maxTerminalQueuedBytes : maxQueuedBytes;
        const auto charge = raw + std::max<std::size_t>(rendered, 64) + 1;
        // First-failing reason: invalid declaration, raw/rendered declaration,
        // then total outstanding records/lines/bytes (including in-flight).
        if (!record || !validLines || (admission != Admission::Ordinary && !terminal))
            reject(RejectionReason::Invalid, declaredLines, true);
        else if (!raw || raw > maxDeferredRawBytes) reject(RejectionReason::Raw, declaredLines, true);
        else if (!rendered || rendered > (declaredLines == 1 ? maxLineBytes : maxDeferredOutputBytes))
            reject(RejectionReason::Rendered, declaredLines, true);
        else if (stats_.deferredRecords >= maximumRecords) reject(RejectionReason::Records, declaredLines, true);
        else if (declaredLines > maxQueuedLines - residentLines_) reject(RejectionReason::Lines, declaredLines, true);
        else if (stats_.residentBytes >= maximumBytes || charge > maximumBytes - stats_.residentBytes)
            reject(RejectionReason::Bytes, declaredLines, true);
        else {
            const auto nowUs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
            queue_.push_back({{}, std::move(record), charge, nowUs, declaredLines, rendered});
            stats_.acceptedLines += declaredLines; ++stats_.acceptedDeferred; ++stats_.deferredRecords;
            stats_.queuedLines += declaredLines; residentLines_ += declaredLines;
            stats_.queuedBytes += charge; stats_.residentBytes += charge;
            stats_.highWaterBytes = std::max(stats_.highWaterBytes, stats_.residentBytes);
            accepted = true;
        }
    } catch (...) {
        std::lock_guard<std::mutex> lock(mutex_);
        reject(RejectionReason::Allocation, declaredLines, true);
    }
    ready_.notify_one();
    return accepted && archiveHealthy() && mirrorHealthy();
}
void RunLog::requestFlush() noexcept {
    { std::lock_guard<std::mutex> lock(mutex_); if (!running_ || stopping_) return; ++requestedBarrier_; }
    ready_.notify_one();
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
        fog::diagnostics::observeThread("runlog");
    using Clock = std::chrono::steady_clock;
    std::size_t inFlightLines = 0;
    bool dirty = false;
    auto nextSync = Clock::now() + std::chrono::seconds(5);
    try {
        for (;;) {
            std::deque<Entry> lines;
            std::uint64_t barrier = 0, dropped = 0;
            RejectionCounts rejected;
            bool sync = false;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                const auto hasWork = [&] { return stopping_ || !queue_.empty() || pendingDrops_ || requestedBarrier_ > completedBarrier_; };
                if (dirty) ready_.wait_until(lock, nextSync, hasWork);
                else ready_.wait(lock, hasWork);
                if (!stopping_ && requestedBarrier_ == completedBarrier_ && !queue_.empty() && Clock::now() < nextSync) {
                    ready_.wait_for(lock, std::chrono::milliseconds(250), [&] {
                        return stopping_ || requestedBarrier_ > completedBarrier_ || stats_.queuedBytes >= 32 * 1024;
                    });
                }
                if (stopping_ && !dirty && queue_.empty() && !pendingDrops_ && requestedBarrier_ == completedBarrier_) break;
                lines.swap(queue_);
                inFlightLines = stats_.queuedLines;
                stats_.queuedLines = stats_.queuedBytes = 0;
                dropped = pendingDrops_; pendingDrops_ = 0;
                rejected = pendingRejections_; pendingRejections_ = {};
                barrier = requestedBarrier_;
                sync = !fog::stage1::sparseDiagnostics || stopping_ ||
                    barrier > completedBarrier_ || Clock::now() >= nextSync;
            }
            const auto began = Clock::now();
            std::string batch;
            std::size_t formattedLines = 0, writtenEarlier = 0, failedRecords = 0, failedLines = 0;
            std::size_t releasedBytes = 0, releasedRecords = 0, releasedLines = 0;
            std::uint64_t formatMaxUs = 0, residenceMaxUs = 0;
            for (const auto& entry : lines) {
                releasedBytes += entry.charge;
                releasedLines += entry.declaredLines;
                if (entry.record) {
                    ++releasedRecords;
                    const auto formatBegan = Clock::now();
                    const auto nowUs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(formatBegan.time_since_epoch()).count());
                    residenceMaxUs = std::max(residenceMaxUs, nowUs >= entry.enqueuedAtUs ? nowUs-entry.enqueuedAtUs : 0);
                    const auto beforeRecord = batch.size();
                    try {
                        DeferredSink sink(entry.maximumOutput, entry.declaredLines);
                        entry.record->format(sink);
                        sink.finish();
                        batch += sink.text(); batch += '\n'; formattedLines += entry.declaredLines;
                    } catch (...) {
                        // Formatting and batch growth can both allocate. If text
                        // appended but its newline failed, discard the whole
                        // record before adding the bounded failure notice.
                        batch.resize(beforeRecord);
                        ++failedRecords; failedLines += entry.declaredLines;
                        constexpr std::string_view notice = "[RunLog] deferred-format failure\n";
                        try { batch += notice; }
                        catch (...) {
                            // Persistent OOM must not lose the earlier complete
                            // FIFO prefix or make the failed record partially
                            // visible. Both writes below allocate no memory;
                            // the eventual sync still belongs to this barrier.
                            const bool archive = writeSink(archiveFd_, batch, "write-archive", false);
                            const bool mirror = writeSink(mirrorFd_, batch, "write-mirror", false);
                            if (archive || mirror) writtenEarlier += formattedLines;
                            batch.clear(); formattedLines = 0;
                            writeSink(archiveFd_, notice, "write-archive", false);
                            writeSink(mirrorFd_, notice, "write-mirror", false);
                        }
                    }
                    formatMaxUs = std::max(formatMaxUs, static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-formatBegan).count()));
                } else { batch += entry.line; batch += '\n'; ++formattedLines; }
            }
            if (dropped) batch += "[RunLog] dropped " + std::to_string(dropped) +
                " diagnostic lines (enqueue rejection) rejectedRequestsRecords=" + std::to_string(rejected.records) +
                " rejectedRequestsBytes=" + std::to_string(rejected.bytes) + " rejectedRequestsLines=" + std::to_string(rejected.lines) +
                " rejectedRequestsRaw=" + std::to_string(rejected.raw) + " rejectedRequestsRendered=" + std::to_string(rejected.rendered) +
                " rejectedRequestsInvalidOrProducerLoss=" + std::to_string(rejected.invalidOrProducerLoss) + " rejectedRequestsAllocation=" + std::to_string(rejected.allocation) + "\n";
            const bool archive = writeSink(archiveFd_, batch, "write-archive", sync);
            const bool mirror = writeSink(mirrorFd_, batch, "write-mirror", sync);
            dirty = !sync;
            if (sync) nextSync = Clock::now() + std::chrono::seconds(5);
            if (sync && commit_) {
                try { if (!commit_()) fail("commit-device", EIO); }
                catch (...) { fail("commit-device", EIO); }
            }
            const auto duration = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-began).count());
            lines.clear(); // Destroy owned raw copies before releasing their admission charge.
            {
                std::lock_guard<std::mutex> lock(mutex_);
                stats_.writtenLines += writtenEarlier + ((archive || mirror) ? formattedLines : 0);
                stats_.droppedLines += failedLines; stats_.droppedDeferred += failedRecords;
                stats_.formatFailures += failedRecords;
                stats_.residentBytes -= releasedBytes; stats_.deferredRecords -= releasedRecords;
                residentLines_ -= releasedLines;
                stats_.formatMaxUs = std::max(stats_.formatMaxUs, formatMaxUs);
                stats_.residenceMaxUs = std::max(stats_.residenceMaxUs, residenceMaxUs);
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
        stats_.droppedLines += stats_.queuedLines + inFlightLines;
        stats_.droppedDeferred += stats_.deferredRecords;
        queue_.clear();
        stats_.queuedLines = stats_.queuedBytes = stats_.residentBytes = stats_.deferredRecords = 0;
        residentLines_ = 0;
        running_ = false;
    }
    drained_.notify_all();
}
}
