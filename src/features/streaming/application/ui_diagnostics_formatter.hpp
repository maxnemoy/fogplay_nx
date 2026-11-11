#pragma once
#include "features/streaming/application/ui_telemetry.hpp"
#include "core/logging/run_log.hpp"

namespace stream::telemetry::ui {
inline std::string prefix(std::uint64_t elapsedMs) { return "[" + std::to_string(elapsedMs) + " ms] "; }
inline const char* scopeName(LoopScope scope) noexcept {
    switch (scope) {
        case LoopScope::FinalWindow: return "final window";
        case LoopScope::FinalLifetime: return "final lifetime";
        default: return "window";
    }
}
inline std::string formatDuration(const DurationSummary& value, const char* name) {
    return std::string(name) + "N=" + std::to_string(value.calls) + " " + name + "AvgUs=" +
        std::to_string(value.calls ? value.totalUs / value.calls : 0) + " " + name + "MinUs=" + std::to_string(value.minUs) +
        " " + name + "MaxUs=" + std::to_string(value.maxUs) + " " + name + "P95UpperUs=" +
        std::to_string(value.p95UpperUs) + " " + name + "P99UpperUs=" + std::to_string(value.p99UpperUs) +
        " " + name + "Slow50ms=" + std::to_string(value.slow50ms) + " " + name + "Overflow=" + std::to_string(value.overflow);
}
inline std::string formatLoopLine(const LoopSnapshot& value, std::size_t line) {
    if (line > 1) return {};
    const auto& snapshot = value.completed;
    std::string result = prefix(value.elapsedMs) + (line ? "Output draw " : "Output loop ") + scopeName(value.scope) + ":";
    if (!line) {
        result += " spanMs=" + std::to_string(value.spanMs) + " " + LoopTiming::activeText(value.active);
        for (const auto stage : {LoopTiming::Stage::Loop, LoopTiming::Stage::Events, LoopTiming::Stage::Poll,
                LoopTiming::Stage::Pump, LoopTiming::Stage::Input, LoopTiming::Stage::Draw})
            result += " " + formatDuration(snapshot.at(stage), LoopTiming::name(stage));
    } else {
        for (const auto stage : {LoopTiming::Stage::Artwork, LoopTiming::Stage::MediaDraw,
                LoopTiming::Stage::Overlay, LoopTiming::Stage::Present})
            result += " " + formatDuration(snapshot.at(stage), LoopTiming::name(stage));
        result += " " + formatDuration(snapshot.newPresentGap, "newPresentGap") +
            " newPresentAnchors=" + std::to_string(snapshot.anchors) +
            " suppressedNewFrames=" + std::to_string(snapshot.suppressedFrames) +
            " presentContinuityBreaks=" + std::to_string(snapshot.continuityBreaks);
    }
    return result;
}
inline std::size_t threadLineCount(const ThreadChunk& value) noexcept {
    return value.size + (value.first == 0 ? 1 : 0);
}
inline std::string formatThreadLine(const ThreadChunk& value, std::size_t line) {
    if (line >= threadLineCount(value)) return {};
    if (!value.first && !line)
        return prefix(value.elapsedMs) + fog::diagnostics::threadObservationSummary(value.epoch,
            value.total, value.overflow, value.persistentOverflow);
    const auto row = line - (value.first == 0 ? 1 : 0);
    if (row >= value.entries.size()) return {};
    return prefix(value.elapsedMs) + fog::diagnostics::threadObservationRow(value.entries[row], value.first + row, value.epoch);
}
class LoopRecord final : public fog::RunLog::DeferredRecord {
public:
    explicit LoopRecord(LoopSnapshot value) : values_{{std::move(value), {}}}, scopes_(1) {}
    LoopRecord(LoopSnapshot window, LoopSnapshot lifetime) : values_{{std::move(window), std::move(lifetime)}}, scopes_(2) {
        if (values_[0].scope != LoopScope::FinalWindow || values_[1].scope != LoopScope::FinalLifetime)
            throw std::invalid_argument("invalid final loop scopes");
    }
    std::size_t rawBytes() const noexcept override { return sizeof(*this); }
    std::size_t maxOutputBytes() const noexcept override { return lineCount() * 4096; }
    std::size_t lineCount() const noexcept override { return 2 * scopes_; }
    void format(fog::RunLog::DeferredSink& sink) const override {
        for (std::size_t scope = 0; scope < scopes_; ++scope) {
            if (scope) sink.nextLine();
            sink.append(formatLoopLine(values_[scope], 0)); sink.nextLine(); sink.append(formatLoopLine(values_[scope], 1));
        }
    }
private:
    const std::array<LoopSnapshot, 2> values_;
    const std::size_t scopes_;
};
class ThreadRecord final : public fog::RunLog::DeferredRecord {
public:
    explicit ThreadRecord(ThreadChunk value) : value_(std::move(value)) {
        if (value_.size > ThreadChunkRows || value_.total > 128 || value_.first > value_.total ||
                value_.size > value_.total - value_.first || !threadLineCount(value_))
            throw std::length_error("invalid thread chunk");
    }
    std::size_t rawBytes() const noexcept override { return sizeof(*this); }
    std::size_t maxOutputBytes() const noexcept override { return lineCount() * 512; }
    std::size_t lineCount() const noexcept override { return threadLineCount(value_); }
    void format(fog::RunLog::DeferredSink& sink) const override {
        for (std::size_t i = 0; i < lineCount(); ++i) {
            if (i) sink.nextLine();
            sink.append(formatThreadLine(value_, i));
        }
    }
private:
    const ThreadChunk value_;
};
static_assert(sizeof(LoopRecord) <= fog::RunLog::maxDeferredRawBytes);
static_assert(sizeof(ThreadRecord) <= fog::RunLog::maxDeferredRawBytes);
}
