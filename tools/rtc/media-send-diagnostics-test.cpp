#include <rtc/rtc.hpp>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

class DiagnosticReceiver final : public rtc::MediaHandler {
public:
    explicit DiagnosticReceiver(std::weak_ptr<rtc::PeerConnection> peer) : peer_(std::move(peer)) {}
    void incoming(rtc::message_vector &messages, const rtc::message_callback &) override {
        for (const auto &message : messages) {
            // Snapshot from inside the callback must not wait on the current
            // media/TLS lock, even while another thread reads diagnostics.
            if (auto peer = peer_.lock()) assert(peer->getDtlsDiagnostics().available);
            std::this_thread::sleep_for(message->type == rtc::Message::Control ? 30ms : 15ms);
        }
        messages.clear();
    }
private:
    std::weak_ptr<rtc::PeerConnection> peer_;
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
        sender->onLocalDescription([weakReceiver](rtc::Description d) { if (auto p = weakReceiver.lock()) p->setRemoteDescription(d); });
        receiver->onLocalDescription([weakSender](rtc::Description d) { if (auto p = weakSender.lock()) p->setRemoteDescription(d); });
        sender->onLocalCandidate([weakReceiver](rtc::Candidate c) { if (auto p = weakReceiver.lock()) p->addRemoteCandidate(c); });
        receiver->onLocalCandidate([weakSender](rtc::Candidate c) { if (auto p = weakSender.lock()) p->addRemoteCandidate(c); });
        struct TrackState { std::mutex mutex; std::shared_ptr<rtc::Track> track; };
        auto trackState = std::make_shared<TrackState>();
        receiver->onTrack([weakReceiver, trackState](std::shared_ptr<rtc::Track> track) {
            { std::lock_guard lock(trackState->mutex); trackState->track = track; }
            track->setMediaHandler(std::make_shared<DiagnosticReceiver>(weakReceiver));
        });
        rtc::Description::Video video("video", rtc::Description::Direction::SendOnly);
        video.addH264Codec(96);
        video.addSSRC(1234, "test", "stream", "track");
        auto track = sender->addTrack(video);
        sender->setLocalDescription();
        const auto deadline = Clock::now() + 10s;
        while (!track->isOpen() && Clock::now() < deadline) std::this_thread::sleep_for(2ms);
        assert(track->isOpen());
        std::atomic<bool> stop{false};
        std::thread snapshotReader([&] {
            while (!stop.load()) {
                const auto d = receiver->getDtlsDiagnostics();
                assert(d.recvMediaCallbackCount == d.recvRtpCallbackCount + d.recvRtcpCallbackCount);
                assert(d.recvMediaCallbackTotalUs == d.recvRtpCallbackTotalUs + d.recvRtcpCallbackTotalUs);
                std::this_thread::yield();
            }
        });
        rtc::binary rtp = {std::byte{0x80}, std::byte{0xe0}, std::byte{0}, std::byte{1},
            std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1},
            std::byte{0}, std::byte{0}, std::byte{4}, std::byte{0xd2}, std::byte{0x65}};
        rtc::binary sr(28, std::byte{0});
        sr[0] = std::byte{0x80}; sr[1] = std::byte{200}; sr[3] = std::byte{6};
        sr[6] = std::byte{4}; sr[7] = std::byte{0xd2};
        assert(track->send(rtp));
        assert(track->send(sr));
        rtc::DtlsDiagnostics received;
        do {
            received = receiver->getDtlsDiagnostics();
            if (received.recvRtpCallbackCount == 1 && received.recvRtcpCallbackCount == 1) break;
            std::this_thread::sleep_for(2ms);
        } while (Clock::now() < deadline);
        stop = true; snapshotReader.join();
        assert(received.recvRtpCallbackCount == 1 && received.recvRtcpCallbackCount == 1);
        assert(received.recvRtpCallbackTotalUs >= 10000 && received.recvRtcpCallbackTotalUs >= 20000);
        assert(received.recvMediaCallbackCount == 2 && received.recvUnprotectCount == 2);
        assert(received.recvMediaCallbackTotalUs == received.recvRtpCallbackTotalUs + received.recvRtcpCallbackTotalUs);
        const auto sent = sender->getDtlsDiagnostics();
        assert(sent.sendSrtpLockCount == 2 && sent.sendSrtpRejected == 0 && sent.sendSrtpFailed == 0);
        assert(sent.sendRtpCount == 1 && sent.sendRtcpCount == 1);
        assert(sent.sendRtpProtectCount == 1 && sent.sendRtcpProtectCount == 1);
        assert(sent.sendRtpLowerCount == 1 && sent.sendRtcpLowerCount == 1);
        assert(sent.sendSrtpLockHoldTotalUs >= sent.sendRtpProtectTotalUs + sent.sendRtcpProtectTotalUs + sent.sendRtpLowerTotalUs + sent.sendRtcpLowerTotalUs);
        std::printf("Media diagnostics: RTP callbacks=%llu RTCP callbacks=%llu; SRTP/RTCP protect/lower sends counted separately; reentrant/concurrent snapshots PASS\n",
            (unsigned long long)received.recvRtpCallbackCount, (unsigned long long)received.recvRtcpCallbackCount);
        track->close(); sender->close(); receiver->close();
    }
    auto cleanup = rtc::Cleanup();
    assert(cleanup.wait_for(5s) == std::future_status::ready);
    rtc::JoinCleanupThreads(); cleanup.get();
}