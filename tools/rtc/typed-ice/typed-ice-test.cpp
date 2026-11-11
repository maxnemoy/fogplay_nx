#include <rtc/rtc.hpp>
#include <juice/juice.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <future>
#include <limits>
#include <mutex>
#include <thread>
#include <type_traits>

using D = rtc::IceDiagnostics;
using A = D::Availability;
using P = D::Provenance;
using namespace std::chrono_literals;
static std::atomic<unsigned> calls{0};
enum class Mode { Real, Inject, Fail, BlockReal };
static std::atomic<Mode> mode{Mode::Real};
static juice_diagnostics_t fixture{};
static juice_udp_poll_diagnostics_t pollFixture{};
static std::atomic<unsigned> pollCalls{0};
static std::mutex gateMutex;
static std::condition_variable gate;
static bool entered = false, released = false;

extern "C" int __real_juice_get_udp_poll_diagnostics(juice_agent_t *, juice_diagnostics_t *, juice_udp_poll_diagnostics_t *);
extern "C" int __wrap_juice_get_udp_poll_diagnostics(juice_agent_t *agent,
    juice_diagnostics_t *summary, juice_udp_poll_diagnostics_t *out) {
    ++pollCalls;
    if (mode == Mode::Inject) { *summary = fixture; *out = pollFixture; return 0; }
    if (mode == Mode::Fail) return JUICE_ERR_FAILED;
    if (mode == Mode::BlockReal) {
        std::unique_lock lock(gateMutex);
        entered = true;
        gate.notify_all();
        gate.wait(lock, [] { return released; });
    }
    return __real_juice_get_udp_poll_diagnostics(agent, summary, out);
}

extern "C" int __real_juice_get_diagnostics(juice_agent_t *, juice_diagnostics_t *);
extern "C" int __wrap_juice_get_diagnostics(juice_agent_t *agent, juice_diagnostics_t *out) {
    ++calls;
    switch (mode.load()) {
    case Mode::Inject: *out = fixture; return JUICE_ERR_SUCCESS;
    case Mode::Fail: return JUICE_ERR_FAILED;
    case Mode::BlockReal: {
        std::unique_lock lock(gateMutex);
        entered = true;
        gate.notify_all();
        gate.wait(lock, [] { return released; });
        break;
    }
    case Mode::Real: break;
    }
    // Deliberately read the real agent AFTER close to prove shared ownership.
    return __real_juice_get_diagnostics(agent, out);
}

static void checkFixture(const D &d) {
    assert(d.availability == A::Available && d.provenance == P::Live);
    assert(d.backend == D::Backend::LibJuice);
    assert(d.state == static_cast<unsigned>(fixture.state));
    assert(d.udpBudgetYields == fixture.udp_budget_yields);
    assert(d.udpDatagrams == fixture.udp_datagrams);
    assert(d.udpMaxBatch == fixture.udp_max_batch);
    assert(d.recvErrno == -11 && d.socketError == -22 && d.stunSendErrno == -33);
    assert(d.pollFlags == 37 && d.socketErrorAvailable);
    assert(d.consentExpired == 39 && d.stunTimeouts == 40);
    assert(d.stunSendFailures == 41 && d.stunErrorResponses == 42);
    assert(d.stunErrorCode == 487 && d.connectivityTimeouts == 43);
}

static void delayedClose(const rtc::Configuration &config, bool sampled) {
    rtc::PeerConnection peer(config);
    (void)peer.createOffer();
    mode = Mode::Real;
    if (sampled) assert(peer.getIceDiagnostics().availability == A::Available);
    {
        std::lock_guard lock(gateMutex);
        entered = released = false;
    }
    mode = Mode::BlockReal;
    auto query = std::async(std::launch::async, [&] { return peer.getIceDiagnostics(); });
    {
        std::unique_lock lock(gateMutex);
        assert(gate.wait_for(lock, 5s, [] { return entered; }));
    }
    const auto beforeClose = calls.load();
    auto close = std::async(std::launch::async, [&] { peer.close(); });
    assert(close.wait_for(2s) == std::future_status::ready);
    close.get();
    assert(peer.state() == rtc::PeerConnection::State::Closed);
    assert(calls == beforeClose);
    // A blocked SDK call cannot block a closed cache read or teardown.
    const auto during = peer.getIceDiagnostics();
    assert(during.availability == (sampled ? A::Available : A::ClosedWithoutSample));
    assert(during.provenance == (sampled ? P::FinalRetained : P::None));
    {
        std::lock_guard lock(gateMutex);
        released = true;
    }
    gate.notify_all();
    assert(query.wait_for(5s) == std::future_status::ready);
    const auto late = query.get();
    assert(late.availability == A::Available && late.provenance == P::FinalRetained);
    const auto retained = peer.getIceDiagnostics();
    assert(retained.availability == A::Available && retained.provenance == P::FinalRetained);
    assert(retained.state == late.state && retained.udpDatagrams == late.udpDatagrams);
    assert(calls == beforeClose);
    mode = Mode::Real;
}

