#include "features/streaming/infrastructure/stream.hpp"
#include "features/streaming/infrastructure/stream_protocol.hpp"
#include "features/streaming/domain/h264_rtp.hpp"
#include "features/streaming/infrastructure/stream_queue.hpp"
#include "features/streaming/infrastructure/control_queue.hpp"
#include "features/streaming/infrastructure/incoming_control.hpp"
#include "features/streaming/infrastructure/transport_history.hpp"
#include "features/streaming/domain/auto_quality.hpp"
#include <functional>
#include <cmath>
#include <cstdio>
#include <rtc/rtc.hpp>
#include <algorithm>
#include <chrono>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace stream {
using Clock = std::chrono::steady_clock;
using Json = protocol::Json;
// Only fixed client-generated text may use this exception. Parser/library errors
// can contain SDP, session hashes or TURN credentials and remain sanitized below.
struct TransportError final : std::runtime_error {
    bool retryable;
    explicit TransportError(const char* message, bool mayRetry = false)
        : std::runtime_error(message), retryable(mayRetry) {}
};
// Consume RTP only after the chained RTCP receiver has seen the original packet.
// Ownership and timeout handling live in Shared; this callback never sends PLI.
class VideoRtpHandler final : public rtc::MediaHandler {
public:
    explicit VideoRtpHandler(std::function<void(const std::byte*, std::size_t)> receive)
        : receive_(std::move(receive)) {}
    void incoming(rtc::message_vector& messages, const rtc::message_callback&) override {
        for (const auto& message : messages)
            if (message->type == rtc::Message::Binary) receive_(message->data(), message->size());
        messages.clear();
    }
private:
    std::function<void(const std::byte*, std::size_t)> receive_;
};
static std::uint64_t videoNow() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}
struct Client::Impl {
    enum class EventType { WsOpen, WsData, WsClosed, Failure, Description, Candidate,
                           Channel, ChannelOpen, Control, PeerState, IceState, GatheringState, Track };
    struct Event {
        EventType type;
        std::uint64_t arrivedMs = 0;
        std::string text;
        std::string mid;
        std::shared_ptr<rtc::DataChannel> channel;
        std::shared_ptr<rtc::Track> track;
        rtc::PeerConnection::State peerState = rtc::PeerConnection::State::New;
        rtc::PeerConnection::IceState iceState = rtc::PeerConnection::IceState::New;
        rtc::PeerConnection::GatheringState gatheringState = rtc::PeerConnection::GatheringState::New;
        explicit Event(EventType type_) : type(type_) {}
    };
    struct Shared {
        std::mutex mutex;
        bool stopped = false;
        bool eventOverflow = false;
        bool callbackFailure = false;
        IncomingControl::Kind terminalControl = IncomingControl::Kind::Ignored;
        std::size_t eventBytes = 0, eventHigh = 0;
        std::uint64_t eventDropped = 0;
        std::deque<Event> events;
        MediaQueue media;
        TransportHistory transportHistory;
        IncomingControl incoming;
        std::uint64_t qualityTimeOffset = 0, eventDelayHighMs = 0;
        Clock::time_point lastVideo{};
        H264RtpAssembler video;
        bool videoTrack = false;
        std::uint64_t videoFault = 0, videoGeneration = 0, queueResyncs = 0, queueCapacity = 0, queueAge = 0;
        std::uint64_t pliRequests = 0, receivedVideoBytes = 0, lastRtpMs = 0, lastAudioMs = 0, lastControlMs = 0;
        std::uint64_t lastPeerMs = 0, lastIceMs = 0, lastGatheringMs = 0;
        rtc::PeerConnection::State peerState = rtc::PeerConnection::State::New;
        rtc::PeerConnection::IceState iceState = rtc::PeerConnection::IceState::New;
        rtc::PeerConnection::GatheringState gatheringState = rtc::PeerConnection::GatheringState::New;
        void reconcileFault() {
            if (videoFault == video.stats().faultSerial) return;
            media.clearVideo(); videoFault = video.stats().faultSerial; ++videoGeneration;
        }
        void resyncQueue(bool capacity, std::uint64_t now) {
            ++queueResyncs;
            if (capacity) ++queueCapacity; else ++queueAge;
            media.clearVideo(); video.requestResync(now); reconcileFault();
        }
        void expireMedia(std::uint64_t now) {
            media.expireAudio(now);
            if (media.age(Kind::Video, now) > MediaQueue::MaxVideoAgeMs) resyncQueue(false, now);
        }
        void drainVideo() {
            const auto now = videoNow();
            expireMedia(now); reconcileFault();
            H264RtpAssembler::Frame frame;
            while (video.poll(frame)) {
                Packet packet{Kind::Video, std::move(frame.data), frame.timestamp,
                              frame.firstSequence, frame.lastSequence, now, videoGeneration};
                if (!media.push(std::move(packet), now)) { resyncQueue(true, now); return; }
                lastVideo = Clock::now();
            }
        }
        void videoRtp(const std::byte* bytes, std::size_t size) {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopped) return;
            lastRtpMs = videoNow(); receivedVideoBytes += size;
            expireMedia(lastRtpMs);
            video.receive(bytes, size, lastRtpMs); drainVideo();
        }
        std::string videoDiagnostics() const {
            const auto now = videoNow();
            return video.diagnostics() + " queue=" + std::to_string(media.packets.size()) +
                " queueBytes=" + std::to_string(media.videoBytes + media.audioBytes) +
                " videoQueue=" + std::to_string(media.videoCount) + " audioQueue=" + std::to_string(media.audioCount) +
                " videoAgeMs=" + std::to_string(media.age(Kind::Video, now)) +
                " audioAgeMs=" + std::to_string(media.age(Kind::Audio, now)) +
                " videoAgeHighMs=" + std::to_string(media.videoAgeHighMs) +
                " audioAgeHighMs=" + std::to_string(media.audioAgeHighMs) +
                " videoHigh=" + std::to_string(media.videoHigh) + " audioHigh=" + std::to_string(media.audioHigh) +
                " queueBytesHigh=" + std::to_string(media.bytesHigh) +
                " queueResync=" + std::to_string(queueResyncs) + " queueCapacity=" + std::to_string(queueCapacity) +
                " queueAge=" + std::to_string(queueAge) + " audioDrop=" + std::to_string(media.audioDropped) +
                " audioReject=" + std::to_string(media.audioRejected) + " videoGeneration=" + std::to_string(videoGeneration) +
                " pli=" + std::to_string(pliRequests);
        }
        std::string transportDiagnostics() const {
            const auto now = videoNow();
            const auto age = [now](std::uint64_t at) -> long long { return at ? static_cast<long long>(now - at) : -1; };
            return " peerState=" + std::to_string(static_cast<int>(peerState)) +
                " peerAgeMs=" + std::to_string(age(lastPeerMs)) +
                " iceState=" + std::to_string(static_cast<int>(iceState)) + " iceAgeMs=" + std::to_string(age(lastIceMs)) +
                " gatheringState=" + std::to_string(static_cast<int>(gatheringState)) +
                " gatheringAgeMs=" + std::to_string(age(lastGatheringMs)) +
                " rtpAgeMs=" + std::to_string(age(lastRtpMs)) + " audioReceiveAgeMs=" + std::to_string(age(lastAudioMs)) +
                " controlAgeMs=" + std::to_string(age(lastControlMs)) +
                " eventQueue=" + std::to_string(events.size()) + " eventBytes=" + std::to_string(eventBytes) +
                " eventHigh=" + std::to_string(eventHigh) + " eventDrop=" + std::to_string(eventDropped) +
                " eventOverflow=" + std::to_string(eventOverflow) + " callbackFailure=" + std::to_string(callbackFailure) +
                transportHistory.diagnostics() + incoming.diagnostics() +
                " eventDelayHighMs=" + std::to_string(eventDelayHighMs);
        }
        void event(Event e) {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopped) return;
            const auto now = videoNow();
            if (e.type == EventType::PeerState) { peerState = e.peerState; lastPeerMs = now; }
            if (e.type == EventType::IceState) { iceState = e.iceState; lastIceMs = now; }
            if (e.type == EventType::GatheringState) { gatheringState = e.gatheringState; lastGatheringMs = now; }
            const auto age = [now](std::uint64_t at) -> long long {
                return at ? static_cast<long long>(now - at) : -1;
            };
            using HistoryKind = TransportHistory::Kind;
            if (e.type == EventType::PeerState)
                transportHistory.record(HistoryKind::Peer, static_cast<int>(e.peerState), now,
                    e.peerState == rtc::PeerConnection::State::Failed || e.peerState == rtc::PeerConnection::State::Closed,
                    age(lastRtpMs), age(lastAudioMs), age(lastControlMs));
            else if (e.type == EventType::IceState)
                transportHistory.record(HistoryKind::Ice, static_cast<int>(e.iceState), now,
                    e.iceState == rtc::PeerConnection::IceState::Failed || e.iceState == rtc::PeerConnection::IceState::Closed,
                    age(lastRtpMs), age(lastAudioMs), age(lastControlMs));
            else if (e.type == EventType::GatheringState)
                transportHistory.record(HistoryKind::Gathering, static_cast<int>(e.gatheringState), now);
            else if (e.type == EventType::ChannelOpen)
                transportHistory.record(HistoryKind::ChannelOpen, 0, now);
            else if (e.type == EventType::WsClosed || e.type == EventType::Failure) {
                // Codes identify only fixed locally generated callback failures.
                const int code = e.text == "Control channel closed" ? 1 : e.text == "Control channel failed" ? 2 :
                    e.text == "Video track closed" ? 3 : e.text == "Audio track closed" ? 4 :
                    e.text == "Video track failed" ? 5 : e.text == "Audio track failed" ? 6 :
                    e.text == "Signaling transport failed" ? 7 : 0;
                transportHistory.record(e.type == EventType::WsClosed ? HistoryKind::Closed : HistoryKind::Failure,
                    code, now, true, age(lastRtpMs), age(lastAudioMs), age(lastControlMs));
            }
            e.arrivedMs = now;
            const auto bytes = e.text.size() + e.mid.size();
            if (events.size() >= 128 || eventBytes + bytes > 1024 * 1024) { eventOverflow = true; ++eventDropped; return; }
            eventBytes += bytes; events.push_back(std::move(e));
            eventHigh = std::max(eventHigh, events.size());
        }
        void controlMessage(const std::string& text, const std::weak_ptr<rtc::DataChannel>& weakChannel) {
            const auto message = IncomingControl::classify(text);
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (stopped) return;
                lastControlMs = videoNow();
                incoming.receive(message, lastControlMs + qualityTimeOffset);
                if (message.kind == IncomingControl::Kind::Close || message.kind == IncomingControl::Kind::Error ||
                    message.kind == IncomingControl::Kind::Invalid) terminalControl = message.kind;
            }
            if (message.kind == IncomingControl::Kind::Ping) {
                bool sent = false;
                try {
                    // Local libdatachannel delivers messages outside its send mutex.
                    // Never send while holding Shared::mutex, including teardown races.
                    if (const auto dc = weakChannel.lock(); dc && dc->isOpen()) {
                        if (dc->bufferedAmount() <= 64 * 1024) {
                            dc->send(message.pong); // false is accepted into RTC's send queue.
                            sent = true;
                        }
                    }
                } catch (...) {}
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (stopped) return;
                    if (sent) incoming.pongSent(); else incoming.pongFailed();
                }
                if (!sent) {
                    Event e(EventType::Failure); e.text = "Ping response transport failed"; event(std::move(e));
                }
            } else if (message.kind == IncomingControl::Kind::Close ||
                       message.kind == IncomingControl::Kind::Error ||
                       message.kind == IncomingControl::Kind::Invalid) {
                Event e(EventType::Control);
                e.text = message.kind == IncomingControl::Kind::Close ? "close" :
                    message.kind == IncomingControl::Kind::Error ? "error" : "invalid";
                event(std::move(e));
            }
        }
        void packet(Kind kind, rtc::binary bytes, std::uint32_t timestamp) {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopped) return;
            const auto now = videoNow();
            expireMedia(now);
            if (kind == Kind::Audio) lastAudioMs = now;
            if (!media.push({kind, std::move(bytes), timestamp, 0, 0, now, videoGeneration}, now))
                resyncQueue(true, now);
        }
    };
    template<class F> static void callback(const std::weak_ptr<Shared>& weak, F&& fn) noexcept {
        auto shared = weak.lock();
        if (!shared) return;
        {
            std::lock_guard<std::mutex> lock(shared->mutex);
            if (shared->stopped) return;
        }
        try { fn(*shared); }
        catch (const std::exception&) {
            std::lock_guard<std::mutex> lock(shared->mutex);
            shared->callbackFailure = true;
        } catch (...) {
            // Unknown callback failures must also reach the owner, without leaking SDP/credentials.
            std::lock_guard<std::mutex> lock(shared->mutex);
            shared->callbackFailure = true;
        }
    }
    std::shared_ptr<Shared> shared;
    std::shared_ptr<rtc::WebSocket> ws;
    std::shared_ptr<rtc::PeerConnection> peer;
    std::shared_ptr<rtc::DataChannel> channel;
    std::vector<std::shared_ptr<rtc::Track>> tracks;
    ControlQueue controls;
    std::uint64_t controlTimeOffset = 0;
