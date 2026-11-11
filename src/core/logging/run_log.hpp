#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <deque>
#include <mutex>
#include <memory>
#include <string_view>
#include <stdexcept>
#include <string>
#include <thread>

namespace fog {
// Ordinary, already-sanitized diagnostics only. Rental/auth journals use separate
// synchronous storage. start/close are owner-thread lifecycle operations; only
// one RunLog may own a data directory at a time.
class RunLog {
public:
    // Each rejected enqueue request has one first-failing reason. Counts are
    // requests, while droppedLines still counts every declared rejected line.
    // Null records also account for intentional producer discards and failures
    // before a concrete record exists (capture/make_unique). Their exact cause
    // is unavailable here; invalidOrProducerLoss deliberately does not claim
    // a malformed record or a proven allocation failure.
    struct RejectionCounts {
        std::uint64_t records=0, bytes=0, lines=0, raw=0, rendered=0, invalidOrProducerLoss=0, allocation=0;
    };
    enum class Admission { Ordinary, Terminal };
    struct Stats {
        // Payload line counts; internal failure/drop notices are not writtenLines.
        // Deferred admission counts all valid declared lines, while the three
        // deferred-specific counters below count records, not emitted lines.
        std::uint64_t acceptedLines = 0, writtenLines = 0, droppedLines = 0, batches = 0;
        std::size_t queuedLines = 0, queuedBytes = 0, highWaterBytes = 0;
        std::uint64_t lastBatchUs = 0, maxBatchUs = 0;
        std::uint64_t acceptedDeferred = 0, droppedDeferred = 0, formatFailures = 0;
        std::uint64_t formatMaxUs = 0, residenceMaxUs = 0;
        std::size_t residentBytes = 0, deferredRecords = 0;
        RejectionCounts rejected;
        // Startup preservation never copies payload bytes. Compared bytes include
        // reads from both the mirror and its tracked archive.
        std::uint64_t startupPreservedBytes = 0, startupRenamedBytes = 0;
        std::uint64_t startupReusedBytes = 0, startupComparedBytes = 0, startupUs = 0;
    };
    // Historical name: the line reservation includes writer in-flight entries.
    static constexpr std::size_t maxQueuedLines = 512;
    static constexpr std::size_t maxQueuedBytes = 256 * 1024;
    static constexpr std::size_t maxLineBytes = 8192;
    static constexpr std::size_t maxDeferredRawBytes = 16 * 1024;
    static constexpr std::size_t maxDeferredRecords = 4;
    // All stream sessions using this RunLog share one fixed outstanding
    // terminal reserve, including in-flight writer entries. It is never reset per
    // session. Ordinary admission always checks TOTAL residency against the
    // original limits, so it cannot borrow this reserve. No producer waits.
    static constexpr std::size_t maxTerminalDeferredRecords = maxDeferredRecords + 3;
    static constexpr std::size_t maxTerminalQueuedBytes = maxQueuedBytes + 192 * 1024;
    // Explicit multiline records retain the 8192-byte PER-LINE bound. Their
    // payload (including inter-line separators, excluding one final newline)
    // has a separate 64-KiB bound. Existing records remain single-line by default.
    static constexpr std::size_t maxDeferredOutputBytes = 64 * 1024;
    static constexpr std::size_t maxDeferredLines = 128;
    // No owner pointers, callbacks or SDK objects belong in a concrete record.
    // Its declarations and payload must be immutable. Only the writer formats.
    class DeferredSink {
    public:
        explicit DeferredSink(std::size_t maximum, std::size_t lines = 1)
            : maximum_(maximum), declaredLines_(lines) {
            if (!maximum || maximum > (lines == 1 ? maxLineBytes : maxDeferredOutputBytes) ||
                !lines || lines > maxDeferredLines)
                throw std::length_error("deferred declaration limit");
            text_.reserve(maximum);
        }
        // The writer owns the admission limits and accumulated payload. A
        // formatter may append, but cannot replace the limits or move data out.
        DeferredSink(const DeferredSink&) = delete;
        DeferredSink& operator=(const DeferredSink&) = delete;
        DeferredSink(DeferredSink&&) = delete;
        DeferredSink& operator=(DeferredSink&&) = delete;
        void append(std::string_view value) {
            if (failed_ || value.size() > maximum_ - text_.size() || value.size() > maxLineBytes - currentLineBytes_ ||
                value.find_first_of("\r\n") != value.npos) {
                failed_ = true;
                throw std::length_error("deferred line limit");
            }
            text_.append(value.data(), value.size());
            currentLineBytes_ += value.size();
        }
        // End the current line and begin the next. Blank lines are permitted;
        // separators may only be emitted through this checked operation.
        void nextLine() {
            if (failed_ || lines_ >= declaredLines_ || text_.size() == maximum_) {
                failed_ = true;
                throw std::length_error("deferred line count limit");
            }
            text_ += '\n'; ++lines_; currentLineBytes_ = 0;
        }
        void finish() const {
            if (failed_ || lines_ != declaredLines_) throw std::length_error("deferred incomplete lines");
        }
        const std::string& text() const { return text_; }
    private:
        std::size_t maximum_, declaredLines_, lines_ = 1, currentLineBytes_ = 0;
        bool failed_ = false;
        std::string text_;
    };
    class DeferredRecord {
    public:
        virtual ~DeferredRecord() = default;
        virtual std::size_t rawBytes() const noexcept = 0;
        // Total text plus checked inter-line separators; no final newline.
        virtual std::size_t maxOutputBytes() const noexcept = 0;
        virtual std::size_t lineCount() const noexcept { return 1; }
        virtual void format(DeferredSink&) const = 0;
    };
    RunLog() = default;
    ~RunLog();
    RunLog(const RunLog&) = delete;
    RunLog& operator=(const RunLog&) = delete;
    // Commit executes during start, clean close, and on the writer thread at most every five seconds (or an explicit flush barrier).
    // The Stage 1 baseline profile instead syncs each ordinary 250-ms batch.
    // The callback must be thread-safe and must not re-enter this RunLog.
    bool start(const std::string& dataDir, std::time_t timestamp = std::time(nullptr), bool (*commit)() = nullptr) noexcept;
    // Nonblocking bounded enqueue. true means accepted with both sinks healthy,
    // not durable. Surviving sinks continue after a sticky I/O failure.
    bool append(const std::string& preparedLine) noexcept;
    // Same FIFO and nonblocking admission as append(). Charge raw value bytes
    // plus worst rendered bytes (at least the fixed failure notice), including
    // records already formatting/writing. At most four outstanding records,
    // 512 resident declared lines and 256 KiB shared charge. Each declaration
    // is frozen at admission. Failure rejects all declared lines atomically;
    // formatFailures/droppedDeferred count records, droppedLines counts lines.
    // An invalid/missing line-count declaration counts one rejected request.
    // Internal failure notices do not count as successfully written lines.
    // As with append(), false may also mean accepted with an unhealthy sink;
    // callers must not retry a false return as though admission were known.
    // Terminal is restricted by callers to Final, Closed and FinalOutput. It
    // permits at most seven TOTAL records/448 KiB; raw/rendered/line limits and
    // sticky I/O return semantics are identical to ordinary admission.
    bool appendDeferred(std::unique_ptr<const DeferredRecord> record, Admission admission = Admission::Ordinary) noexcept;
    // Ask the writer to sync pending lines promptly without waiting for SD I/O.
    void requestFlush() noexcept;
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
    enum class RejectionReason { Records, Bytes, Lines, Raw, Rendered, Invalid, Allocation };
    // Caller holds mutex_; invalid declarations count one rejected line.
    void reject(RejectionReason reason, std::size_t lines, bool deferred) noexcept;
    void fail(const char* operation, int code) noexcept;
    void closeSink(int& fd, const char* operation) noexcept;
    bool writeSink(int& fd, std::string_view batch, const char* operation, bool sync) noexcept;
    void writer() noexcept;
    int archiveFd_ = -1, mirrorFd_ = -1;
    std::atomic<bool> archiveHealthy_{false}, mirrorHealthy_{false};
    std::string archivePath_, dataDir_;
    mutable std::mutex mutex_;
    std::condition_variable ready_, drained_;
    std::thread worker_;
    struct Entry {
        std::string line;
        std::unique_ptr<const DeferredRecord> record;
        std::size_t charge = 0;
        std::uint64_t enqueuedAtUs = 0;
        std::size_t declaredLines = 1, maximumOutput = 0;
    };
    std::deque<Entry> queue_;
    Stats stats_;
    RejectionCounts pendingRejections_;
    std::uint64_t pendingDrops_ = 0, requestedBarrier_ = 0, completedBarrier_ = 0;
    std::size_t residentLines_ = 0;
    bool running_ = false, stopping_ = false;
    bool (*commit_)() = nullptr;
    const char* errorOperation_ = nullptr;
    int errorCode_ = 0;
};
}