int main() {
    std::printf("DTO sizes: compact=%zu rich=%zu event=%zu\n",sizeof(D),sizeof(rtc::IceUdpPollDiagnostics),sizeof(rtc::IceUdpPollDiagnostics::Event));
    static_assert(std::is_same<decltype(D::recvErrno), int>::value);
    static_assert(std::is_same<decltype(D::socketError), int>::value);
    static_assert(std::is_same<decltype(D::stunSendErrno), int>::value);
    rtc::InitLogger(rtc::LogLevel::Warning);
    rtc::Configuration config;
    config.bindAddress = "127.0.0.1";
    config.iceServers.clear();
    config.enableIceTcp = false;
    {
        rtc::PeerConnection peer(config), other(config);
        assert(peer.getIceUdpPollDiagnostics().ice.availability == A::NotStarted);
        (void)peer.createOffer();
        auto live = peer.getIceUdpPollDiagnostics();
        assert(live.ice.availability == A::Available && live.ice.provenance == P::Live);
        assert(!live.rcvbufChecked && !live.rcvbufAvailable); // socket not initialized
        assert(live.sampleClockAvailable && live.sampleMonoUs > 0);
        mode = Mode::Inject;
        fixture = {};
        fixture.udp_datagrams = 123;
        pollFixture.sample_clock_available = true;
        pollFixture.sample_mono_us = 123456;
        pollFixture.ready_delays = 73;
        pollFixture.ready_delay_max_us = 999;
        pollFixture.event_count = 1;
        pollFixture.events_total = 9;
        pollFixture.events[0] = {9, 100, 200, 300, 400, 500, 600, 255, 1, 71, 72};
        pollFixture.legacy_eligible_batches=100;
        pollFixture.phase_sample_every=16;
        pollFixture.phase_sampled_batches=10;pollFixture.phase_timed_batches=9;pollFixture.phase_invalid_batches=1;
        pollFixture.phase_sampled_datagrams=11;pollFixture.phase_timed_datagrams=12;
        pollFixture.phase_sampled_receive_calls=13;pollFixture.phase_timed_receive_calls=14;
        pollFixture.phase_recv_total_us=15;pollFixture.phase_recv_batch_max_us=16;
        pollFixture.phase_callback_total_us=17;pollFixture.phase_callback_batch_max_us=18;
        live = peer.getIceUdpPollDiagnostics();
        assert(live.ice.udpDatagrams == 123 && live.sampleMonoUs == 123456);
        assert(live.readyDelays == 73 && live.readyDelayMaxUs == 999);
        assert(live.events[0].sequence == 9 && live.events[0].readyDelayUs == 500);
        assert(live.events[0].datagrams == 600 && live.events[0].flags == 255);
        assert(live.legacyEligibleBatches==100 && live.phaseSampleEvery==16);
        assert(live.phaseSampledBatches==10 && live.phaseTimedBatches==9 && live.phaseInvalidBatches==1);
        assert(live.phaseSampledDatagrams==11 && live.phaseTimedDatagrams==12);
        assert(live.phaseSampledReceiveCalls==13 && live.phaseTimedReceiveCalls==14);
        assert(live.phaseRecvTotalUs==15 && live.phaseRecvBatchMaxUs==16);
        assert(live.phaseCallbackTotalUs==17 && live.phaseCallbackBatchMaxUs==18);
        assert(live.events[0].phaseRecvUs==71 && live.events[0].phaseCallbackUs==72);
        mode = Mode::Fail;
        assert(peer.getIceUdpPollDiagnostics().ice.availability == A::ReadFailed);
        const auto before = pollCalls.load();
        peer.close();
        auto retained = peer.getIceUdpPollDiagnostics();
        assert(retained.ice.provenance == P::FinalRetained && retained.ice.udpDatagrams == 123);
        assert(retained.sampleMonoUs == 123456 && retained.events[0].sequence == 9);
        assert(retained.phaseSampledBatches==10 && retained.events[0].phaseRecvUs==71);
        assert(peer.getIceDiagnostics().udpDatagrams == 123);
        assert(pollCalls == before);
        other.close();
        assert(other.getIceUdpPollDiagnostics().ice.availability == A::ClosedWithoutSample);
        mode = Mode::Real;
    }
    {
        rtc::PeerConnection peer(config);
        (void)peer.createOffer();
        const auto prior = peer.getIceUdpPollDiagnostics();
        {
            std::lock_guard lock(gateMutex);
            entered = released = false;
        }
        mode = Mode::BlockReal;
        auto query = std::async(std::launch::async, [&] { return peer.getIceUdpPollDiagnostics(); });
        {
            std::unique_lock lock(gateMutex);
            assert(gate.wait_for(lock, 5s, [] { return entered; }));
        }
        const auto before = pollCalls.load();
        auto close = std::async(std::launch::async, [&] { peer.close(); });
        assert(close.wait_for(2s) == std::future_status::ready);
        close.get();
        const auto retained = peer.getIceUdpPollDiagnostics();
        assert(retained.ice.provenance == P::FinalRetained);
        assert(retained.sampleMonoUs == prior.sampleMonoUs && pollCalls == before);
        {
            std::lock_guard lock(gateMutex);
            released = true;
        }
        gate.notify_all();
        const auto late = query.get();
        assert(late.ice.provenance == P::FinalRetained && pollCalls == before);
        mode = Mode::Real;
    }
    {
        rtc::PeerConnection peer(config);
        const auto d = peer.getIceDiagnostics();
        assert(d.availability == A::NotStarted && d.provenance == P::None);
        assert(d.backend == D::Backend::LibJuice && calls == 0);
        peer.close();
        assert(peer.getIceDiagnostics().availability == A::ClosedWithoutSample);
        assert(calls == 0);
    }
    {
        rtc::PeerConnection peer(config);
        (void)peer.createOffer();
        const auto live = peer.getIceDiagnostics();
        assert(live.availability == A::Available && live.provenance == P::Live);
        assert(live.udpDatagrams == 0 && live.udpBudgetYields == 0);
        assert(live.failureReason == D::FailureReason::None);
        const auto before = calls.load();
        peer.close();
        assert(peer.iceDiagnostics().empty()); // typed query does not prime legacy cache
        for (int i = 0; i < 1000; ++i) {
            const auto final = peer.getIceDiagnostics();
            assert(final.availability == A::Available && final.provenance == P::FinalRetained);
            assert(final.state == live.state && final.udpDatagrams == live.udpDatagrams);
        }
        assert(calls == before);
    }
    {
        rtc::PeerConnection legacyOnly(config), neverSampled(config);
        (void)legacyOnly.createOffer();
        (void)neverSampled.createOffer();
        const auto line = legacyOnly.iceDiagnostics();
        const auto before = calls.load();
        legacyOnly.close();
        neverSampled.close();
        assert(legacyOnly.iceDiagnostics() == line);
        assert(legacyOnly.getIceDiagnostics().availability == A::ClosedWithoutSample);
        assert(neverSampled.getIceDiagnostics().availability == A::ClosedWithoutSample);
        assert(calls == before);
    }
    {
        rtc::PeerConnection failedOnly(config);
        (void)failedOnly.createOffer();
        mode = Mode::Fail;
        assert(failedOnly.getIceDiagnostics().availability == A::ReadFailed);
        const auto before = calls.load();
        failedOnly.close();
        assert(failedOnly.getIceDiagnostics().availability == A::ClosedWithoutSample);
        assert(calls == before);
        mode = Mode::Real;
    }
    {
        rtc::PeerConnection peer(config), other(config);
        (void)peer.createOffer();
        mode = Mode::Fail;
        auto failed = peer.getIceDiagnostics();
        assert(failed.availability == A::ReadFailed && failed.provenance == P::None);
        assert(peer.iceDiagnostics().empty());
        assert(other.getIceDiagnostics().availability == A::NotStarted);
        fixture.state = JUICE_STATE_COMPLETED;
        fixture.udp_budget_yields = std::numeric_limits<uint64_t>::max();
        fixture.udp_datagrams = 9007199254740993ULL;
        fixture.udp_max_batch = std::numeric_limits<unsigned>::max();
        fixture.recv_errno = -11;
        fixture.poll_flags = 37;
        fixture.socket_error = -22;
        fixture.socket_error_available = true;
        fixture.consent_expired = 39;
        fixture.stun_timeouts = 40;
        fixture.stun_send_failures = 41;
        fixture.stun_error_responses = 42;
        fixture.stun_error_code = 487;
        fixture.stun_send_errno = -33;
        fixture.connectivity_timeouts = 43;
        mode = Mode::Inject;
        const char *reasons[] = {"none", "udp-recv", "udp-poll", "connection", "consent-expired",
            "stun-timeout", "stun-send", "stun-response", "connectivity-timeout", "unknown"};
        for (unsigned i = 0; i < 10; ++i) {
            fixture.failure_reason = static_cast<juice_failure_reason_t>(i);
            const auto d = peer.getIceDiagnostics();
            checkFixture(d);
            assert(d.failureReason == (i < 9 ? static_cast<D::FailureReason>(i) : D::FailureReason::Unknown));
            const auto expected = "iceState=" + std::to_string(unsigned(JUICE_STATE_COMPLETED)) +
                " iceReason=" + reasons[i] +
                " udpBudgetYields=18446744073709551615 udpDatagrams=9007199254740993 udpMaxBatch=" +
                std::to_string(std::numeric_limits<unsigned>::max()) +
                " recvErrno=-11 pollFlags=37 socketError=-22 socketErrorAvailable=1 consentExpired=39"
                " stunTimeouts=40 stunSendFailures=41 stunErrorResponses=42 stunErrorCode=487"
                " stunSendErrno=-33 connectivityTimeouts=43";
            assert(peer.iceDiagnostics() == expected);
        }
        mode = Mode::Fail;
        failed = peer.getIceDiagnostics();
        assert(failed.availability == A::ReadFailed && failed.provenance == P::None);
        const auto before = calls.load();
        peer.close();
        const auto retained = peer.getIceDiagnostics();
        assert(retained.availability == A::Available && retained.provenance == P::FinalRetained);
        assert(retained.recvErrno == -11 && retained.udpDatagrams == fixture.udp_datagrams);
        assert(other.getIceDiagnostics().availability == A::NotStarted);
        other.close();
        assert(other.getIceDiagnostics().availability == A::ClosedWithoutSample);
        assert(calls == before);
        mode = Mode::Real;
    }
    {
        rtc::PeerConnection first(config), second(config);
        (void)first.createOffer();
        (void)second.createOffer();
        mode = Mode::Inject;
        fixture = {};
        auto zero = first.getIceDiagnostics();
        assert(zero.availability == A::Available && !zero.socketErrorAvailable);
        assert(zero.socketError == 0 && zero.udpDatagrams == 0);
        fixture.udp_datagrams = 123;
        assert(second.getIceDiagnostics().udpDatagrams == 123);
        const auto before = calls.load();
        first.close();
        second.close();
        assert(first.getIceDiagnostics().udpDatagrams == 0);
        assert(second.getIceDiagnostics().udpDatagrams == 123);
        assert(calls == before);
        mode = Mode::Real;
    }
    for (int i = 0; i < 16; ++i) delayedClose(config, i % 2 != 0);
    auto cleanup = rtc::Cleanup();
    assert(cleanup.wait_for(5s) == std::future_status::ready);
    rtc::JoinCleanupThreads();
    cleanup.get();
    std::printf("PASS: typed ICE size=%zu; real live/zero, unavailable/read failure, signed/max fields, all reasons, legacy golden bytes, independent caches/instances, delayed close/query x16, no close SDK calls\n", sizeof(D));
}