#ifdef FOG_STREAM_TESTING
    bool controlBlocked = false;
#endif
    std::uint64_t controlNow() const { return videoNow() + controlTimeOffset; }
    std::vector<Json> pendingCandidates;
    std::vector<std::string> mids;
    protocol::Parser parser;
    std::string hash, clientHash, suffix, state = "Idle", failure, pendingTransportFailure;
    Clock::time_point started{}, lastWs{}, lastHeartbeat{}, lastPli{};
    std::chrono::milliseconds heartbeat{0}, incomingHeartbeat{0};
    bool running = false, stompConnected = false, remoteSet = false, channelOpen = false;
    bool isReady = false, decoderWaiting = false, retryableFailure = false, closingForRetry = false;
    fog::video::Settings videoSettings;
    VideoCodec selectedCodec = VideoCodec::H264;
    bool qualityPending = true;
    unsigned qualityRequests = 0;
    Clock::time_point qualityAttempt{}, rateAt{};
    std::uint64_t lastRateBytes = 0;
    double receiveMbps = 0;
    quality::Telemetry serverStats;
    quality::Controller autoQuality;
    std::deque<std::string> qualityEvents;
    void qualityEvent(std::string event) {
        if (qualityEvents.size() >= 32) qualityEvents.pop_front();
        qualityEvents.push_back(std::move(event));
    }
    std::uint64_t qualityTimeOffset = 0;
    bool rateKnown = false;
    std::uint64_t qualityNow() const { return videoNow() + qualityTimeOffset; }
    std::string qualityDiagnostics() const {
        const auto now = qualityNow();
        char numbers[650];
        std::snprintf(numbers, sizeof(numbers),
            " codec=%s requestedCodec=%s capMbps=%d quality=%d pliSeconds=%d receiveMbps=%.3f serverTargetMbps=%.3f targetAgeMs=%lld serverPreset=%s presetAgeMs=%lld statsCount=%llu statsAgeMs=%lld serverAuto=%d autoAgeMs=%lld serverFps=%.2f fpsAgeMs=%lld serverCodec=%s codecAgeMs=%lld qualityRequests=%u qualityPending=%d",
            selectedCodec == VideoCodec::H265 ? "H265" : "H264",
            videoSettings.codec == VideoCodec::H265 ? "H265" : "H264", videoSettings.bitrateMbps,
            videoSettings.quality, videoSettings.keyframeSeconds, receiveMbps,
            serverStats.targetMbps.known ? serverStats.targetMbps.value : -1,
            static_cast<long long>(serverStats.targetMbps.age(now)), quality::name(serverStats.profile.value),
            static_cast<long long>(serverStats.profile.age(now)), static_cast<unsigned long long>(serverStats.count),
            serverStats.count ? static_cast<long long>(now - serverStats.at) : -1,
            serverStats.automatic.known ? static_cast<int>(serverStats.automatic.value) : -1,
            static_cast<long long>(serverStats.automatic.age(now)), serverStats.fps.known ? serverStats.fps.value : -1,
            static_cast<long long>(serverStats.fps.age(now)), serverStats.codec.known ? serverStats.codec.value.c_str() : "unknown",
            static_cast<long long>(serverStats.codec.age(now)), qualityRequests, qualityPending ? 1 : 0);
        return std::string(numbers) + autoQuality.diagnostics();
    }
    void readQualityStats(const quality::Telemetry& data) {
        // Each valid optional field owns its timestamp. Empty/malformed extensions
        // cannot refresh an old profile/target and never reach logs as raw text.
        const auto before = serverStats.profile.value;
        serverStats = data;
        if (before != serverStats.profile.value)
            qualityEvent(std::string("qualityEvent=stats-profile profile=") + quality::name(serverStats.profile.value));
    }
    void applyQuality() {
        if (!qualityPending || !channelOpen) return;
        const auto now = Clock::now();
        if (qualityAttempt != Clock::time_point{} && now - qualityAttempt < std::chrono::seconds(1)) return;
        // RTCP needs the real remote SSRC learned from video RTP first.
        { std::lock_guard<std::mutex> lock(shared->mutex);
          if (!shared->video.stats().received) return; }
        qualityAttempt = now;
        try {
            bool sent = false;
            for (const auto& track : tracks)
                if (track->description().type() == "video" && track->isOpen())
                    sent |= track->requestBitrate(static_cast<unsigned>(videoSettings.bitrateMbps) * 1000000u);
            if (!sent) return;
            static constexpr const char* commands[]{"set_autoquality", "set_lowquality", "set_midquality", "set_highquality", "set_ultraquality"};
            if (!control(Json{{"id", commands[videoSettings.quality]}}.dump())) return;
            ++qualityRequests;
            qualityPending = false; // Sent/queued, not an acknowledgment by the encoder.
        } catch (const std::exception&) {
            // Keep retryable UI changes separate from terminal streaming errors.
        }
    }
    std::string finalVideoDiagnostics, finalTransportDiagnostics;
    std::deque<std::string> finalVideoEvents;
    const char* operation = "idle";
    unsigned offers = 0, candidatesReceived = 0, emptyCandidates = 0, heartbeatsSent = 0;
    std::string diagnostics() const {
        return std::string("operation=") + operation + " offers=" + std::to_string(offers) +
            " candidates=" + std::to_string(candidatesReceived) + " emptyCandidates=" + std::to_string(emptyCandidates) +
            " hbOutMs=" + std::to_string(heartbeat.count()) + " hbInMs=" + std::to_string(incomingHeartbeat.count()) +
            " hbSent=" + std::to_string(heartbeatsSent) +
            " wsAgeMs=" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - lastWs).count()) +
            " controlPending=" + std::to_string(controls.size()) +
            " controlBytes=" + std::to_string(controls.bytes()) + " controlRejected=" + std::to_string(controls.rejected()) +
            transportDiagnostics();
    }
    std::string transportDiagnostics() const {
        if (!shared) return finalTransportDiagnostics;
        std::lock_guard<std::mutex> lock(shared->mutex);
        return shared->transportDiagnostics();
    }
    void failAt(const std::string& reason) {
        const auto detail = reason + " at " + operation;
        fail(detail.c_str());
    }

    template<class Send> static void transportSend(Send&& send, const char* reason) {
        try { send(); }
        catch (const std::runtime_error&) { throw TransportError(reason, true); }
        // Logic/size errors and allocation failures are not transport recovery.
    }
    void sendSignaling(const std::string& frame) {
        if (closingForRetry) return;
        transportSend([&] { ws->send(frame); }, "Signaling send failed");
    }
    void signal(const Json& payload) {
        if (!ws || !ws->isOpen() || !stompConnected) throw TransportError("Signaling unavailable", true);
        if (ws->bufferedAmount() > 256 * 1024) throw TransportError("Signaling backpressure", true);
        const auto frame = protocol::frame("SEND", {{"destination", "/topic/" + hash + "/server"},
                  {"content-type", "application/json"}, {"client-hash", clientHash}}, payload.dump());
        sendSignaling(frame);
    }
    void flushControl() {
        if (!channel || !channel->isOpen()) return;
        while (!controls.empty()) {
#ifdef FOG_STREAM_TESTING
            if (controlBlocked) break;
#endif
            if (channel->bufferedAmount() > 64 * 1024) break;
            // false means accepted into libdatachannel's queue, not lost.
            transportSend([&] { channel->send(controls.front().payload); }, "Control send failed");
            controls.pop(controlNow());
        }
        if (controls.stalled(controlNow())) throw TransportError("Control channel stalled", true);
    }
    bool control(const std::string& payload, ControlQueue::Mode mode = ControlQueue::Mode::Ordered,
                 const std::string& input = {}) {
        if (!channel || !channel->isOpen()) return false;
        flushControl();
        const bool accepted = controls.push(payload, mode, input, controlNow());
        flushControl();
        return accepted;
    }
    void openChannel() {
        if (closingForRetry || channelOpen || !channel || !channel->isOpen()) return;
        control(Json{{"id", "connection_ready"}}.dump());
        channelOpen = true;
        if (!isReady) state = "Waiting for video";
    }
    void stop(bool notify) {
        // No shutdown exception can prevent release of the remaining transports.
        if (notify) {
            closingForRetry = false; // A rejected retry follows ordinary final-close notification.
            try { control(protocol::gamepad(Gamepad{}).dump(), ControlQueue::Mode::Gamepad); }
            catch (const std::exception&) { if (failure.empty()) failure = "Neutral control send failed"; }
            try { if (stompConnected && ws && ws->isOpen()) signal({{"id", "close"}}); }
            catch (const std::exception&) { if (failure.empty()) failure = "Close notification failed"; }
        }
        running = false; isReady = false; channelOpen = false; stompConnected = false;
        // Queued events can own tracks/channels. Release them outside the queue lock:
        // their destruction may release RTC handles while a callback is draining.
        std::deque<Event> abandonedEvents;
        std::deque<Packet> abandonedPackets;
        if (shared) {
            std::lock_guard<std::mutex> lock(shared->mutex);
            shared->stopped = true;
            // Diagnostics must never interrupt transport teardown, including
            // allocation failure while snapshotting counters or event strings.
            try {
                finalVideoDiagnostics = shared->videoDiagnostics() + qualityDiagnostics();
                finalTransportDiagnostics = shared->transportDiagnostics();
                std::string videoEvent;
                while (shared->video.pollEvent(videoEvent)) finalVideoEvents.push_back(std::move(videoEvent));
            } catch (...) {}
            abandonedEvents.swap(shared->events);
            abandonedPackets.swap(shared->media.packets);
            shared->eventBytes = 0;
        }
        if (channel) {
            try { channel->resetCallbacks(); channel->close(); }
            catch (const std::exception&) { if (failure.empty()) failure = "Control shutdown failed"; }
            channel.reset();
        }
        for (auto& track : tracks) {
            try { track->resetCallbacks(); track->close(); }
            catch (const std::exception&) { if (failure.empty()) failure = "Track shutdown failed"; }
        }
        tracks.clear();
        if (peer) {
            try { peer->resetCallbacks(); peer->close(); }
            catch (const std::exception&) { if (failure.empty()) failure = "Peer shutdown failed"; }
            peer.reset();
        }
        if (ws) {
            try { ws->resetCallbacks(); ws->close(); }
            catch (const std::exception&) { if (failure.empty()) failure = "Signaling shutdown failed"; }
            ws.reset();
        }
        shared.reset();
        controls.clear(); pendingTransportFailure.clear(); decoderWaiting = false;
        pendingCandidates.clear(); mids.clear();
        state = failure.empty() ? "Closed" : "Failed";
        hash.clear(); clientHash.clear(); suffix.clear();
    }
    void fail(const char* reason, bool mayRetry = false) {
        // Terminal evidence wins over concurrent transport loss, even when the FIFO is full.
        if (mayRetry && shared) {
            std::lock_guard<std::mutex> lock(shared->mutex);
            if (shared->terminalControl == IncomingControl::Kind::Close) { reason = "Remote control closed"; mayRetry = false; }
            else if (shared->terminalControl == IncomingControl::Kind::Error) { reason = "Remote control error"; mayRetry = false; }
            else if (shared->terminalControl == IncomingControl::Kind::Invalid) { reason = "Malformed control message"; mayRetry = false; }
            else if (shared->eventOverflow) { reason = "Signaling/control event queue capacity exceeded"; mayRetry = false; }
            else if (shared->callbackFailure) { reason = "Stream callback failed"; mayRetry = false; }
        }
        if (failure.empty()) { failure = reason; retryableFailure = mayRetry; }
        // A recoverable transport loss must never send the remote close notification.
        stop(!retryableFailure);
    }
    void createPeer(const Json& init) {
        operation = "init.ice-servers";
        if (peer) throw TransportError("Duplicate peer initialization");
        rtc::Configuration config;
        config.disableAutoNegotiation = true;
        config.maxMessageSize = 64 * 1024;
        for (const auto& item : protocol::iceServers(init)) {
            rtc::IceServer ice(item.url);
            ice.username = item.username; ice.password = item.credential;
            config.iceServers.push_back(std::move(ice));
        }
        // A frozen retry shutdown validates pending protocol but starts no new transport.
        if (closingForRetry) return;
        operation = "init.peer-create";
        peer = std::make_shared<rtc::PeerConnection>(config);
        const std::weak_ptr<Shared> weak(shared);
        peer->onLocalDescription([weak](rtc::Description description) {
            callback(weak, [&description](Shared& s) {
                Event e(EventType::Description);
                e.text = Json{{"type", description.typeString()}, {"sdp", std::string(description)}}.dump();
                s.event(std::move(e));
            });
        });
        peer->onLocalCandidate([weak](rtc::Candidate candidate) {
            callback(weak, [&candidate](Shared& s) {
                Event e(EventType::Candidate); e.text = candidate.candidate(); e.mid = candidate.mid();
                s.event(std::move(e));
            });
        });
        peer->onStateChange([weak](rtc::PeerConnection::State next) {
            callback(weak, [next](Shared& s) {
                Event e(EventType::PeerState); e.peerState = next; s.event(std::move(e));
            });
        });
        peer->onIceStateChange([weak](rtc::PeerConnection::IceState next) {
            callback(weak, [next](Shared& s) {
                Event e(EventType::IceState); e.iceState = next; s.event(std::move(e));
            });
        });
        peer->onGatheringStateChange([weak](rtc::PeerConnection::GatheringState next) {
            callback(weak, [next](Shared& s) {
                Event e(EventType::GatheringState); e.gatheringState = next; s.event(std::move(e));
            });
        });
        peer->onDataChannel([weak](std::shared_ptr<rtc::DataChannel> dc) {
            callback(weak, [&dc, &weak](Shared& s) {
                dc->onOpen([weak] { callback(weak, [](Shared& st) { st.event(Event(EventType::ChannelOpen)); }); });
                dc->onClosed([weak] { callback(weak, [](Shared& st) {
                    Event e(EventType::Failure); e.text = "Control channel closed"; st.event(std::move(e));
                }); });
                dc->onError([weak](std::string) { callback(weak, [](Shared& st) {
                    Event e(EventType::Failure); e.text = "Control channel failed"; st.event(std::move(e));
                }); });
                const std::weak_ptr<rtc::DataChannel> weakChannel(dc);
                dc->onMessage([weak, weakChannel](rtc::message_variant msg) {
                    callback(weak, [&msg, &weakChannel](Shared& st) {
                        if (const auto text = std::get_if<std::string>(&msg))
                            st.controlMessage(*text, weakChannel);
                    });
                });
                Event e(EventType::Channel); e.channel = dc; s.event(std::move(e));
            });
        });
        peer->onTrack([weak](std::shared_ptr<rtc::Track> track) {
            callback(weak, [&track, &weak](Shared& s) {
                auto desc = track->description();
                if (desc.isRemoved()) return;
                desc.setDirection(rtc::Description::Direction::RecvOnly);
                track->setDescription(desc);
                Kind kind;
                std::shared_ptr<rtc::MediaHandler> depacketizer;
                if (desc.type() == "video") {
                    kind = Kind::Video;
                    {
                        std::lock_guard<std::mutex> lock(s.mutex);
                        if (s.stopped) return;
                        if (s.videoTrack) throw TransportError("Duplicate video track");
                        s.videoTrack = true;
                    }
                    depacketizer = std::make_shared<VideoRtpHandler>([weak](const std::byte* data, std::size_t size) {
                        callback(weak, [data, size](Shared& st) { st.videoRtp(data, size); });
                    });
                } else if (desc.type() == "audio") {
                    kind = Kind::Audio; depacketizer = std::make_shared<rtc::OpusRtpDepacketizer>();
                } else return;
                // incomingChain runs next first: RTCP sees original RTP before depacketization.
                depacketizer->addToChain(std::make_shared<rtc::RtcpReceivingSession>());
                track->setMediaHandler(depacketizer);
                if (kind == Kind::Audio) track->onFrame([weak](rtc::binary bytes, rtc::FrameInfo info) {
                    callback(weak, [&bytes, info](Shared& st) { st.packet(Kind::Audio, std::move(bytes), info.timestamp); });
                });
                track->onClosed([weak, kind] { callback(weak, [kind](Shared& st) {
                    Event e(EventType::Failure); e.text = kind == Kind::Video ? "Video track closed" : "Audio track closed";
                    st.event(std::move(e));
                }); });
                track->onError([weak, kind](std::string) { callback(weak, [kind](Shared& st) {
                    Event e(EventType::Failure); e.text = kind == Kind::Video ? "Video track failed" : "Audio track failed";
                    st.event(std::move(e));
                }); });
                Event e(EventType::Track); e.track = track; s.event(std::move(e));
            });
        });
        state = "Waiting for offer";
        operation = "init.send-request";
        signal({{"id", "init_webrtc"}, {"client_hash", clientHash}, {"limit_fps", 30},
                {"control_only", false}, {"iceTransportPolicy", "all"}, {"gamepad", true}, {"isPS", false}});
    }
    void candidate(const Json& candidate) {
        operation = "candidate.validate";
        if (!candidate.is_object() || candidate.at("candidate").get<std::string>().size() > 8192)
            throw TransportError("Invalid ICE candidate");
        // Browser end-of-candidates objects need neither a mid nor an ICE parser.
        if (candidate.at("candidate").get<std::string>().empty()) { ++emptyCandidates; return; }
        if (!peer || !remoteSet) {
            if (pendingCandidates.size() >= 64) throw TransportError("ICE queue limit");
            pendingCandidates.push_back(candidate); return;
        }
        std::string mid;
        if (candidate.contains("sdpMid") && candidate["sdpMid"].is_string()) mid = candidate["sdpMid"].get<std::string>();
        else {
            const auto index = candidate.at("sdpMLineIndex").get<int>();
            if (index < 0 || static_cast<std::size_t>(index) >= mids.size()) throw TransportError("Invalid ICE media index");
            mid = mids[index];
        }
        if (std::find(mids.begin(), mids.end(), mid) == mids.end()) throw TransportError("Invalid ICE mid");
        const auto value = candidate.at("candidate").get<std::string>();
        operation = "candidate.apply";
        peer->addRemoteCandidate(rtc::Candidate(value, mid));
    }
    void message(const Json& message) {
        operation = "signal.envelope";
        const auto id = message.at("id").get<std::string>();
        if (id == "init") createPeer(message);
        else if (id == "offer") {
            operation = "offer.envelope";
            const auto encoded = message.at("data").get<std::string>();
            if (encoded.empty()) return; // Official Android ignores empty offer notifications.
            if (!peer) throw TransportError("Unexpected offer");
            if (remoteSet) throw TransportError("Remote renegotiation unsupported");
            ++offers;
            operation = "offer.base64";
            const auto decoded = protocol::base64Decode(encoded);
            operation = "offer.json";
            const auto offer = Json::parse(decoded);
            if (offer.at("type") != "offer") throw TransportError("Invalid offer type");
            operation = "offer.filter";
            const auto sdp = protocol::filterOffer(offer.at("sdp").get<std::string>(), videoSettings.codec);
            selectedCodec = protocol::offeredVideoCodec(sdp);
            {
                std::lock_guard<std::mutex> lock(shared->mutex);
                shared->video = H264RtpAssembler(selectedCodec);
            }
            operation = "offer.mids";
            mids = protocol::mediaMids(sdp);
            if (mids.empty() || mids.size() > 16 || std::any_of(mids.begin(), mids.end(), [](const std::string& mid) { return mid.empty() || mid.size() > 64; }))
                throw TransportError("Missing SDP mid");
            auto uniqueMids = mids;
            std::sort(uniqueMids.begin(), uniqueMids.end());
            if (std::adjacent_find(uniqueMids.begin(), uniqueMids.end()) != uniqueMids.end())
                throw TransportError("Duplicate SDP mid");
            operation = "offer.parse-sdp";
            rtc::Description remote(sdp, "offer");
            operation = "offer.set-remote";
            peer->setRemoteDescription(remote);
            remoteSet = true;
            for (const auto& c : pendingCandidates) candidate(c);
            pendingCandidates.clear();
            operation = "offer.create-answer";
            peer->setLocalDescription(rtc::Description::Type::Answer);
            state = "Connecting peer";
        } else if (id == "candidate" || id == "ice_candidate") {
            ++candidatesReceived;
            operation = "candidate.envelope";
            const auto encoded = message.at("data").get<std::string>();
            // The official client treats an empty signal payload as a queue/end marker.
            // Our pending candidates already drain after setRemoteDescription succeeds.
            if (encoded.empty()) { ++emptyCandidates; return; }
            operation = "candidate.base64";
            const auto decoded = protocol::base64Decode(encoded);
            operation = "candidate.json";
            candidate(Json::parse(decoded));
        } else if (id == "init_webrtc_error" || id == "init_webrtc" || id == "error" || id == "close") {
            if (id == "close") throw TransportError("Remote signaling closed stream");
            if (id == "error") throw TransportError("Remote signaling error");
            throw TransportError("Remote WebRTC initialization rejected");
        } else if (id == "ping") {
            Json pong{{"id", "pong"}};
            if (message.contains("ts")) pong["ts"] = message["ts"];
            signal(pong);
        }
    }
    void event(Event& event) {
        switch (event.type) {
        case EventType::WsOpen:
            state = "Connecting signaling";
            sendSignaling(protocol::frame("CONNECT", {{"accept-version", "1.1,1.2"},
                       {"heart-beat", "4000,4000"}, {"host", "remoteplay.mts.ru"}}));
            break;
        case EventType::WsData: {
            operation = "stomp.parse";
            lastWs = Clock::now();
            std::string frameTransportFailure;
            for (const auto& frame : parser.feed(event.text)) {
                try {
                if (frame.command == "CONNECTED") {
                    if (stompConnected) throw TransportError("Duplicate STOMP connection");
                    stompConnected = true;
                    const auto hb = frame.headers.find("heart-beat");
                    if (hb != frame.headers.end()) {
                        const auto comma = hb->second.find(',');
                        if (comma == std::string::npos) throw TransportError("Invalid heartbeat negotiation");
                        const auto interval = [](const std::string& value) {
                            if (value.empty() || value.size() > 5 ||
                                !std::all_of(value.begin(), value.end(), [](unsigned char c) { return c >= '0' && c <= '9'; }))
                                throw TransportError("Invalid heartbeat interval");
                            const auto parsed = std::stoi(value);
                            if (parsed > 60000) throw TransportError("Invalid heartbeat interval");
                            return parsed;
                        };
                        const auto outgoing = interval(hb->second.substr(comma + 1));
                        const auto incoming = interval(hb->second.substr(0, comma));
                        heartbeat = outgoing == 0 ? std::chrono::milliseconds(0) : std::chrono::milliseconds(std::max(4000, outgoing));
                        incomingHeartbeat = incoming == 0 ? std::chrono::milliseconds(0) : std::chrono::milliseconds(std::max(4000, incoming));
                    }
                    sendSignaling(protocol::frame("SUBSCRIBE", {{"destination", "/topic/" + clientHash + "/client"},
                               {"ack", "auto"}, {"id", "nx-" + suffix}}));
                    signal({{"id", "enabled"}, {"client_hash", clientHash}});
                    state = "Waiting for remote host";
                } else if (frame.command == "MESSAGE") { operation = "signal.json"; message(Json::parse(frame.body)); }
                else if (frame.command == "ERROR") throw TransportError("STOMP rejected stream");
                } catch (const TransportError& error) {
                    if (!error.retryable) throw;
                    if (frameTransportFailure.empty()) frameTransportFailure = error.what();
                }
            }
            if (!frameTransportFailure.empty()) throw TransportError(frameTransportFailure.c_str(), true);
            break;
        }
        case EventType::Description:
            operation = "answer.send";
            signal({{"id", "answer"}, {"data", protocol::base64Encode(event.text)}});
            break;
        case EventType::Candidate: {
            operation = "candidate.send";
            const auto it = std::find(mids.begin(), mids.end(), event.mid);
            if (it == mids.end()) throw TransportError("Unknown local ICE mid");
            const Json c{{"candidate", event.text}, {"sdpMid", event.mid}, {"sdpMLineIndex", std::distance(mids.begin(), it)}};
            signal({{"id", "ice_candidate"}, {"data", protocol::base64Encode(c.dump())}});
            break;
        }
        case EventType::Channel:
            if (channel) throw TransportError("Duplicate control channel");
            channel = event.channel;
            openChannel();
            break;
        case EventType::ChannelOpen:
            openChannel();
            break;
        case EventType::Control:
            operation = "control.json";
            if (event.text == "close") throw TransportError("Remote control closed");
            if (event.text == "error") throw TransportError("Remote control error");
            throw TransportError("Malformed control message");
        case EventType::Track: tracks.push_back(event.track); break;
        case EventType::PeerState:
            qualityEvent("transportEvent=peer state=" + std::to_string(static_cast<int>(event.peerState)) + " ms=" + std::to_string(event.arrivedMs));
            if (event.peerState == rtc::PeerConnection::State::Failed) throw TransportError("Peer connection failed", true);
            if (event.peerState == rtc::PeerConnection::State::Closed) throw TransportError("Peer connection closed", true);
            if (!isReady) {
                if (event.peerState == rtc::PeerConnection::State::Connecting) state = "Connecting peer";
                else if (event.peerState == rtc::PeerConnection::State::Connected) state = channelOpen ? "Waiting for video" : "Waiting for video/control";
            }
            break;
        case EventType::IceState:
            if (event.iceState == rtc::PeerConnection::IceState::Failed)
                throw TransportError("ICE connection failed", true);
            if (event.iceState == rtc::PeerConnection::IceState::Closed)
                throw TransportError("ICE connection closed", true);
            qualityEvent("transportEvent=ice state=" + std::to_string(static_cast<int>(event.iceState)) + " ms=" + std::to_string(event.arrivedMs));
            break;
        case EventType::GatheringState:
            qualityEvent("transportEvent=gathering state=" + std::to_string(static_cast<int>(event.gatheringState)) + " ms=" + std::to_string(event.arrivedMs));
            break;
        case EventType::WsClosed:
            throw TransportError("Signaling transport closed", true);
        case EventType::Failure:
            throw TransportError(event.text.empty() ? "Transport failed" : event.text.c_str(), true);
        }
    }
};

