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
    unsigned expectedSequence = 1;
    bool outboundControl = false, inboundControl = false;
    bool saturatedControl = false;
    std::shared_ptr<rtc::DataChannel> channel;
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
                track->onMessage([weakState](rtc::binary packet) {
                    if (auto state = weakState.lock()) {
                        std::unique_lock lock(state->mutex);
                        const unsigned sequence = (unsigned(std::to_integer<unsigned char>(packet[2])) << 8) | std::to_integer<unsigned char>(packet[3]);
                        assert(sequence == state->expectedSequence++);
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
        receiver->onDataChannel([weakState](std::shared_ptr<rtc::DataChannel> channel) {
            if (auto s = weakState.lock()) {
                { std::lock_guard lock(s->mutex); s->channel = channel; s->cv.notify_all(); }
                channel->onMessage([](rtc::binary) {}, [weakState](std::string value) {
                    if (auto s = weakState.lock()) {
                        std::lock_guard lock(s->mutex);
                        if (value == "after-media-flood") s->inboundControl = true;
                        else { assert(value == "saturated-control"); s->saturatedControl = true; }
                        s->cv.notify_all();
                    }
                });
            }
        });
        auto channel = sender->createDataChannel("input");
        channel->onMessage([](rtc::binary) {}, [weakState](std::string value) {
            if (auto s = weakState.lock()) {
                std::lock_guard lock(s->mutex);
                assert(value == "while-media-blocked");
                s->outboundControl = true; s->cv.notify_all();
            }
        });
        const auto deadline = Clock::now() + 10s;
        while ((!track->isOpen() || !channel->isOpen()) && Clock::now() < deadline) std::this_thread::sleep_for(5ms);
        assert(track->isOpen() && channel->isOpen());
        { std::unique_lock lock(state->mutex); assert(state->cv.wait_for(lock, 5s, [&] { return state->channel && state->channel->isOpen(); })); }
        rtc::binary packet = {std::byte{0x80}, std::byte{0xe0}, std::byte{0}, std::byte{1},
            std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1},
            std::byte{0}, std::byte{0}, std::byte{4}, std::byte{0xd2},
            std::byte{0x65}, std::byte{1}, std::byte{2}, std::byte{3}};
        track->send(packet);
        {
            std::unique_lock lock(state->mutex);
            assert(state->cv.wait_for(lock, 5s, [&] { return state->received == 1; }));
        }
        constexpr unsigned mediaCount = 193;
        for (unsigned int i = 2; i <= mediaCount; ++i) {
            packet[2] = std::byte(i >> 8);
            packet[3] = std::byte(i & 255);
            track->send(packet);
        }
        rtc::DtlsDiagnostics occupied;
        while (Clock::now() < deadline) {
            occupied = receiver->getDtlsDiagnostics();
            if (occupied.queueDepth >= mediaCount - 1) break;
            std::this_thread::sleep_for(2ms);
        }
        assert(occupied.available && occupied.queueDepth >= mediaCount - 1 && occupied.queueHighWater >= mediaCount - 1);
        // A real SCTP/DTLS control send completes while the media callback is
        // deliberately held. Releasing it on timeout avoids hanging a failing test.
        auto outbound = std::async(std::launch::async, [&] {
            return state->channel->send(std::string("while-media-blocked"));
        });
        const bool completedOutsideMedia = outbound.wait_for(1s) == std::future_status::ready;
        if (!completedOutsideMedia) {
            std::lock_guard lock(state->mutex); state->release = true; state->cv.notify_all();
        }
        outbound.get();
        assert(completedOutsideMedia);
        {
            std::unique_lock lock(state->mutex);
            assert(state->cv.wait_for(lock, 1s, [&] { return state->outboundControl; }));
        }
        channel->send(std::string("after-media-flood"));
        std::this_thread::sleep_for(30ms);
        assert(receiver->getDtlsDiagnostics().queueOldestAgeUs >= 20000);
        {
            std::lock_guard lock(state->mutex);
            state->release = true;
            state->cv.notify_all();
        }
        {
            std::unique_lock lock(state->mutex);
            assert(state->cv.wait_for(lock, 5s, [&] { return state->received == mediaCount && state->inboundControl; }));
        }
        rtc::DtlsDiagnostics drained;
        do {
            drained = receiver->getDtlsDiagnostics();
            if (drained.queueDepth == 0 && drained.recvProcessingMaxUs >= 20000) break;
            std::this_thread::sleep_for(2ms);
        } while (Clock::now() < deadline);
        assert(drained.dequeued >= mediaCount && drained.residenceMaxUs >= 20000);
        assert(drained.recvProcessingMaxUs >= 20000 && drained.recvRuns > 0);
        assert(drained.recvScheduleCount > 0 && drained.queueDepth == 0);
        assert(drained.sendSslLockCount > 0 && drained.recvSslLockCount > 0);
        assert(drained.recvBatchYields > 0 && drained.recvBatchPacketsMax <= 64);
        assert(drained.recvSslLockHoldMaxUs < drained.recvProcessingMaxUs);
        assert(drained.recvDemuxCount >= mediaCount && drained.recvUnprotectCount == mediaCount);
        assert(drained.recvMediaCallbackCount == mediaCount && drained.recvMediaCallbackMaxUs >= 20000);
        assert(drained.recvDemuxMaxUs >= drained.recvMediaCallbackMaxUs);
        assert(drained.recvDataCallbackCount > 0);
        assert(drained.recvBatchTimeYields + drained.recvBatchWorkYields == drained.recvBatchYields);
        assert(drained.admissionMediaDropped == 0 && drained.admissionCriticalOverflow == 0);
        std::printf("Real mbedTLS/SRTP/SCTP: ordered %u media, bidirectional control during/after flood, SSL hold max=%llu us, callback batch max=%llu us, yields=%llu PASS\n",
                    mediaCount, (unsigned long long)drained.recvSslLockHoldMaxUs,
                    (unsigned long long)drained.recvProcessingMaxUs, (unsigned long long)drained.recvBatchYields);
        std::atomic<bool> stop{false}, lost{false};
        std::thread reader([&] {
            while (!stop.load()) if (!receiver->getDtlsDiagnostics().available) lost = true;
        });
        {
            std::lock_guard lock(state->mutex); state->release = false;
        }
        packet[3] = std::byte((mediaCount + 1) & 255);
        track->send(packet);
        {
            std::unique_lock lock(state->mutex);
            assert(state->cv.wait_for(lock, 5s, [&] { return state->received == mediaCount + 1; }));
        }
        // A second, encrypted RTP wave exceeds 1024 while the callback is held.
        // Pace local sends to exercise ingress admission rather than UDP socket
        // drops. RTP's trailing 440 packets are deliberately shed by this queue.
        constexpr unsigned floodCount = 1400;
        for (unsigned i = mediaCount + 2; i <= mediaCount + 1 + floodCount; ++i) {
            packet[2] = std::byte(i >> 8); packet[3] = std::byte(i & 255);
            track->send(packet);
            std::this_thread::sleep_for(200us);
        }
        const auto saturatedDeadline = Clock::now() + 5s;
        rtc::DtlsDiagnostics saturated;
        do {
            saturated = receiver->getDtlsDiagnostics();
            if (saturated.admissionMediaDropped == floodCount - 960) break;
            std::this_thread::sleep_for(2ms);
        } while (Clock::now() < saturatedDeadline);
        assert(saturated.queueDepth == 960 && saturated.admissionMediaDropped == floodCount - 960);
        assert(saturated.fullPushWaitCount == 0 && saturated.admissionCriticalOverflow == 0);
        channel->send(std::string("saturated-control"));
        // Reverse real TLS control must still complete while inbound media is
        // saturated; incoming DTLS uses reserved slots and waits in FIFO order.
        {
            std::lock_guard lock(state->mutex); state->outboundControl = false;
        }
        state->channel->send(std::string("while-media-blocked"));
        {
            std::unique_lock lock(state->mutex);
            assert(state->cv.wait_for(lock, 2s, [&] { return state->outboundControl; }));
            state->release = true; state->cv.notify_all();
            assert(state->cv.wait_for(lock, 5s, [&] {
                return state->received == mediaCount + 1 + 960 && state->saturatedControl;
            }));
            // Next test packet follows the 440 packets shed at admission.
            state->expectedSequence = mediaCount + 2 + floodCount;
            state->release = false;
        }
        std::printf("Real saturated SRTP/SCTP: 1400 queued RTP attempts, 960 admitted, 440 counted media drops; bidirectional TLS control preserved PASS\n");
        const unsigned finalSequence = mediaCount + 2 + floodCount;
        packet[2] = std::byte(finalSequence >> 8); packet[3] = std::byte(finalSequence & 255);
        track->send(packet);
        {
            std::unique_lock lock(state->mutex);
            assert(state->cv.wait_for(lock, 5s, [&] { return state->received == mediaCount + 2 + 960; }));
        }
        auto closing = std::async(std::launch::async, [&] { receiver->close(); });
        std::this_thread::sleep_for(30ms);
        {
            std::lock_guard lock(state->mutex); state->release = true; state->cv.notify_all();
        }
        assert(closing.wait_for(5s) == std::future_status::ready);
        closing.get();
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
