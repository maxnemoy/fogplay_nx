#include <rtc/rtc.hpp>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

int main() {
    rtc::InitLogger(rtc::LogLevel::Warning);
    rtc::Configuration config;
    config.bindAddress = "127.0.0.1";
    config.iceServers.clear();
    config.enableIceTcp = false;
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
    std::puts("Public ICE diagnostics: empty before ICE, sanitized live fields, concurrent close retention, per-peer isolation PASS");
}

