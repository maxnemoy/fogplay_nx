#include "core/diagnostics/thread_observation.hpp"
#include <cassert>
#include <iostream>
#include <thread>

int main() {
    using namespace fog::diagnostics;
    assert(!persistentThreadRole("rtc-media"));
    assert(persistentThreadRole("RTC worker"));
    observeThread("RTC worker");
    observeThread("runlog");
    observeThread("latency");
    observeThread("juice poll");
    observeThread("SCTP timer");
    observeThread("SCTP iterator");
    for (unsigned i = 0; i < 300; ++i) observeThread(("catalog-task-" + std::to_string(i)).c_str());
    assert(threadObservationText().find("overflow=0") == std::string::npos);
    beginThreadObservationEpoch();
    auto text = threadObservationText();
    assert(text.find("epoch=1 count=6 overflow=0") != std::string::npos);
    assert(text.find("role=RTC worker") != std::string::npos && text.find("role=runlog") != std::string::npos);
    assert(text.find("role=latency") != std::string::npos);
    assert(text.find("role=juice poll") != std::string::npos && text.find("role=SCTP timer") != std::string::npos &&
        text.find("role=SCTP iterator") != std::string::npos);
    assert(text.find("observedEpoch=0 carried=1") != std::string::npos);
    assert(text.find("catalog-task-") == std::string::npos);
    for (unsigned epoch = 2; epoch < 20; ++epoch) {
        beginThreadObservationEpoch();
        observeThread("application"); // Recaptures current owner, never marked carried.
        observeThread("media-video");
        text = threadObservationText();
        assert(text.find("epoch=" + std::to_string(epoch) + " observedEpoch=" +
            std::to_string(epoch) + " carried=0") != std::string::npos);
        assert(text.find("role=RTC worker") != std::string::npos);
        assert(text.find("observedEpoch=0 carried=1") != std::string::npos);
        for (unsigned i = 0; i < 150; ++i) observeThread(("catalog-task-" + std::to_string(i)).c_str());
        observeThread("control-sender"); // Saturation replaces oldest; new starts are admitted.
        assert(threadObservationText().find("role=control-sender") != std::string::npos);
    }
    beginThreadObservationEpoch();
    text = threadObservationText();
    assert(text.find("media-video") == std::string::npos && text.find("control-sender") == std::string::npos);
    assert(text.find("count=7 overflow=0") != std::string::npos);
    std::thread observer([] { for (unsigned i = 0; i < 1000; ++i) observeThread("RTC worker"); });
    for (unsigned i = 0; i < 100; ++i) {
        beginThreadObservationEpoch();
        const auto lines = threadObservationLines();
        assert(lines.size() <= 129);
        for (const auto& line : lines) assert(line.size() <= 512);
    }
    observer.join();
    beginThreadObservationEpoch();
    assert(threadObservationText().find("role=RTC worker") != std::string::npos);
    std::cout << "Thread observations PASS: saturated catalog history, per-transport reset, retained SDK starts with carried epoch, owner recapture, transient cleanup, new-start admission and concurrent epochs\n";
}
