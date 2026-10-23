#include "impl/dtlstransport.hpp"
#include "impl/icetransport.hpp"
#include "impl/threadpool.hpp"
#include <rtc/rtc.hpp>
#include <cassert>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>
#include <vector>
#include <cstdio>
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

struct Probe : rtc::impl::DtlsTransport {
    using DtlsTransport::DtlsTransport;
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false, release = false;
    std::vector<unsigned> order;
    unsigned closed = 0;
    void ready() {
        changeState(State::Connected);
        onRecv([this](rtc::message_ptr p) { if (!p) { std::lock_guard lock(mutex); ++closed; cv.notify_all(); } });
    }
    void queue(unsigned n) {
        auto p = rtc::make_message(12);
        (*p)[0] = std::byte{128}; (*p)[1] = std::byte{96};
        (*p)[2] = std::byte(n >> 8); (*p)[3] = std::byte(n & 255);
        mIncomingQueue.push(p);
    }
    void schedule() { enqueueRecv(); }
    void eof() { incoming(nullptr); }
    bool sslAvailable() {
        const bool got = mSslMutex.try_lock();
        if (got) mSslMutex.unlock();
        return got;
    }
    int pending() const { return mPendingRecvCount; }
    void timer() { doRecv(); }
    bool demuxMessage(rtc::message_ptr p) override {
        std::unique_lock lock(mutex);
        const unsigned n = (unsigned(std::to_integer<unsigned char>((*p)[2])) << 8) | std::to_integer<unsigned char>((*p)[3]);
        order.push_back(n);
        if (!entered) {
            entered = true; cv.notify_all();
            cv.wait(lock, [&] { return release; });
        }
        cv.notify_all();
        return true;
    }
};

int main() {
    rtc::SetThreadPoolSize(2);
    rtc::InitLogger(rtc::LogLevel::Warning);
    rtc::Preload();
    {
        rtc::Configuration c; c.bindAddress = "127.0.0.1";
        auto ice = std::make_shared<rtc::impl::IceTransport>(c, nullptr, nullptr, nullptr);
        for (const bool stopDuringFlood : {false, true}) {
        auto p = std::make_shared<Probe>(ice, rtc::impl::make_certificate().get(), std::nullopt,
            rtc::CertificateFingerprint::Algorithm::Sha256, [](auto &) { return true; }, nullptr);
        p->ready();
        // Occupy one worker so the other must give the pool back after a batch.
        std::promise<void> workerEntered, releaseWorker;
        auto releaseFuture = releaseWorker.get_future().share();
        auto occupied = rtc::impl::ThreadPool::Instance().enqueue([&] { workerEntered.set_value(); releaseFuture.wait(); });
        workerEntered.get_future().wait();
        constexpr unsigned count = 193;
        for (unsigned i = 0; i < count; ++i) p->queue(i);
        p->schedule();
        {
            std::unique_lock lock(p->mutex);
            assert(p->cv.wait_for(lock, 5s, [&] { return p->entered; }));
        }
        // A callback held indefinitely must not hold the TLS serialization lock.
        assert(p->sslAvailable());
        if (stopDuringFlood) p->eof();
        auto unrelated = rtc::impl::ThreadPool::Instance().enqueue([&] {
            std::lock_guard lock(p->mutex);
            return p->order.size();
        });
        {
            std::lock_guard lock(p->mutex); p->release = true; p->cv.notify_all();
        }
        assert(unrelated.wait_for(5s) == std::future_status::ready);
        const auto atYield = unrelated.get();
        assert(atYield > 0 && atYield <= 64 && atYield < count);
        {
            std::unique_lock lock(p->mutex);
            assert(p->cv.wait_for(lock, 5s, [&] { return p->order.size() == count; }));
            for (unsigned i = 0; i < count; ++i) assert(p->order[i] == i);
        }
        // No further incoming() call was made: continuation drained all packets.
        auto d = p->getDtlsDiagnostics();
        assert(d.recvBatchYields >= 3 && d.recvBatchPacketsMax <= 64);
        p->eof();
        {
            std::unique_lock lock(p->mutex);
            assert(p->cv.wait_for(lock, 5s, [&] { return p->closed == 1; }));
        }
        assert(p->state() == rtc::impl::Transport::State::Disconnected);
        p->timer(); p->timer(); // timer callbacks must never consume queued tokens
        assert(p->pending() >= 0);
        releaseWorker.set_value(); occupied.get();
        std::printf("TLS receive fairness: %zu packets before unrelated task; %llu yields; ordered 193; %s EOF PASS\n", atYield, (unsigned long long)d.recvBatchYields, stopDuringFlood ? "pending flood" : "idle");
        }
    }
    auto cleanup = rtc::Cleanup();
    assert(cleanup.wait_for(5s) == std::future_status::ready);
    rtc::JoinCleanupThreads(); cleanup.get();
}
