#include <rtc/rtc.hpp>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

struct State {
    std::mutex mutex;
    std::condition_variable cv;
    bool release = false;
    size_t received = 0;
    std::shared_ptr<rtc::Track> track;
};

int main() {
    rtc::SetThreadPoolSize(2);
    rtc::InitLogger(rtc::LogLevel::Warning);
    {
        rtc::Configuration config;
        config.bindAddress = "127.0.0.1";
        config.iceServers.clear();
        auto sender = std::make_shared<rtc::PeerConnection>(config);
        auto receiver = std::make_shared<rtc::PeerConnection>(config);
        assert(!receiver->getDtlsDiagnostics().available);
        std::weak_ptr<rtc::PeerConnection> weakSender = sender, weakReceiver = receiver;
        sender->onLocalDescription([weakReceiver](rtc::Description value) {
            if (auto p = weakReceiver.lock()) p->setRemoteDescription(value);
        });
        receiver->onLocalDescription([weakSender](rtc::Description value) {
            if (auto p = weakSender.lock()) p->setRemoteDescription(value);
        });
        sender->onLocalCandidate([weakReceiver](rtc::Candidate value) {
            if (auto p = weakReceiver.lock()) p->addRemoteCandidate(value);
        });
        receiver->onLocalCandidate([weakSender](rtc::Candidate value) {
            if (auto p = weakSender.lock()) p->addRemoteCandidate(value);
        });
        auto state = std::make_shared<State>();
        std::weak_ptr<State> weakState = state;
        receiver->onTrack([weakState](std::shared_ptr<rtc::Track> track) {
            if (auto state = weakState.lock()) {
                state->track = track;
                track->onMessage([weakState](rtc::binary) {
                    if (auto state = weakState.lock()) {
                        std::unique_lock lock(state->mutex);
                        ++state->received;
                        state->cv.notify_all();
                        state->cv.wait(lock, [&] { return state->release; });
                    }
                }, [](std::string) {});
            }
        });
        rtc::Description::Video video("video", rtc::Description::Direction::SendOnly);
        video.addH264Codec(96);
        video.addSSRC(1234, "test", "stream", "track");
        auto track = sender->addTrack(video);
        auto channel = sender->createDataChannel("input");
        const auto deadline = Clock::now() + 10s;
        while (!track->isOpen() && Clock::now() < deadline) std::this_thread::sleep_for(5ms);
        assert(track->isOpen());
        rtc::binary packet = {std::byte{0x80}, std::byte{0xe0}, std::byte{0}, std::byte{1},
            std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1},
            std::byte{0}, std::byte{0}, std::byte{4}, std::byte{0xd2},
            std::byte{0x65}, std::byte{1}, std::byte{2}, std::byte{3}};
        track->send(packet);
        {
            std::unique_lock lock(state->mutex);
            assert(state->cv.wait_for(lock, 5s, [&] { return state->received == 1; }));
        }
        for (unsigned int i = 2; i <= 17; ++i) {
            packet[3] = std::byte(i);
            track->send(packet);
        }
        rtc::DtlsDiagnostics occupied;
        while (Clock::now() < deadline) {
            occupied = receiver->getDtlsDiagnostics();
            if (occupied.queueDepth >= 16) break;
            std::this_thread::sleep_for(2ms);
        }
        assert(occupied.available && occupied.queueDepth >= 16 && occupied.queueHighWater >= 16);
        std::this_thread::sleep_for(30ms);
        assert(receiver->getDtlsDiagnostics().queueOldestAgeUs >= 20000);
        {
            std::lock_guard lock(state->mutex);
            state->release = true;
            state->cv.notify_all();
        }
        {
            std::unique_lock lock(state->mutex);
            assert(state->cv.wait_for(lock, 5s, [&] { return state->received == 17; }));
        }
        rtc::DtlsDiagnostics drained;
        do {
            drained = receiver->getDtlsDiagnostics();
            if (drained.queueDepth == 0 && drained.recvProcessingMaxUs >= 20000) break;
            std::this_thread::sleep_for(2ms);
        } while (Clock::now() < deadline);
        assert(drained.dequeued >= 17 && drained.residenceMaxUs >= 20000);
        assert(drained.recvProcessingMaxUs >= 20000 && drained.recvRuns > 0);
        assert(drained.recvScheduleCount > 0 && drained.queueDepth == 0);
        std::atomic<bool> stop{false}, lost{false};
        std::thread reader([&] {
            while (!stop.load()) if (!receiver->getDtlsDiagnostics().available) lost = true;
        });
        receiver->close();
        const auto closeDeadline = Clock::now() + 5s;
        while (receiver->state() != rtc::PeerConnection::State::Closed && Clock::now() < closeDeadline)
            std::this_thread::sleep_for(2ms);
        assert(receiver->state() == rtc::PeerConnection::State::Closed);
        assert(receiver->getDtlsDiagnostics().available);
        receiver->close();
        stop = true;
        reader.join();
        assert(!lost.load());
        rtc::PeerConnection isolated(config);
        assert(!isolated.getDtlsDiagnostics().available);
        isolated.close();
        channel->close(); track->close(); sender->close();
    }
    auto cleanup = rtc::Cleanup();
    assert(cleanup.wait_for(5s) == std::future_status::ready);
    rtc::JoinCleanupThreads();
    cleanup.get();
    std::puts("Public DTLS diagnostics: real SRTP backlog, oldest age, residence, processing, concurrent close retention, per-peer isolation PASS");
}
