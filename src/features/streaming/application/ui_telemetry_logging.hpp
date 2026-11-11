#pragma once
#include "features/streaming/application/ui_diagnostics_formatter.hpp"

namespace stream::telemetry::ui {
// A false return can be an accepted record with unhealthy sinks. Do not retry.
inline bool enqueueLoop(fog::RunLog& log, const LoopSnapshot& value) noexcept {
    try { return log.appendDeferred(std::make_unique<LoopRecord>(value)); }
    catch (...) { log.appendDeferred({}); return false; }
}
inline bool enqueueFinalLoops(fog::RunLog& log, const LoopSnapshot& window, const LoopSnapshot& lifetime) noexcept {
    try { return log.appendDeferred(std::make_unique<LoopRecord>(window, lifetime)); }
    catch (...) { log.appendDeferred({}); return false; }
}
inline bool enqueueThreadChunk(fog::RunLog& log, const ThreadChunk& value) noexcept {
    try { return log.appendDeferred(std::make_unique<ThreadRecord>(value)); }
    catch (...) { log.appendDeferred({}); return false; }
}
// Admission is per record, not a transaction. Do not enqueue all UI, media and
// transport groups in one burst: RunLog allows only four outstanding records.
// Caller captures before clearWindow; final combines Window then Lifetime in one record.
// Thread deduplication compares numeric snapshots, never formatted strings.
}
