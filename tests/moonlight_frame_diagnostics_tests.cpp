#include "features/streaming/infrastructure/moonlight/frame_diagnostics.hpp"

#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <thread>

using moonlight::detail::FrameDiagnostics;
using moonlight::detail::TimingSamples;

namespace {
void expectSamples(const TimingSamples& sample, std::uint64_t count, std::uint64_t total,
                   std::uint64_t maximum, std::uint64_t over50ms = 0) {
    assert(sample.count == count && sample.totalUs == total && sample.maxUs == maximum);
    assert(sample.over50ms == over50ms);
}

std::array<const TimingSamples*, 9> samples(const FrameDiagnostics& trace) {
    return {&trace.assembly, &trace.coreAge, &trace.arrivalGap, &trace.hostProcessing,
            &trace.workerGap, &trace.drain, &trace.copy, &trace.queueLock, &trace.completion};
}

void cumulativeSamplesAndThreshold() {
    TimingSamples sample;
    assert(sample.text("timing") == "timing_n=0 avg_us=0 max_us=0 over50ms=0");
    for (const auto value : {0u, 50000u, 50001u, 100000u}) sample.add(value);
    expectSamples(sample, 4, 200001, 100000, 2);
    // Reading diagnostics must not consume samples or turn cumulative values into deltas.
    const std::string expected = "timing_n=4 avg_us=50000 max_us=100000 over50ms=2";
    assert(sample.text("timing") == expected && sample.text("timing") == expected);
    sample.add(2);
    expectSamples(sample, 5, 200003, 100000, 2);
    sample.reset();
    expectSamples(sample, 0, 0, 0);
}

void validMetadataAndOptionalHostLatency() {
    FrameDiagnostics trace;
    assert(trace.ageUs(1000) == 0);
    trace.frame(10, true, 100, 1000, 1100, 1200, 0);
    trace.frame(11, false, 200, 1200, 1225, 1300, 1);
    trace.frame(12, false, 0, 1300, 1300, 1300, std::numeric_limits<std::uint16_t>::max());
    assert(trace.units == 3 && trace.payloadBytes == 300 && trace.idrs == 1 && trace.invalidTimes == 0);
    expectSamples(trace.assembly, 3, 125, 100);
    expectSamples(trace.coreAge, 3, 175, 100);
    expectSamples(trace.arrivalGap, 2, 100, 100);
    // Zero means unavailable; the host value is tenths of a millisecond, converted to microseconds.
    expectSamples(trace.hostProcessing, 2, 6553600, 6553500, 1);
    assert(trace.ageUs(1350) == 50 && trace.ageUs(1299) == 0);
}

void invalidMetadataDoesNotUnderflow() {
    FrameDiagnostics trace;
    trace.frame(1, false, -10, 0, 100, 200, 0); // Missing receive timestamp.
    trace.frame(2, false, 0, 200, 199, 300, 0); // Enqueued before receipt.
    trace.frame(3, true, 30, 200, 400, 399, 0); // Polled before enqueue.
    trace.frame(4, false, 40, 200, 0, 400, 0); // Missing enqueue timestamp.
    assert(trace.units == 4 && trace.payloadBytes == 70 && trace.idrs == 1 && trace.invalidTimes == 4);
    expectSamples(trace.assembly, 0, 0, 0);
    expectSamples(trace.coreAge, 0, 0, 0);
    expectSamples(trace.hostProcessing, 0, 0, 0);
    trace.frame(5, false, 50, 400, 400, 400, 0);
    assert(trace.invalidTimes == 4 && trace.units == 5 && trace.payloadBytes == 120);
    expectSamples(trace.assembly, 1, 0, 0);
    expectSamples(trace.coreAge, 1, 0, 0);
    // A clock reading older than the preceding poll must never create an unsigned timing spike.
    const auto gapCount = trace.arrivalGap.count.load();
    const auto gapTotal = trace.arrivalGap.totalUs.load();
    trace.frame(6, false, 0, 350, 375, 390, 0);
    assert(trace.arrivalGap.count == gapCount && trace.arrivalGap.totalUs == gapTotal);
    assert(trace.arrivalGap.maxUs == 100);
}

void sequenceGapsReorderAndWrap() {
    FrameDiagnostics trace;
    for (const auto number : {10u, 11u, 15u, 15u, 14u, 16u, 0x80000010u, 17u})
        trace.frame(number, false, 0, 1, 1, 1, 0);
    assert(trace.skippedFrameNumbers == 3 && trace.repeatedOrReordered == 3);
    assert(trace.previousFrame == 17 && trace.units == 8);
    trace.reset();
    for (const auto number : {0xfffffffeu, 0xffffffffu, 0u, 2u, 0xffffffffu, 3u})
        trace.frame(number, false, 0, 1, 1, 1, 0);
    assert(trace.skippedFrameNumbers == 1 && trace.repeatedOrReordered == 1);
    assert(trace.previousFrame == 3 && trace.units == 6);
}

void workerGapsAndReset() {
    FrameDiagnostics trace;
    trace.worker(100);
    trace.worker(100);
    trace.worker(50100);
    trace.worker(100101);
    trace.worker(100000); // Invalid backward observation is omitted.
    expectSamples(trace.workerGap, 3, 100001, 50001, 1);
    trace.frame(42, true, 12, 10, 20, 30, 2);
    trace.frame(44, false, 13, 0, 20, 40, 0);
    trace.frame(44, false, 14, 10, 20, 50, 0);
    trace.drain.add(51000); trace.copy.add(2); trace.queueLock.add(3); trace.completion.add(4);
    trace.emptyPolls = 9;
    trace.reset();
    for (const auto* sample : samples(trace)) expectSamples(*sample, 0, 0, 0);
    assert(trace.units == 0 && trace.payloadBytes == 0 && trace.idrs == 0);
    assert(trace.skippedFrameNumbers == 0 && trace.repeatedOrReordered == 0);
    assert(trace.invalidTimes == 0 && trace.emptyPolls == 0 && trace.lastUnitUs == 0);
    assert(!trace.hasFrame && trace.previousFrame == 0 && trace.previousWorkerUs == 0);
    assert(trace.ageUs(1000000) == 0);
    trace.worker(2000000);
    trace.frame(1000, false, 1, 1, 2, 3, 0);
    expectSamples(trace.workerGap, 0, 0, 0);
    expectSamples(trace.arrivalGap, 0, 0, 0);
    assert(trace.skippedFrameNumbers == 0 && trace.repeatedOrReordered == 0 && trace.units == 1);
}

void concurrentSnapshots() {
    FrameDiagnostics trace;
    constexpr std::uint64_t iterations = 20000;
    std::atomic<bool> readerReady{false}, writerDone{false};
    std::thread writer([&] {
        while (!readerReady.load(std::memory_order_acquire)) std::this_thread::yield();
        for (std::uint64_t i = 1; i <= iterations; ++i) {
            const auto now = i * 100;
            trace.worker(now);
            trace.frame(static_cast<std::uint32_t>(i), i % 100 == 0, 10, now - 20, now - 10, now, 2);
            trace.drain.add(3); trace.copy.add(4); trace.queueLock.add(5); trace.completion.add(6);
            trace.emptyPolls.fetch_add(1, std::memory_order_relaxed);
        }
        writerDone.store(true, std::memory_order_release);
    });
    unsigned snapshots = 0;
    readerReady.store(true, std::memory_order_release);
    do {
        // Fields are independently atomic: check each field's bounds, never cross-field consistency.
        for (const auto* sample : samples(trace)) {
            const auto text = sample->text("snapshot");
            assert(text.find("snapshot_n=") == 0 && text.find(" avg_us=") != std::string::npos);
            assert(text.find(" max_us=") != std::string::npos && text.find(" over50ms=") != std::string::npos);
            assert(sample->count.load(std::memory_order_relaxed) <= iterations);
            assert(sample->maxUs.load(std::memory_order_relaxed) <= 200);
        }
        assert(trace.units.load(std::memory_order_relaxed) <= iterations);
        assert(trace.payloadBytes.load(std::memory_order_relaxed) <= iterations * 10);
        assert(trace.emptyPolls.load(std::memory_order_relaxed) <= iterations);
        assert(trace.ageUs(iterations * 100) <= iterations * 100);
        ++snapshots;
    } while (!writerDone.load(std::memory_order_acquire));
    writer.join();
    assert(snapshots > 0 && trace.units == iterations && trace.payloadBytes == iterations * 10);
    assert(trace.idrs == iterations / 100 && trace.emptyPolls == iterations && trace.invalidTimes == 0);
    assert(trace.skippedFrameNumbers == 0 && trace.repeatedOrReordered == 0);
    expectSamples(trace.assembly, iterations, iterations * 10, 10);
    expectSamples(trace.coreAge, iterations, iterations * 10, 10);
    expectSamples(trace.hostProcessing, iterations, iterations * 200, 200);
    expectSamples(trace.arrivalGap, iterations - 1, (iterations - 1) * 100, 100);
    expectSamples(trace.workerGap, iterations - 1, (iterations - 1) * 100, 100);
    expectSamples(trace.drain, iterations, iterations * 3, 3);
    expectSamples(trace.copy, iterations, iterations * 4, 4);
    expectSamples(trace.queueLock, iterations, iterations * 5, 5);
    expectSamples(trace.completion, iterations, iterations * 6, 6);
}
}

int main() {
    cumulativeSamplesAndThreshold();
    validMetadataAndOptionalHostLatency();
    invalidMetadataDoesNotUnderflow();
    sequenceGapsReorderAndWrap();
    workerGapsAndReset();
    concurrentSnapshots();
    std::cout << "Moonlight frame diagnostics: 6 scenarios PASS (real FrameDiagnostics, deterministic metadata, concurrent snapshots)\n";
}
