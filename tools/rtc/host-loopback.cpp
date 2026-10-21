#include <rtc/rtc.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include <string>

#ifndef RTC_LOOPBACK_STAGE
#define RTC_LOOPBACK_STAGE(text) std::fprintf(stderr, "RTC stage: %s\n", text)
#endif

namespace {
struct ProbeState {
    std::mutex mutex;
    std::condition_variable cv;
    bool data = false, media = false, websocket = false, failed = false;
    std::shared_ptr<rtc::DataChannel> channel;
    std::shared_ptr<rtc::Track> track;
    std::shared_ptr<rtc::WebSocket> socket;
};
using State = std::shared_ptr<ProbeState>;

void fail(const std::weak_ptr<ProbeState>& weak) {
    if (auto state = weak.lock()) {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->failed = true; state->cv.notify_all();
    }
}

// Callbacks own no stack references or peer wrappers. A callback that outlives
// an exceptional exit can only lock its independent state or an existing peer.
struct PeerOutcome { bool data, media, success; };
PeerOutcome peerPhase() {
    auto state = std::make_shared<ProbeState>();
    std::weak_ptr<ProbeState> weak = state;
    rtc::Configuration config;
    config.bindAddress = "127.0.0.1";
    config.enableIceTcp = false;
    config.iceServers.clear();
    auto sender = std::make_shared<rtc::PeerConnection>(config);
    auto receiver = std::make_shared<rtc::PeerConnection>(config);
    std::weak_ptr<rtc::PeerConnection> weakSender = sender, weakReceiver = receiver;
    sender->onLocalDescription([weakReceiver, weak](rtc::Description d) {
        try { if (auto peer = weakReceiver.lock()) peer->setRemoteDescription(d); }
        catch (...) { fail(weak); }
    });
    receiver->onLocalDescription([weakSender, weak](rtc::Description d) {
        try { if (auto peer = weakSender.lock()) peer->setRemoteDescription(d); }
        catch (...) { fail(weak); }
    });
    sender->onLocalCandidate([weakReceiver, weak](rtc::Candidate c) {
        try { if (auto peer = weakReceiver.lock()) peer->addRemoteCandidate(c); }
        catch (...) { fail(weak); }
    });
    receiver->onLocalCandidate([weakSender, weak](rtc::Candidate c) {
        try { if (auto peer = weakSender.lock()) peer->addRemoteCandidate(c); }
        catch (...) { fail(weak); }
    });
    auto onState = [weak](rtc::PeerConnection::State value) {
        if (value == rtc::PeerConnection::State::Failed) fail(weak);
    };
    sender->onStateChange(onState); receiver->onStateChange(onState);
    receiver->onDataChannel([weak](std::shared_ptr<rtc::DataChannel> dc) {
        if (auto s = weak.lock()) {
            { std::lock_guard<std::mutex> lock(s->mutex); s->channel = dc; }
            dc->onMessage([weak](rtc::message_variant value) {
                if (auto text = std::get_if<std::string>(&value); text && *text == "rtc-loopback")
                    if (auto s = weak.lock()) {
                        std::lock_guard<std::mutex> lock(s->mutex); s->data = true; s->cv.notify_all();
                    }
            });
        }
    });
    receiver->onTrack([weak](std::shared_ptr<rtc::Track> track) {
        if (auto s = weak.lock()) {
            { std::lock_guard<std::mutex> lock(s->mutex); s->track = track; }
            track->onMessage([weak](rtc::binary packet) {
                if (packet.size() == 16 && packet[12] == std::byte{0x65})
                    if (auto s = weak.lock()) {
                        std::lock_guard<std::mutex> lock(s->mutex); s->media = true; s->cv.notify_all();
                    }
            }, [](std::string) {});
        }
    });
    rtc::Description::Video video("video", rtc::Description::Direction::SendOnly);
    video.addH264Codec(96);
    video.addSSRC(1234, "rtc-test", "stream", "track");
    auto track = sender->addTrack(video);
    auto channel = sender->createDataChannel("input");
    channel->onOpen([dc = std::weak_ptr<rtc::DataChannel>(channel), weak] {
        try { if (auto channel = dc.lock()) channel->send(std::string("rtc-loopback")); }
        catch (...) { fail(weak); }
    });
    bool sent = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline) {
        // No additional media thread: synchronous send exceptions unwind into
        // the probe's outer boundary, and cannot call std::terminate.
        if (!sent && track->isOpen()) {
            rtc::binary packet = {std::byte{0x80}, std::byte{0xe0}, std::byte{0}, std::byte{1},
                std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1},
                std::byte{0}, std::byte{0}, std::byte{4}, std::byte{0xd2},
                std::byte{0x65}, std::byte{1}, std::byte{2}, std::byte{3}};
            track->send(packet); sent = true;
        }
        std::unique_lock<std::mutex> lock(state->mutex);
        if (state->failed || (state->data && state->media)) break;
        state->cv.wait_for(lock, std::chrono::milliseconds(25));
    }
    PeerOutcome outcome{};
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        outcome = {state->data, state->media, !state->failed && state->data && state->media};
#ifdef RTC_LOOPBACK_REPORT
        RTC_LOOPBACK_REPORT(state->data, state->media, false);
