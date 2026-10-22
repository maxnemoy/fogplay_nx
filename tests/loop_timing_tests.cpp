#include "features/streaming/domain/loop_timing.hpp"
#include <iostream>
#include <stdexcept>

using Timing = fog::stream::LoopTiming;
using Stage = Timing::Stage;
using namespace std::chrono_literals;
static void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
static void histograms() {
    fog::stream::LoopDuration histogram;
    check(histogram.percentileUpper(95) == 0, "empty quantile not zero");
    for (unsigned n = 0; n < 95; ++n) histogram.record(1000);
    for (unsigned n = 0; n < 4; ++n) histogram.record(34000);
    histogram.record(300000);
    check(histogram.calls == 100 && histogram.minUs == 1000 && histogram.maxUs == 300000 &&
        histogram.slow50ms == 1 && histogram.buckets.back() == 1, "duration distribution counters wrong");
    check(histogram.percentileUpper(95) == 2000 && histogram.percentileUpper(99) == 34000,
        "nearest-rank quantile upper bounds wrong");
    histogram.record(300001); histogram.record(400000);
    check(histogram.percentileUpper(99) == 400000, "overflow quantile falsely capped at histogram range");
    check(histogram.text("duration").find("durationOverflow=3") != std::string::npos,
        "overflow diagnostics missing");
}
static void frameContinuity() {
    Timing timing;
    const auto start = Timing::Clock::time_point{};
    timing.presented(0, true, start);
    timing.presented(1, true, start + 10s); // Startup is not a ten-second frame gap.
    timing.presented(1, true, start + 10016667us); // Old texture redraw.
    timing.presented(2, true, start + 10033333us);
    check(timing.lifetime.anchors == 1 && timing.lifetime.newPresentGap.calls == 1 &&
        timing.lifetime.newPresentGap.maxUs == 33333, "startup/repeat affected new-frame interval");
    timing.presented(3, true, start + 10233333us); // Real catch-up/stall stays visible.
    check(timing.lifetime.newPresentGap.maxUs == 200000 && timing.lifetime.newPresentGap.slow50ms == 1,
        "normal playback stall was suppressed");
    timing.presented(4, false, start + 11s); // Menu/recovery output is excluded.
    timing.presented(5, true, start + 20s);
    check(timing.lifetime.newPresentGap.calls == 2 && timing.lifetime.suppressedFrames == 1 &&
        timing.lifetime.continuityBreaks == 1 && timing.lifetime.anchors == 2,
        "menu/recovery interval bridged into playback statistics");
    timing.clearWindow(); // A report boundary must preserve normal continuity.
    timing.presented(6, true, start + 20033333us);
    check(timing.window.newPresentGap.calls == 1 && timing.window.newPresentGap.maxUs == 33333 &&
        timing.lifetime.newPresentGap.calls == 3, "window report lost frame continuity or lifetime totals");
    timing.suspendPresentation(); // A reset and IDR can happen within one pump.
    timing.presented(7, true, start + 30s);
    check(timing.lifetime.newPresentGap.calls == 3, "same-pump recovery created a false frame gap");
    timing.presented(1, true, start + 40s); // Replacement transport resets Media's counter.
    timing.presented(2, true, start + 40033333us);
    check(timing.lifetime.newPresentGap.calls == 4 && timing.lifetime.newPresentGap.maxUs == 200000,
        "replacement counter failed to reanchor");
    timing.resetPresentationCounter(); // Even an equal counter value belongs to a new Media.
    timing.presented(1, true, start + 50s);
    timing.resetPresentationCounter();
    timing.presented(1, true, start + 60s);
    timing.presented(2, true, start + 60033333us);
    check(timing.lifetime.newPresentGap.calls == 5, "equal counter on transport replacement lost first valid interval");
}
static void scopesAndStop() {
    Timing timing;
    const auto start = Timing::Clock::time_point{};
    Timing::Scope loop(timing, Stage::Loop, true, start);
    Timing::Scope events(timing, Stage::Events, true, start);
    events.finish(start + 2ms);
    Timing::Scope pump(timing, Stage::Pump, true, start + 2ms);
    check(timing.activeText(start + 100ms).find("activePhase=pumpTotal") != std::string::npos &&
        timing.activeText(start + 100ms).find("activeLoopUs=100000") != std::string::npos &&
        timing.lifetime.at(Stage::Loop).calls == 0, "mid-pump stop snapshot counted incomplete loop or hid active work");
    pump.finish(start + 110ms); // Includes the tail after existing media timings.
    loop.finish(start + 120ms);
    check(timing.lifetime.at(Stage::Pump).maxUs == 108000 && timing.lifetime.at(Stage::Loop).maxUs == 120000 &&
        timing.activeText(start + 130ms).find("activeLoop=0") != std::string::npos,
        "whole pump/loop duration omitted tail or remained active");
    {
        Timing::Scope previousRental(timing, Stage::Loop, true, start + 1s);
        timing.reset();
        previousRental.finish(start + 2s);
    }
    check(timing.lifetime.at(Stage::Loop).calls == 0, "previous rental scope contaminated reset metrics");
    try {
        Timing::Scope exceptionLoop(timing, Stage::Loop);
        Timing::Scope exceptionInput(timing, Stage::Input);
        throw std::runtime_error("simulated input failure");
    } catch (const std::runtime_error&) {}
    check(timing.lifetime.at(Stage::Loop).calls == 1 && timing.lifetime.at(Stage::Input).calls == 1 &&
        timing.activeText(Timing::Clock::now()).find("activeLoop=0") != std::string::npos,
        "exception/early return timing scopes were not safely completed");
    Timing disabled;
    Timing::Scope uiLoop(disabled, Stage::Loop, false, start);
    Timing::Scope uiPoll(disabled, Stage::Poll, true, start);
    uiPoll.finish(start + 20ms); uiLoop.finish(start + 30ms);
    check(disabled.lifetime.at(Stage::Loop).calls == 0 && disabled.lifetime.at(Stage::Poll).calls == 0,
        "non-playback UI cycles entered playback timing");
}
int main() {
    try { histograms(); frameContinuity(); scopesAndStop(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "Loop timing PASS: bounded p95/p99 including overflow, new-frame intervals, repeats, startup/menu/recovery, "
        "catch-up stalls, window/lifetime continuity, transport reset, stop mid-pump, exceptions and rental reset\n";
}
