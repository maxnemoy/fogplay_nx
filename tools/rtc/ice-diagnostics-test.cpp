#include <rtc/rtc.hpp>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <juice/juice.h>

static std::atomic<unsigned> iceSamples{0};
extern "C" int __real_juice_get_diagnostics(juice_agent_t *, juice_diagnostics_t *);
extern "C" int __wrap_juice_get_diagnostics(juice_agent_t *agent, juice_diagnostics_t *out) {
    ++iceSamples;
    return __real_juice_get_diagnostics(agent, out);
}

int main() {
    rtc::InitLogger(rtc::LogLevel::Warning);
    rtc::Configuration config;
    config.bindAddress = "127.0.0.1";
    config.iceServers.clear();
    config.enableIceTcp = false;
    {
        rtc::PeerConnection peer(config);
        (void)peer.createOffer(); // ICE only: closeTransports is synchronous.
        const auto snapshot = peer.iceDiagnostics();
        assert(!snapshot.empty());
        const auto samples = iceSamples.load();
        peer.close();
        assert(peer.state() == rtc::PeerConnection::State::Closed);
        assert(iceSamples.load() == samples); // close must not sample live ICE
        for (int i = 0; i < 1000; ++i) assert(peer.iceDiagnostics() == snapshot);
        assert(iceSamples.load() == samples); // final snapshot comes from cache

        rtc::PeerConnection neverSampled(config);
        (void)neverSampled.createOffer();
        neverSampled.close();
        assert(neverSampled.iceDiagnostics().empty());
        assert(iceSamples.load() == samples);
    }
    {
        rtc::PeerConnection peer(config);
        assert(peer.iceDiagnostics().empty());
        (void)peer.createOffer(); // Creates ICE only; no remote/network session.
        const std::string snapshot = peer.iceDiagnostics();
        assert(!snapshot.empty());
        assert(snapshot.find("iceReason=none") != std::string::npos);
        assert(snapshot.find("udpBudgetYields=0") != std::string::npos);
        assert(snapshot.find("udpMaxBatch=0") != std::string::npos);
        assert(snapshot.find("127.0.0.1") == std::string::npos);
        std::atomic<bool> stop{false}, lostSnapshot{false};
        std::thread reader([&] {
            while (!stop.load()) {
                if (peer.iceDiagnostics() != snapshot)
                    lostSnapshot = true;
            }
        });
        peer.close(); // No SCTP: synchronously exchanges the ICE pointer away.
        assert(peer.state() == rtc::PeerConnection::State::Closed);
        for (int i = 0; i < 1000; ++i)
            assert(peer.iceDiagnostics() == snapshot);
        stop = true;
        reader.join();
        assert(!lostSnapshot.load());
        peer.close(); // Repeated close must retain the same snapshot.
        assert(peer.iceDiagnostics() == snapshot);
        rtc::PeerConnection other(config);
        assert(other.iceDiagnostics().empty());
        assert(peer.iceDiagnostics() == snapshot);
        other.close();
    }
    auto cleanup = rtc::Cleanup();
    assert(cleanup.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    rtc::JoinCleanupThreads();
    cleanup.get();
    std::puts("Public ICE diagnostics: zero fresh ICE queries on close, last explicit sample retained, empty before sampling, sanitized live fields, concurrent close retention, per-peer isolation PASS");
}