#endif
    }
    channel->close(); track->close(); sender->close(); receiver->close();
    return outcome;
}

bool websocketPhase() {
    auto state = std::make_shared<ProbeState>();
    std::weak_ptr<ProbeState> weak = state;
    rtc::WebSocketServer::Configuration config;
    config.port = 0; config.bindAddress = "127.0.0.1";
    rtc::WebSocketServer server(config);
    server.onClient([weak](std::shared_ptr<rtc::WebSocket> ws) {
        if (auto state = weak.lock()) {
            { std::lock_guard<std::mutex> lock(state->mutex); state->socket = ws; }
            ws->onMessage([socket = std::weak_ptr<rtc::WebSocket>(ws), weak](rtc::message_variant msg) {
                try { if (auto ws = socket.lock()) ws->send(msg); }
                catch (...) { fail(weak); }
            });
        }
    });
    auto client = std::make_shared<rtc::WebSocket>();
    client->onOpen([socket = std::weak_ptr<rtc::WebSocket>(client), weak] {
        try { if (auto ws = socket.lock()) ws->send(std::string("ws-loopback")); }
        catch (...) { fail(weak); }
    });
    client->onError([weak](std::string) { fail(weak); });
    client->onMessage([weak](rtc::message_variant msg) {
        if (auto text = std::get_if<std::string>(&msg); text && *text == "ws-loopback")
            if (auto state = weak.lock()) {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->websocket = true; state->cv.notify_all();
            }
    });
    client->open("ws://127.0.0.1:" + std::to_string(server.port()) + "/");
    bool success;
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        state->cv.wait_for(lock, std::chrono::seconds(15), [&state] { return state->websocket || state->failed; });
        success = state->websocket && !state->failed;
    }
    client->close(); server.stop();
    return success;
}

bool cleanupPhase() {
    auto cleanup = rtc::Cleanup();
    if (cleanup.wait_for(std::chrono::seconds(5)) != std::future_status::ready) return false;
    rtc::JoinCleanupThreads();
    cleanup.get(); // Ready does not imply successful cleanup.
    return true;
}
}

int main() {
    rtc::SetThreadPoolSize(2);
#ifdef RTC_LOOPBACK_LOG
    rtc::InitLogger(rtc::LogLevel::Warning, RTC_LOOPBACK_LOG);
#else
    rtc::InitLogger(rtc::LogLevel::Warning);
#endif
    RTC_LOOPBACK_STAGE("WebRTC peers begin");
    const PeerOutcome peers = peerPhase();
    RTC_LOOPBACK_STAGE("WebRTC peers returned; cleanup begin");
    if (!cleanupPhase()) return 2;
    RTC_LOOPBACK_STAGE("WebRTC cleanup complete");
    // Serial phases avoid holding two 1 MiB ICE send/receive pairs alongside
    // TCP WebSocket buffers in the same bounded Horizon BSD memory pool.
    RTC_LOOPBACK_STAGE("WebSocket begin");
    const bool websocket = websocketPhase();
    RTC_LOOPBACK_STAGE("WebSocket returned; cleanup begin");
    if (!cleanupPhase()) return 2;
    RTC_LOOPBACK_STAGE("WebSocket cleanup complete");
#ifdef RTC_LOOPBACK_REPORT
    RTC_LOOPBACK_REPORT(peers.data, peers.media, websocket);
#endif
    std::printf("WebRTC data=%d SRTP=%d WebSocket=%d cleanup=1\n", peers.data, peers.media, websocket);
    return peers.success && websocket ? 0 : 1;
}

