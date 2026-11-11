#include "features/streaming/application/ui_telemetry_logging.hpp"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <unistd.h>

namespace ui = stream::telemetry::ui;
using Timing = fog::stream::LoopTiming;
using namespace std::chrono_literals;
static std::string rendered(const fog::RunLog::DeferredRecord& record) {
    fog::RunLog::DeferredSink sink(record.maxOutputBytes(), record.lineCount());
    record.format(sink); sink.finish(); return sink.text();
}
static std::string duration(const char* name) {
    // Golden empty distribution, including legacy units and histogram qualifiers.
    const std::string n(name);
    return n+"N=0 "+n+"AvgUs=0 "+n+"MinUs=0 "+n+"MaxUs=0 "+n+"P95UpperUs=0 "+n+"P99UpperUs=0 "+n+"Slow50ms=0 "+n+"Overflow=0";
}
static void loopFrozenAndGolden() {
    std::unique_ptr<const ui::LoopRecord> record;
    const auto began = Timing::Clock::time_point{};
    {
        Timing owner;
        Timing::Scope loop(owner, Timing::Stage::Loop, true, began);
        Timing::Scope pump(owner, Timing::Stage::Pump, true, began + 5ms);
        const auto captured = ui::captureLoop(owner, ui::LoopScope::FinalWindow, began + 100ms, began + 1ms, 123);
        assert(captured.capturedSteadyUs == 100000 && captured.spanMs == 99);
        assert(captured.active.active && captured.active.loopUs == 100000 && captured.active.phaseUs == 95000);
        assert(captured.completed.at(Timing::Stage::Pump).calls == 0);
        record = std::make_unique<ui::LoopRecord>(captured);
        owner.reset(); // Reset before scopes finish must not mutate captured state.
    }
    std::string expected = "[123 ms] Output loop final window: spanMs=99 activeLoop=1 activePhase=pumpTotal activeLoopUs=100000 activePhaseUs=95000";
    for (const char* name : {"loop", "events", "poll", "pumpTotal", "input", "draw"}) expected += " " + duration(name);
    expected += "\n[123 ms] Output draw final window:";
    for (const char* name : {"artwork", "mediaDraw", "overlay", "present", "newPresentGap"}) expected += " " + duration(name);
    expected += " newPresentAnchors=0 suppressedNewFrames=0 presentContinuityBreaks=0";
    assert(rendered(*record) == expected);
    Timing owner;
    Timing::Scope loop(owner, Timing::Stage::Loop, true, began);
    loop.finish(began + 7ms);
    owner.clearWindow();
    const auto window = ui::captureLoop(owner, ui::LoopScope::Window, began + 10ms, began, 5);
    const auto lifetime = ui::captureLoop(owner, ui::LoopScope::FinalLifetime, began + 10ms, began, 5);
    assert(window.completed.at(Timing::Stage::Loop).calls == 0);
    assert(lifetime.completed.at(Timing::Stage::Loop).calls == 1);
    assert(ui::formatLoopLine(lifetime, 0).find("Output loop final lifetime:") != std::string::npos);
    assert(ui::formatLoopLine(window, 2).empty());
    auto finalWindow = window; finalWindow.scope = ui::LoopScope::FinalWindow;
    ui::LoopRecord finalRecord(finalWindow, lifetime);
    assert(finalRecord.lineCount() == 4 && rendered(finalRecord) == ui::formatLoopLine(finalWindow,0) + "\n" +
        ui::formatLoopLine(finalWindow,1) + "\n" + ui::formatLoopLine(lifetime,0) + "\n" + ui::formatLoopLine(lifetime,1));
    // Fixed histogram fixture retains exact existing 2-ms upper-bound semantics.
    fog::stream::LoopDuration histogram;
    for (unsigned i = 0; i < 95; ++i) histogram.record(1000);
    for (unsigned i = 0; i < 4; ++i) histogram.record(34000);
    histogram.record(300000);
    const auto summary = ui::captureDuration(histogram);
    assert(summary.p95UpperUs == 2000 && summary.p99UpperUs == 34000 && summary.overflow == 1);
    assert(ui::formatDuration(summary,"sample") == histogram.text("sample"));
    // Negative active elapsed remains clamped, while legacy signed span is retained.
    Timing active;
    Timing::Scope backwards(active, Timing::Stage::Loop, true, began + 20ms);
    const auto earlier = ui::captureLoop(active, ui::LoopScope::Window, began + 10ms, began + 15ms, 8);
    assert(earlier.active.loopUs == 0 && earlier.spanMs == -5);
}
static void threadChunksAndGolden() {
    fog::diagnostics::ThreadObservationSnapshot snapshot;
    snapshot.size = 128; snapshot.epoch = 7; snapshot.overflow = 9; snapshot.persistentOverflow = 2;
    for (auto& item : snapshot.entries) std::strcpy(item.role.data(), "worker");
    assert(ui::threadChunkCount(snapshot) == 2);
    const auto now = Timing::Clock::time_point{} + 10ms;
    ui::ThreadRecord first(ui::captureThreadChunk(snapshot, 0, 99, now));
    ui::ThreadRecord second(ui::captureThreadChunk(snapshot, 1, 99, now));
    assert(first.lineCount() == 65 && second.lineCount() == 64);
    std::string expected;
    for (const auto& line : fog::diagnostics::threadObservationLines(snapshot)) {
        if (!expected.empty()) expected += '\n';
        expected += "[99 ms] " + line;
    }
    assert(rendered(first) + "\n" + rendered(second) == expected);
    assert(ui::formatThreadLine(ui::captureThreadChunk(snapshot, 1, 99, now), 0) ==
        "[99 ms] Thread scheduling role=worker slot=64 epoch=7 observedEpoch=0 carried=0 available=0 core=-1 ideal=-1 mask=0 priority=-1 queryErrors=0 affinityResult=-1 priorityResult=-1");
    const auto clone = snapshot;
    snapshot.entries[64].core = 2;
    assert(snapshot != clone);
    snapshot.entries[64].core = -1;
    assert(snapshot == clone);
    // Legacy diagnostic() replaces control characters before publishing lines.
    snapshot.entries[0].role[1] = '\n';
    ui::ThreadRecord sanitized(ui::captureThreadChunk(snapshot, 0, 1, now));
    assert(rendered(sanitized).find("role=w rker") != std::string::npos);
    fog::diagnostics::ThreadObservationSnapshot empty;
    ui::ThreadRecord emptyRecord(ui::captureThreadChunk(empty, 0, 1, now));
    assert(emptyRecord.lineCount() == 1 && rendered(emptyRecord) ==
        "[1 ms] Thread scheduling scope=transport-observed-starts inventory=not-live epoch=0 count=0 overflow=0 persistentOverflowLifetime=0");
    bool rejected = false;
    try { ui::ThreadRecord invalid(ui::captureThreadChunk(snapshot, 2, 1, now)); }
    catch (const std::length_error&) { rejected = true; }
    assert(rejected);
}
static void maximumBounds() {
    ui::LoopSnapshot value;
    value.elapsedMs = std::numeric_limits<std::uint64_t>::max();
    value.spanMs = std::numeric_limits<std::int64_t>::min();
    value.scope = ui::LoopScope::FinalLifetime;
    value.active = {Timing::Stage::Pump, value.elapsedMs, value.elapsedMs, true};
    auto fill = [](ui::DurationSummary& d) {
        d.calls = d.totalUs = d.minUs = d.maxUs = d.slow50ms = std::numeric_limits<std::uint64_t>::max();
        d.overflow = d.p95UpperUs = d.p99UpperUs = d.calls;
    };
    for (auto& d : value.completed.stages) fill(d);
    fill(value.completed.newPresentGap);
    value.completed.anchors = value.completed.suppressedFrames = value.completed.continuityBreaks = value.elapsedMs;
    ui::LoopRecord loop(value);
    assert(rendered(loop).size() <= loop.maxOutputBytes());
    fog::diagnostics::ThreadObservationSnapshot threads;
    threads.size = 128; threads.epoch = threads.overflow = threads.persistentOverflow = value.elapsedMs;
    for (auto& item : threads.entries) {
        item.role.fill('x'); item.core = item.idealCore = item.priority = std::numeric_limits<int>::min();
        item.affinity = item.observedEpoch = value.elapsedMs;
        item.affinityResult = item.priorityResult = std::numeric_limits<std::int64_t>::min();
        item.queryErrors = std::numeric_limits<unsigned>::max(); item.carried = item.available = true;
    }
    for (std::size_t chunk = 0; chunk < 2; ++chunk) {
        ui::ThreadRecord record(ui::captureThreadChunk(threads, chunk, value.elapsedMs, {}));
        assert(rendered(record).size() <= record.maxOutputBytes());
    }
}
static void concurrentFrozenRegistry() {
    using namespace fog::diagnostics;
    std::thread worker([] { for (unsigned i=0; i<2000; ++i) observeThread(("worker-"+std::to_string(i)).c_str()); });
    for (unsigned i=0; i<100; ++i) {
        const auto frozen = captureThreadObservations();
        for (std::size_t n=0; n<ui::threadChunkCount(frozen); ++n) {
            ui::ThreadRecord record(ui::captureThreadChunk(frozen,n,10,{}));
            assert(!rendered(record).empty());
        }
    }
    worker.join();
    const auto frozen = captureThreadObservations();
    ui::ThreadRecord record(ui::captureThreadChunk(frozen,0,10,{}));
    std::future<std::string> result;
    bool ready;
    {
        std::lock_guard<std::mutex> blocked(threadObservations().mutex);
        result = std::async(std::launch::async, [&record] { return rendered(record); });
        ready = result.wait_for(2s) == std::future_status::ready;
    }
    assert(ready && !result.get().empty());
}
static void realWriterAfterOwnerDeath() {
    const auto directory = std::filesystem::temp_directory_path() / ("fog-ui-telemetry-" + std::to_string(getpid()));
    std::filesystem::create_directories(directory);
    fog::RunLog log;
    assert(log.start(directory.string(), 1735689600));
    std::unique_ptr<const ui::LoopRecord> record;
    {
        auto owner = std::make_unique<Timing>();
        record = std::make_unique<ui::LoopRecord>(ui::captureLoop(*owner,ui::LoopScope::Window,{}, {}, 456));
    }
    const auto expected = rendered(*record);
    assert(log.appendDeferred(std::move(record)));
    assert(log.flush());
    const auto path = log.archivePath();
    log.close();
    std::ifstream input(path);
    const std::string bytes((std::istreambuf_iterator<char>(input)),std::istreambuf_iterator<char>());
    assert(bytes.find(expected + "\n") != std::string::npos);
    std::filesystem::remove_all(directory);
}
int main() {
    loopFrozenAndGolden(); threadChunksAndGolden(); maximumBounds(); concurrentFrozenRegistry(); realWriterAfterOwnerDeath();
    std::cout << "UI telemetry PASS: frozen loop/thread records, legacy bytes, capture clock, window/lifetime, bounds, concurrent registry and real writer after owner death\n";
    std::cout << "sizeof LoopSnapshot=" << sizeof(ui::LoopSnapshot) << " LoopRecord=" << sizeof(ui::LoopRecord)
        << " ThreadSnapshot=" << sizeof(fog::diagnostics::ThreadObservationSnapshot)
        << " ThreadChunk=" << sizeof(ui::ThreadChunk) << " ThreadRecord=" << sizeof(ui::ThreadRecord) << '\n';
}