Client::Client() : impl_(std::make_unique<Impl>()) {}
Client::~Client() { close(); }
bool Client::start(const std::string& hash, const std::string& suffix, const std::string& caPath) {
    return startEndpoint(hash, suffix, caPath, "wss://remoteplay.mts.ru/ws/");
}
#ifdef FOG_STREAM_TESTING
bool Client::startForTesting(const std::string& hash, const std::string& suffix, const std::string& url) {
    if (url.rfind("ws://127.0.0.1:", 0) != 0) return false;
    return startEndpoint(hash, suffix, "unused-loopback.pem", url);
}
void Client::advanceQualityForTesting(unsigned milliseconds) {
    impl_->qualityTimeOffset += milliseconds;
    if (impl_->shared) {
        std::lock_guard<std::mutex> lock(impl_->shared->mutex);
        impl_->shared->qualityTimeOffset = impl_->qualityTimeOffset;
    }
}
void Client::failThenReceiveForTesting(const std::string& text, bool signaling) {
    if (!impl_->shared) return;
    Impl::Event failure(Impl::EventType::Failure); failure.text = "Signaling transport failed";
    impl_->shared->event(std::move(failure));
    if (signaling) {
        Impl::Event data(Impl::EventType::WsData); data.text = text;
        impl_->shared->event(std::move(data));
    } else impl_->shared->controlMessage(text, impl_->channel);
}
void Client::blockControlForTesting(bool blocked) { impl_->controlBlocked = blocked; }
void Client::advanceControlForTesting(unsigned milliseconds) { impl_->controlTimeOffset += milliseconds; }
void Client::elapseForTesting(unsigned seconds) {
    const auto elapsed = std::chrono::seconds(seconds);
    auto& i = *impl_;
    i.qualityTimeOffset += static_cast<std::uint64_t>(seconds) * 1000;
    i.started -= elapsed;
    i.lastWs -= elapsed;
    i.lastHeartbeat -= elapsed;
    i.lastPli -= elapsed;
    if (i.shared) {
        std::lock_guard<std::mutex> lock(i.shared->mutex);
        i.shared->qualityTimeOffset = i.qualityTimeOffset;
        if (i.shared->lastVideo != Clock::time_point{}) i.shared->lastVideo -= elapsed;
    }
}
#endif
bool Client::startEndpoint(const std::string& hash, const std::string& suffix,
                           const std::string& caPath, const std::string& url) {
    const auto preferences = impl_->videoSettings;
    close();
    impl_ = std::make_unique<Impl>();
    impl_->videoSettings = preferences;
    auto& i = *impl_;
    // Do not permit STOMP header/destination injection.
    const auto safe = [](const std::string& s) {
        return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        });
    };
    if (hash.size() > 256 || !safe(hash) || suffix.size() != 5 || !safe(suffix) || caPath.empty()) {
        i.failure = "Invalid stream configuration"; i.state = "Failed"; return false;
    }
    i.hash = hash; i.suffix = suffix; i.clientHash = hash + suffix;
    i.shared = std::make_shared<Impl::Shared>();
    i.started = i.lastWs = i.lastHeartbeat = i.lastPli = i.rateAt = Clock::now();
    i.running = true; i.state = "Opening signaling";
    i.autoQuality.enable(i.videoSettings.quality == 0, i.qualityNow());
    try {
        rtc::WebSocketConfiguration config;
        config.caCertificatePemFile = caPath;
        config.connectionTimeout = std::chrono::milliseconds(25000);
        config.maxMessageSize = 256 * 1024;
        i.ws = std::make_shared<rtc::WebSocket>(config);
        const std::weak_ptr<Impl::Shared> weak(i.shared);
        i.ws->onOpen([weak] { Impl::callback(weak, [](Impl::Shared& s) { s.event(Impl::Event(Impl::EventType::WsOpen)); }); });
        i.ws->onClosed([weak] { Impl::callback(weak, [](Impl::Shared& s) { s.event(Impl::Event(Impl::EventType::WsClosed)); }); });
        i.ws->onError([weak](std::string) { Impl::callback(weak, [](Impl::Shared& s) {
            Impl::Event e(Impl::EventType::Failure); e.text = "Signaling transport failed"; s.event(std::move(e));
        }); });
        i.ws->onMessage([weak](rtc::message_variant data) {
            Impl::callback(weak, [&data](Impl::Shared& s) {
                Impl::Event e(Impl::EventType::WsData);
                if (auto text = std::get_if<std::string>(&data)) e.text = std::move(*text);
                else {
                    const auto& bytes = std::get<rtc::binary>(data);
                    e.text.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                }
                s.event(std::move(e));
            });
        });
        i.ws->open(url);
        return true;
    } catch (const std::exception&) { i.fail("Stream startup failed"); return false; }
}
void Client::pump() {
    auto& i = *impl_;
    if (!i.running || !i.shared) return;
    bool eventOverflow, callbackFailure, needsPli, statsPending;
    quality::Telemetry stats;
    Clock::time_point lastVideo;
    std::deque<Impl::Event> events;
    try {
        std::lock_guard<std::mutex> lock(i.shared->mutex);
        i.shared->video.tick(videoNow()); i.shared->drainVideo();
        eventOverflow = i.shared->eventOverflow; callbackFailure = i.shared->callbackFailure;
        needsPli = i.shared->videoTrack && (i.shared->video.needsPli() || i.decoderWaiting);
        lastVideo = i.shared->lastVideo;
        const auto now = Clock::now();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - i.rateAt).count();
        if (ms >= 1000) {
            i.receiveMbps = static_cast<double>(i.shared->receivedVideoBytes - i.lastRateBytes) * 8.0 / (static_cast<double>(ms) * 1000.0);
            i.lastRateBytes = i.shared->receivedVideoBytes; i.rateAt = now;
            i.rateKnown = true;
        }
        statsPending = i.shared->incoming.takeStats(stats, i.qualityNow());
        for (const auto& event : i.shared->events)
            i.shared->eventDelayHighMs = std::max(i.shared->eventDelayHighMs, videoNow() - event.arrivedMs);
        events.swap(i.shared->events); i.shared->eventBytes = 0;
    } catch (...) {
        i.fail("Video receive processing failed"); return;
    }
    if (eventOverflow) { i.fail("Signaling/control event queue capacity exceeded"); return; }
    if (callbackFailure) { i.fail("Stream callback failed"); return; }
    try {
        // Inspect the whole bounded batch before committing a retryable failure.
        // A later remote close/error or malformed protocol must remain terminal.
        std::string transportFailure;
        for (auto& event : events) {
            try { i.event(event); }
            catch (const TransportError& error) {
                if (!error.retryable) throw;
                if (transportFailure.empty()) transportFailure = error.what();
            }
        }
        if (!transportFailure.empty()) throw TransportError(transportFailure.c_str(), true);
        if (!i.pendingTransportFailure.empty()) throw TransportError(i.pendingTransportFailure.c_str(), true);
        if (statsPending) i.readQualityStats(stats);
        if (i.closingForRetry) return;
        i.applyQuality();
        i.flushControl();
        const auto now = Clock::now();
        if (i.channelOpen && lastVideo != Clock::time_point{}) { i.isReady = true; i.state = "Streaming"; }
        if (!i.isReady && now - i.started >= std::chrono::seconds(30)) { i.fail("Stream connection timed out", true); return; }
        if (i.isReady && now - lastVideo >= std::chrono::seconds(15)) { i.fail("Video stream stalled", true); return; }
        // STOMP's server interval is the first negotiated value. A zero value
        // disables incoming heartbeats; setup and received-video deadlines still apply.
        if (i.stompConnected && i.incomingHeartbeat.count() &&
            now - i.lastWs >= std::max(std::chrono::milliseconds(15000), i.incomingHeartbeat * 3)) {
            i.fail("Signaling stream stalled", true); return;
        }
        if (i.stompConnected && i.heartbeat.count() && now - i.lastHeartbeat >= i.heartbeat) {
            i.sendSignaling(std::string("\n"));
            i.lastHeartbeat = now; ++i.heartbeatsSent;
        }
        if ((i.videoSettings.keyframeSeconds > 0 && now - i.lastPli >= std::chrono::seconds(i.videoSettings.keyframeSeconds)) ||
            (needsPli && now - i.lastPli >= std::chrono::milliseconds(250))) {
            bool requested = false;
            for (const auto& track : i.tracks)
                if (track->description().type() == "video" && track->isOpen()) {
                    track->requestKeyframe(); requested = true;
                }
            if (requested) {
                i.lastPli = now;
                std::lock_guard<std::mutex> lock(i.shared->mutex);
                ++i.shared->pliRequests;
            }
        }
    } catch (const TransportError& e) { i.fail(e.what(), e.retryable); }
      catch (const protocol::Error& e) { i.failAt(e.what()); }
      catch (const Json::exception& e) { i.failAt("JSON error id=" + std::to_string(e.id)); }
      catch (const std::exception& e) {
          // Exact-match whitelist only: library exception text may otherwise contain secrets.
          const char* safe = "RTC/standard exception";
          for (const auto* fixed : {"Remote description has no ICE user fragment", "Remote description has no ICE password",
              "Remote description has no valid fingerprint", "Remote description has no media line", "Remote description has no active media",
              "Invalid media description line", "Invalid description for extmap", "Invalid format description for rtpmap",
              "ICE transport initialization failed", "Failed to create the ICE agent", "Failed to gather local ICE candidates",
              "Invalid ICE settings from remote SDP", "Hostname ICE candidates are not supported on this platform"})
              if (std::string(e.what()) == fixed) { safe = fixed; break; }
          i.failAt(safe);
      }
}
bool Client::poll(Packet& packet) {
    auto& i = *impl_;
    if (!i.shared || !i.running) return false;
    std::lock_guard<std::mutex> lock(i.shared->mutex);
    i.shared->expireMedia(videoNow());
    return i.shared->media.pop(packet);
}
bool Client::configureVideo(const fog::video::Settings& settings) {
    if (impl_->running || !fog::video::valid(settings)) return false;
    impl_->videoSettings = settings;
    return true;
}
bool Client::updateVideoSettings(const fog::video::Settings& settings) {
    if (!impl_->running || !impl_->channelOpen || !fog::video::valid(settings) ||
        settings.codec != impl_->videoSettings.codec) return false;
    const bool remoteChanged = settings.bitrateMbps != impl_->videoSettings.bitrateMbps ||
                               settings.quality != impl_->videoSettings.quality;
    impl_->videoSettings = settings;
    if (remoteChanged) {
        impl_->qualityPending = true; impl_->qualityAttempt = {};
        impl_->qualityEvent("qualityEvent=settings quality=" + std::to_string(settings.quality) +
                            " capMbps=" + std::to_string(settings.bitrateMbps));
        impl_->autoQuality.enable(false, impl_->qualityNow());
        impl_->autoQuality.enable(settings.quality == 0, impl_->qualityNow());
    }
    return true;
}
VideoCodec Client::selectedVideoCodec() const { return impl_->selectedCodec; }
std::string Client::qualityStatus() const {
    const auto& i = *impl_;
    char rates[160];
    std::snprintf(rates, sizeof(rates), "%.1f Мбит/с", i.receiveMbps);
    std::string result = i.remoteSet ? (i.selectedCodec == VideoCodec::H265 ? "H.265" : "H.264") : "Кодек: подключение";
    result += " · Приём: " + (i.rateKnown ? std::string(rates) : std::string("ожидание"));
    const auto now = i.qualityNow();
    result += "\nСервер: " + std::string(quality::name(i.serverStats.profile.value));
    if (i.serverStats.profile.known && !i.serverStats.profile.fresh(now)) result += " (устар.)";
    if (i.serverStats.targetMbps.known) {
        std::snprintf(rates, sizeof(rates), " · %.1f Мбит/с", i.serverStats.targetMbps.value);
        result += rates;
        if (!i.serverStats.targetMbps.fresh(now)) result += " (устар.)";
    }
    if (i.qualityPending) result += " · ожидание";
    else if (i.autoQuality.pending()) result += " · запрос Auto";
    return result;
}
std::size_t Client::queuedVideoPackets() const {
    if (!impl_->shared) return 0;
    std::lock_guard<std::mutex> lock(impl_->shared->mutex);
    return impl_->shared->media.videoCount;
}
void Client::observePlayback(std::uint64_t decodedFrames, std::uint64_t videoWorkUs,
                             std::uint64_t decodeErrors, bool recovering, int width, int height) {
    auto& i = *impl_;
    if (!i.running || !i.shared || !i.channelOpen || i.videoSettings.quality != 0 || i.qualityPending) return;
    quality::Sample sample;
    sample.frames = decodedFrames; sample.workUs = videoWorkUs; sample.errors = decodeErrors;
    sample.recovering = recovering; sample.width = width; sample.height = height;
    {
        std::lock_guard<std::mutex> lock(i.shared->mutex);
        const auto& stats = i.shared->video.stats();
        sample.missing = stats.missing; sample.faults = stats.faultSerial; sample.recoveries = stats.resyncs; sample.pressure = stats.pressure;
        sample.recovering |= i.shared->video.waiting();
        // Auto sees only work left AFTER this frame's bounded drain. A transient
        // predrain burst is not evidence of sustained decoder congestion.
        sample.queuedVideo = i.shared->media.videoCount;
        sample.oldestVideoMs = i.shared->media.age(Kind::Video, videoNow());
    }
    const auto now = i.qualityNow();
    const auto action = i.autoQuality.observe(now, sample, i.serverStats);
    if (action == quality::Action::None) return;
    try {
        if (i.control(Json{{"id", quality::command(action)}}.dump())) {
            ++i.qualityRequests;
            i.autoQuality.sent(action, now, i.serverStats);
        } else i.autoQuality.sendFailed(now);
    } catch (const std::exception&) { i.autoQuality.sendFailed(now); }
}
bool Client::ready() const { return impl_->isReady; }
std::string Client::status() const { return impl_->state; }
std::string Client::error() const { return impl_->failure; }
bool Client::retryableError() const { return !impl_->failure.empty() && impl_->retryableFailure; }
bool Client::disconnected() const {
    if (!impl_->running || !impl_->shared) return false;
    std::lock_guard<std::mutex> lock(impl_->shared->mutex);
    return impl_->shared->peerState == rtc::PeerConnection::State::Disconnected ||
           impl_->shared->iceState == rtc::PeerConnection::IceState::Disconnected;
}
bool Client::disconnectForRetry() {
    auto& i = *impl_;
    if (!i.running || !i.shared) return i.failure.empty() || i.retryableFailure;
    {
        std::lock_guard<std::mutex> lock(i.shared->mutex);
        i.shared->stopped = true; // Atomic boundary: later callbacks belong to the old generation.
    }
    i.closingForRetry = true;
    pump(); // Examine the frozen bounded batch before granting transport-only retry.
    if (!i.running) return i.retryableFailure;
    i.stop(false);
    return true;
}
std::string Client::diagnostics() const { return impl_->diagnostics(); }
std::string Client::videoDiagnostics() const {
    if (!impl_->shared) return impl_->finalVideoDiagnostics;
    std::lock_guard<std::mutex> lock(impl_->shared->mutex);
    return impl_->shared->videoDiagnostics() + impl_->qualityDiagnostics();
}
bool Client::pollVideoDiagnostic(std::string& event) {
    if (!impl_->qualityEvents.empty()) {
        event = std::move(impl_->qualityEvents.front()); impl_->qualityEvents.pop_front(); return true;
    }
    if (impl_->autoQuality.pollEvent(event)) return true;
    if (impl_->shared) {
        std::lock_guard<std::mutex> lock(impl_->shared->mutex);
        return impl_->shared->video.pollEvent(event);
    }
    if (impl_->finalVideoEvents.empty()) return false;
    event = std::move(impl_->finalVideoEvents.front()); impl_->finalVideoEvents.pop_front(); return true;
}
bool Client::waitingForVideoKeyframe() const {
    if (!impl_->shared) return false;
    std::lock_guard<std::mutex> lock(impl_->shared->mutex);
    return impl_->shared->videoTrack && impl_->shared->video.waiting();
}
void Client::setDecoderWaitingForKeyframe(bool waiting) { impl_->decoderWaiting = waiting; }
bool Client::requestVideoKeyframe() {
    if (!impl_->running || Clock::now() - impl_->lastPli < std::chrono::milliseconds(250)) return false;
    bool requested = false;
    try {
        for (const auto& track : impl_->tracks) {
            if (track->description().type() == "video" && track->isOpen()) {
                track->requestKeyframe(); requested = true;
            }
        }
        if (requested) {
            impl_->lastPli = Clock::now();
            if (impl_->shared) {
                std::lock_guard<std::mutex> lock(impl_->shared->mutex);
                ++impl_->shared->pliRequests;
            }
        }
    } catch (...) { return false; }
    return requested;
}
bool Client::sendGamepad(const Gamepad& state) {
    if (!impl_->running || !impl_->channelOpen) return false;
    try { return impl_->control(protocol::gamepad(state).dump(), ControlQueue::Mode::Gamepad); }
    catch (const TransportError& error) {
        // Public send may run before pump. Examine queued terminal signaling first.
        if (error.retryable) {
            if (impl_->pendingTransportFailure.empty()) impl_->pendingTransportFailure = error.what();
        } else impl_->fail(error.what());
        return false;
    } catch (const std::exception&) { impl_->fail("Control send failed"); return false; }
}
bool Client::sendControl(const std::string& json) {
    if (!impl_->running || !impl_->channelOpen || json.size() > 64 * 1024) return false;
    const auto data = Json::parse(json, nullptr, false);
    if (!data.is_object()) return false;
    auto mode = ControlQueue::Mode::Ordered;
    std::string input;
    const auto id = data.find("id");
    if (id != data.end() && id->is_string()) {
        const auto& name = id->get_ref<const std::string&>();
        if (name == "gamepad") mode = ControlQueue::Mode::Gamepad;
        else if (name == "mousemove") mode = ControlQueue::Mode::MouseMove;
        else if (name == "keyup" || name == "mouseup") mode = ControlQueue::Mode::Release;
        // Tag down/up for the same input so repeated releases can be deduplicated.
        const auto details = data.find("data");
        if (details != data.end() && details->is_object()) {
            if (name == "keyup" || name == "keydown") {
                const auto code = details->find("code");
                if (code != details->end() && code->is_string()) input = "key:" + code->get<std::string>();
            } else if (name == "mouseup" || name == "mousedown") {
                const auto button = details->find("button");
                if (button != details->end() && button->is_number_integer()) input = "mouse:" + button->dump();
            }
        }
    }
    try { return impl_->control(json, mode, input); }
    catch (const TransportError& error) {
        // Public send may run before pump. Examine queued terminal signaling first.
        if (error.retryable) {
            if (impl_->pendingTransportFailure.empty()) impl_->pendingTransportFailure = error.what();
        } else impl_->fail(error.what());
        return false;
    } catch (const std::exception&) { impl_->fail("Control send failed"); return false; }
}
void Client::close() { if (impl_ && impl_->running) impl_->stop(true); }
void Client::cleanupRuntime() {
    auto cleanup = rtc::Cleanup();
    cleanup.wait();
    rtc::JoinCleanupThreads();
    cleanup.get();
}
}
