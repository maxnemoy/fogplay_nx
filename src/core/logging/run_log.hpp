#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace fog {
// Ordinary, already-sanitized diagnostics only. Rental/auth journals use separate
// synchronous storage. start/close are owner-thread lifecycle operations; only
// one RunLog may own a data directory at a time.
class RunLog {
public:
    struct Stats {
        std::uint64_t acceptedLines = 0, writtenLines = 0, droppedLines = 0, batches = 0;
        std::size_t queuedLines = 0, queuedBytes = 0, highWaterBytes = 0;
        std::uint64_t lastBatchUs = 0, maxBatchUs = 0;
        // Startup preservation never copies payload bytes. Compared bytes include
        // reads from both the mirror and its tracked archive.
        std::uint64_t startupPreservedBytes = 0, startupRenamedBytes = 0;
        std::uint64_t startupReusedBytes = 0, startupComparedBytes = 0, startupUs = 0;
    };
    static constexpr std::size_t maxQueuedLines = 512;
    static constexpr std::size_t maxQueuedBytes = 256 * 1024;
    static constexpr std::size_t maxLineBytes = 8192;
    RunLog() = default;
    ~RunLog();
    RunLog(const RunLog&) = delete;
    RunLog& operator=(const RunLog&) = delete;
    // Commit executes during start, clean close, and on the writer thread after each batch.
    // The callback must be thread-safe and must not re-enter this RunLog.
    bool start(const std::string& dataDir, std::time_t timestamp = std::time(nullptr), bool (*commit)() = nullptr) noexcept;
    // Nonblocking bounded enqueue. true means accepted with both sinks healthy,
    // not durable. Surviving sinks continue after a sticky I/O failure.
    bool append(const std::string& preparedLine) noexcept;
    // Wait until all earlier accepted lines and drop notices have synced/committed.
    // Reports sticky I/O failures or lost lines. Never needed in the frame loop.
    bool flush() noexcept;
    // Drain and join before closing files; call before platform/filesystem teardown.
    void close() noexcept;
    bool archiveHealthy() const noexcept { return archiveHealthy_.load(); }
    bool mirrorHealthy() const noexcept { return mirrorHealthy_.load(); }
    const std::string& archivePath() const noexcept { return archivePath_; }
    Stats stats() const noexcept;
    // Fixed operation name and numeric errno only, without file paths or content.
    std::string error() const;
private:
    void fail(const char* operation, int code) noexcept;
    void closeSink(int& fd, const char* operation) noexcept;
    bool writeSink(int& fd, const std::string& batch, const char* operation) noexcept;
    void writer() noexcept;
    int archiveFd_ = -1, mirrorFd_ = -1;
    std::atomic<bool> archiveHealthy_{false}, mirrorHealthy_{false};
    std::string archivePath_, dataDir_;
    mutable std::mutex mutex_;
    std::condition_variable ready_, drained_;
    std::thread worker_;
    std::deque<std::string> queue_;
    Stats stats_;
    std::uint64_t pendingDrops_ = 0, requestedBarrier_ = 0, completedBarrier_ = 0;
    bool running_ = false, stopping_ = false;
    bool (*commit_)() = nullptr;
    const char* errorOperation_ = nullptr;
    int errorCode_ = 0;
};
}
