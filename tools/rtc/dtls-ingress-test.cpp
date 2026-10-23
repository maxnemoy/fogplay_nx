#include "impl/dtlstransport.hpp"
#include "impl/icetransport.hpp"
#include <rtc/rtc.hpp>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>
#include <vector>
#include <cstdio>
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

static rtc::message_ptr packet(unsigned sequence, unsigned type = 96) {
    auto p = rtc::make_message(16);
    (*p)[0] = std::byte(type == 22 ? 22 : 128);
    (*p)[1] = std::byte(type == 22 ? 0 : type);
    (*p)[2] = std::byte(sequence >> 8); (*p)[3] = std::byte(sequence & 255);
    return p;
}

struct Probe : rtc::impl::DtlsTransport {
    using DtlsTransport::DtlsTransport;
    std::mutex mutex, registry;
    std::condition_variable cv;
    bool entered = false, release = false;
    std::atomic<bool> feedbackAttempt{false};
    unsigned failed = 0, closed = 0;
    std::thread::id failedOn, producerOn;
    std::vector<unsigned> order;
    std::vector<unsigned> controls;
    void prepare() {
        changeState(State::Connected);
        onStateChange([this](State s) {
            if (s != State::Failed) return;
            // Mirrors a state handler doing reverse ICE work. No producer
            // callbacks are allowed while that thread owns the registry lock.
            std::lock_guard iceLock(registry);
            std::lock_guard lock(mutex);
            ++failed; failedOn = std::this_thread::get_id(); cv.notify_all();
        });
        onRecv([this](rtc::message_ptr p) {
            if (!p) { std::lock_guard lock(mutex); ++closed; cv.notify_all(); }
        });
    }
    void feed(rtc::message_ptr p) { incoming(std::move(p)); }
    void eof() { incoming(nullptr); }
    bool demuxMessage(rtc::message_ptr p) override {
        const auto sequence = (unsigned(std::to_integer<unsigned char>((*p)[2])) << 8) |
                               std::to_integer<unsigned char>((*p)[3]);
        const bool control = rtc::impl::DtlsIngressQueue::classify(p) == rtc::impl::DtlsIngressQueue::Kind::Control;
        std::unique_lock lock(mutex);
        (control ? controls : order).push_back(sequence);
        if (!entered) {
            entered = true; cv.notify_all();
            cv.wait(lock, [&] { return release; });
            lock.unlock();
            feedbackAttempt = true;
            std::lock_guard iceLock(registry); // simulated TURN feedback
            lock.lock();
        }
        cv.notify_all();
        return true;
    }
};

int main() {
    rtc::SetThreadPoolSize(2); rtc::InitLogger(rtc::LogLevel::Warning); rtc::Preload();
    {
        rtc::Configuration c; c.bindAddress = "127.0.0.1";
        auto ice = std::make_shared<rtc::impl::IceTransport>(c, nullptr, nullptr, nullptr);
        for (const bool overflow : {false, true}) {
            auto p = std::make_shared<Probe>(ice, rtc::impl::make_certificate().get(), std::nullopt,
                rtc::CertificateFingerprint::Algorithm::Sha256, [](auto &) { return true; }, nullptr);
            p->prepare(); p->feed(packet(0));
            {
                std::unique_lock lock(p->mutex);
                assert(p->cv.wait_for(lock, 5s, [&] { return p->entered; }));
            }
            auto producer = std::async(std::launch::async, [&] {
                std::lock_guard iceLock(p->registry);
                p->producerOn = std::this_thread::get_id();
                { std::lock_guard lock(p->mutex); p->release = true; p->cv.notify_all(); }
                const auto deadline = Clock::now() + 2s;
                while (!p->feedbackAttempt && Clock::now() < deadline) std::this_thread::yield();
                assert(p->feedbackAttempt);
                if (overflow) {
                    for (unsigned i = 0; i < 1025; ++i) p->feed(packet(i, i % 2 ? 200 : 22));
                } else {
                    for (unsigned i = 1; i <= 4096; ++i) p->feed(packet(i));
                    // 65 control packets: use the reserve then evict exactly
                    // the oldest RTP. Both DTLS and SRTCP keep FIFO order.
                    for (unsigned i = 0; i < 65; ++i) p->feed(packet(i, i % 2 ? 200 : 22));
                    p->feed(rtc::make_message(0));
                    p->feed(rtc::make_message(2));
                    p->eof(); // pending saturated queue must drain, then close
                    p->feed(packet(9000)); // explicitly rejected after stop
                }
            });
            // A blocking capacity wait deadlocks here against reverse feedback.
            assert(producer.wait_for(5s) == std::future_status::ready); producer.get();
            {
                std::unique_lock lock(p->mutex);
                assert(p->cv.wait_for(lock, 5s, [&] { return p->closed == 1; }));
                if (overflow) {
                    assert(p->failed == 1 && p->failedOn != p->producerOn);
                } else {
                    assert(p->failed == 0 && p->controls.size() == 65 && p->order.size() == 960);
                    assert(p->order.front() == 0);
                    for (unsigned i = 1; i < p->order.size(); ++i) assert(p->order[i] == i + 1);
                    for (unsigned i = 0; i < p->controls.size(); ++i) assert(p->controls[i] == i);
                }
            }
            const auto d = p->getDtlsDiagnostics();
            assert(d.queueHighWater == 1024 && d.queueDepth == 0 && d.fullPushWaitCount == 0);
            if (overflow) {
                assert(d.admissionCriticalOverflow == 1 && d.admissionControlAccepted == 1024);
                assert(p->state() == rtc::impl::Transport::State::Failed);
            } else {
                assert(d.admissionMediaDropped == 3136 && d.admissionMediaEvicted == 1);
                assert(d.admissionMediaAccepted == 961 && d.admissionControlAccepted == 65);
                assert(d.admissionInvalid == 2 && d.admissionStopped == 1);
                assert(p->state() == rtc::impl::Transport::State::Disconnected);
            }
            p->stop();
            std::printf("Ingress under simulated ICE registry + reverse feedback: %s, highwater=1024, zero capacity waits, bounded stop PASS\n",
                overflow ? "1025 critical -> deferred terminal failure" : "4096 RTP + 65 critical -> media shed/evicted, control FIFO");
        }
    }
    auto cleanup = rtc::Cleanup(); assert(cleanup.wait_for(5s) == std::future_status::ready);
    rtc::JoinCleanupThreads(); cleanup.get();
}
