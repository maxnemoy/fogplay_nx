#include "features/streaming/infrastructure/input_protocol.hpp"
#include "core/constants/error_text.hpp"
#include "features/streaming/infrastructure/stream.hpp"
#include "features/streaming/infrastructure/dtls_diagnostics.hpp"
#include "features/streaming/infrastructure/stream_protocol.hpp"
#include "features/streaming/domain/h264_rtp.hpp"
#include "features/streaming/infrastructure/stream_queue.hpp"
#include "features/streaming/infrastructure/async_control_sender.hpp"
#include "features/streaming/infrastructure/async_ice_snapshot.hpp"
#include "features/streaming/infrastructure/rtcp_feedback_worker.hpp"
#include "features/streaming/infrastructure/incoming_control.hpp"
#include "features/streaming/infrastructure/transport_history.hpp"
#include "features/streaming/infrastructure/timed_mutex.hpp"
#include "features/streaming/infrastructure/stream_route.hpp"
#include "features/streaming/domain/auto_quality.hpp"
#include "features/streaming/domain/route_snapshot_schedule.hpp"
#include <functional>
#include <limits>
#include <cmath>
#include <cstdio>
#include <rtc/rtc.hpp>
#include <algorithm>
#include <chrono>
#include <deque>
#include <stdexcept>
#include <utility>

namespace stream {
using Clock = std::chrono::steady_clock;
using Json = protocol::Json;
// Fixed-size, application-thread counters. Scopes can nest (events/quality may
// flush control); measurements include waiting/preemption, not only CPU work.
struct TransportStageTiming {
    std::uint64_t calls = 0, totalUs = 0, maxUs = 0;
    std::string text(const char* tag) const {
        char out[180];
        std::snprintf(out, sizeof(out), " %sCalls=%llu %sAvgUs=%llu %sMaxUs=%llu", tag,
            static_cast<unsigned long long>(calls), tag,
            static_cast<unsigned long long>(calls ? totalUs / calls : 0), tag,
            static_cast<unsigned long long>(maxUs));
        return out;
    }
};
struct TransportStageScope {
    TransportStageTiming& timing;
    Clock::time_point started = Clock::now();
    ~TransportStageScope() {
        const auto us = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started).count());
        ++timing.calls; timing.totalUs += us; timing.maxUs = std::max(timing.maxUs, us);
    }
};
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
    explicit VideoRtpHandler(std::function<void(const std::byte*, std::size_t, const rtc::message_callback&)> receive)
        : receive_(std::move(receive)) {}
    void incoming(rtc::message_vector& messages, const rtc::message_callback& send) override {
        for (const auto& message : messages)
            if (message->type == rtc::Message::Binary) receive_(message->data(), message->size(), send);
        messages.clear();
    }
private:
    std::function<void(const std::byte*, std::size_t, const rtc::message_callback&)> receive_;
};
// incomingChain is reverse ordered: this tail runs before the RTCP receiver.
class RtxRtpHandler final : public rtc::MediaHandler {
public:
    explicit RtxRtpHandler(std::function<bool(rtc::binary&, bool)> normalize) : normalize_(std::move(normalize)) {}
    void incoming(rtc::message_vector& messages, const rtc::message_callback&) override {
        rtc::message_vector accepted;
        for (auto& message : messages) {
            if (message->type == rtc::Message::Control) {
                if (normalize_(*message, true)) accepted.push_back(std::move(message));
                continue;
            }
            if (message->type != rtc::Message::Binary) continue;
            if (normalize_(*message, false)) {
                message->stream = (std::to_integer<unsigned>(message->at(8)) << 24) |
                    (std::to_integer<unsigned>(message->at(9)) << 16) |
                    (std::to_integer<unsigned>(message->at(10)) << 8) | std::to_integer<unsigned>(message->at(11));
                accepted.push_back(std::move(message));
            }
        }
        messages.swap(accepted);
    }
private:
    std::function<bool(rtc::binary&, bool)> normalize_;
};
static std::uint64_t videoNow() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}
struct Client::Impl {
    struct IceData {
        std::string diagnostics;
        rtc::DtlsDiagnostics dtls;
        rtc::Candidate local, remote;
        bool pair = false, pairFailed = false;
        TransportStageTiming diagnosticTiming, pairTiming;
    };
    using IceWorker = AsyncIceSnapshot<IceData>;

    enum class EventType {
#ifdef FOG_STREAM_TESTING
                           TestCandidateProof,
#endif
                           WsOpen, WsData, WsClosed, Failure, Description, Candidate,
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
        TimedMutex mutex;
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
        std::shared_ptr<AsyncControlSender> controlSender;
        std::shared_ptr<IceWorker> iceWorker;
        std::shared_ptr<RtcpFeedbackWorker> rtcpWorker;
        bool relay = false, turnAuthorized = false;
        std::uint64_t routeRevision = 0;
        Clock::time_point routeCapturedAt{};
        std::string pendingPong;
        std::uint64_t qualityTimeOffset = 0, eventDelayHighMs = 0;
        Clock::time_point lastVideo{};
        H264RtpAssembler video;
        RtpRepair repair;
        rtc::message_callback videoFeedback;
        std::uint64_t nackSendErrors = 0;
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
            if (media.age(Kind::Video, now) > MediaQueue::MaxVideoAgeMs) {
                media.recordVideoPressure(MediaQueue::PressureReason::Age, now); resyncQueue(false, now);
            }
        }
        std::size_t videoFrameBudget() const {
            return media.videoCount >= MediaQueue::MaxVideo ? 0 :
                std::min(std::size_t(2), MediaQueue::MaxVideo - media.videoCount);
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
        bool normalizeVideo(rtc::binary& packet, bool control) {
            TimedMutex::Guard lock(mutex, TimedMutex::Scope::Callback);
            if (stopped) return false;
            if (control) return repair.acceptControl(packet);
            const auto now = videoNow();
            return repair.normalize(packet, [this, now](std::uint16_t seq) {
                return video.missingSequence(seq) || video.requestedSequence(seq, now);
            });
        }
        void sendNacks() {
            rtc::message_callback send; std::vector<std::byte> packet;
            {
                TimedMutex::Guard lock(mutex, TimedMutex::Scope::Client);
                if (stopped || !videoFeedback) return;
                packet = repair.nack(video.takeNacks(videoNow())); send = videoFeedback;
            }
            // Weak transport callback, outside the shared mutex. A failed feedback
            // send does not terminate a rental; the bounded IDR fallback remains.
            transmitNack(std::move(packet), send, TimedMutex::Scope::Client);
        }
        void transmitNack(std::vector<std::byte> packet, const rtc::message_callback& send, TimedMutex::Scope scope) {
            if (!packet.empty()) try { send(rtc::make_message(std::move(packet), rtc::Message::Control)); }
            catch (...) { TimedMutex::Guard lock(mutex, scope); ++nackSendErrors; }
        }
        void videoRtp(const std::byte* bytes, std::size_t size, const rtc::message_callback& send) {
            std::vector<std::byte> feedback;
            {
                TimedMutex::Guard lock(mutex, TimedMutex::Scope::Callback);
                if (stopped) return;
                videoFeedback = send;
                lastRtpMs = videoNow(); receivedVideoBytes += size;
                expireMedia(lastRtpMs);
                video.receive(bytes, size, lastRtpMs, videoFrameBudget()); drainVideo();
                feedback = repair.nack(video.takeNacks(lastRtpMs));
            }
            transmitNack(std::move(feedback), send, TimedMutex::Scope::Callback);
        }
        std::string videoDiagnostics() const {
            const auto now = videoNow();
            return video.diagnostics() + repair.diagnostics() + " nackSendErrors=" + std::to_string(nackSendErrors) + " queue=" + std::to_string(media.packets.size()) +
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
            TimedMutex::Guard lock(mutex, TimedMutex::Scope::Callback);
            if (stopped) return;
            const auto now = videoNow();
            if (e.type == EventType::PeerState || e.type == EventType::IceState ||
                e.type == EventType::GatheringState || e.type == EventType::Candidate ||
                e.type == EventType::Description || e.type == EventType::Channel || e.type == EventType::ChannelOpen) {
                if (iceWorker) iceWorker->invalidate();
                if (relay && controlSender) controlSender->allow(false);
            }
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
        void controlMessage(const std::string& text) {
            const auto message = IncomingControl::classify(text);
            std::shared_ptr<AsyncControlSender> sender;
            {
                TimedMutex::Guard lock(mutex, TimedMutex::Scope::Callback);
                if (stopped) return;
                lastControlMs = videoNow();
                incoming.receive(message, lastControlMs + qualityTimeOffset);
                if (message.kind == IncomingControl::Kind::Close || message.kind == IncomingControl::Kind::Error ||
                    message.kind == IncomingControl::Kind::Invalid) terminalControl = message.kind;
                sender = controlSender;
                if (!sender && message.kind == IncomingControl::Kind::Ping) pendingPong = message.pong;
            }
            if (message.kind == IncomingControl::Kind::Ping) {
                // Pong dispatch is independent of the application's frame pump,
                // but shares the same serialized, route-gated sender as input.
                if (!sender) return; // Channel publication will transfer the bounded latest pong.
                if (!sender->enqueue(message.pong, ControlQueue::Mode::Pong)) {
                    {
                        TimedMutex::Guard lock(mutex, TimedMutex::Scope::Callback);
                        if (stopped) return;
                        incoming.pongFailed();
                    }
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
            TimedMutex::Guard lock(mutex, TimedMutex::Scope::Callback);
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
            TimedMutex::Guard lock(shared->mutex, TimedMutex::Scope::Callback);
            if (shared->stopped) return;
        }
        try { fn(*shared); }
        catch (const std::exception&) {
            TimedMutex::Guard lock(shared->mutex, TimedMutex::Scope::Callback);
            shared->callbackFailure = true;
        } catch (...) {
            // Unknown callback failures must also reach the owner, without leaking SDP/credentials.
            TimedMutex::Guard lock(shared->mutex, TimedMutex::Scope::Callback);
            shared->callbackFailure = true;
        }
    }
    std::shared_ptr<Shared> shared;
    std::shared_ptr<rtc::WebSocket> ws;
    std::shared_ptr<rtc::PeerConnection> peer;
    std::shared_ptr<rtc::DataChannel> channel;
    std::vector<std::shared_ptr<rtc::Track>> tracks;
    std::shared_ptr<AsyncControlSender> controlSender;
    std::shared_ptr<IceWorker> iceWorker;
    std::shared_ptr<RtcpFeedbackWorker> rtcpWorker;
    RtcpFeedbackWorker::Snapshot finalRtcp;
    std::uint64_t routeRevision = 0, publishedSerial = 0;
    Clock::time_point routeCapturedAt{};
    TransportStageTiming icePublishTiming;
    AsyncControlSender::Snapshot finalControl;
    std::uint64_t psUnavailableFrames = 0;
    std::uint64_t psInvalidNative = 0;
#ifdef FOG_STREAM_TESTING
    struct ControlSendFixture {
        std::mutex mutex;
        std::condition_variable condition;
        bool blocked = false;
        void wait() {
            std::unique_lock<std::mutex> lock(mutex);
            condition.wait(lock, [this] { return !blocked; });
        }
    };
    std::shared_ptr<ControlSendFixture> controlSendFixture = std::make_shared<ControlSendFixture>();
    struct IceReadFixture {
        std::mutex mutex;
        std::condition_variable condition;
        bool blocked = false;
        unsigned stage = 0;
        void wait(unsigned currentStage) {
            std::unique_lock<std::mutex> lock(mutex);
            condition.wait(lock, [this, currentStage] { return !blocked || stage != currentStage; });
        }
    };
    std::shared_ptr<IceReadFixture> iceReadFixture = std::make_shared<IceReadFixture>();
#endif
    std::vector<Json> pendingCandidates;
    std::vector<std::string> mids;
    protocol::Parser parser;
    std::string hash, clientHash, suffix, state = "Idle", failure, pendingTransportFailure;
    Clock::time_point started{}, lastWs{}, lastHeartbeat{}, lastPli{};
    std::chrono::milliseconds heartbeat{0}, incomingHeartbeat{0};
    bool running = false, stompConnected = false, remoteSet = false, channelOpen = false;
    bool isReady = false, decoderWaiting = false, retryableFailure = false, closingForRetry = false;
    bool relay = false, routeSelected = false, routeCurrent = false, wasReady = false;
    bool playStation = false;
    Clock::time_point unprovenRouteAt{};
    SelectedRoute route;
    CandidateProofs remoteCandidateProofs;
    RouteSnapshotSchedule routeSnapshotSchedule;
    TransportStageTiming pumpTiming, nackTiming;
    TransportStageTiming receiveTiming, eventsTiming, qualityTiming, controlTiming, pliTiming, heartbeatTiming;
    std::string transportTimings() const {
        return "scope=transport-lifetime nested=1 iceThread=worker" + pumpTiming.text("pump") + nackTiming.text("nack") +
            icePublishTiming.text("icePublish") + iceWorkerTimings() + receiveTiming.text("receive") +
            eventsTiming.text("events") + qualityTiming.text("quality") + controlTiming.text("controlFlush") +
            pliTiming.text("pli") + heartbeatTiming.text("heartbeat");
    }
    void invalidateRoute() {
        // Retain the last known enums for final diagnostics, but never authorize
        // required-TURN media/control from a pre-transition cached selection.
        routeCurrent = false;
        if (iceWorker) iceWorker->invalidate();
        if (controlSender && relay) controlSender->allow(false);
        if (relay) isReady = false;
        routeSnapshotSchedule.invalidate();
    }
    void addCandidateProof(rtc::Candidate candidate) {
        const auto before = remoteCandidateProofs.size();
        remoteCandidateProofs.add(std::move(candidate));
        if (remoteCandidateProofs.size() != before) invalidateRoute();
    }
#ifdef FOG_STREAM_TESTING
    bool routeOverride = false, testPairAvailable = true, testPairThrows = false, fixedRouteClock = false;
    std::uint64_t routeClock = 0;
    rtc::Candidate testLocal, testRemote;
#endif
    bool currentRoute() const {
        return routeCurrent && iceWorker && routeRevision == iceWorker->state().revision &&
            routeCapturedAt != Clock::time_point{} && Clock::now() - routeCapturedAt < std::chrono::milliseconds(RouteSnapshotSchedule::IntervalMs * 2);
    }
    bool routeAllowed() const { return !relay || (currentRoute() && route.isTurn()); }
    rtc::Candidate selectedLocal, selectedRemote;
    unsigned routeSelections = 0, routeEventDrops = 0;
    std::deque<std::string> routeEvents;
    std::string finalIceDiagnostics;
    rtc::DtlsDiagnostics finalDtlsDiagnostics;
    bool iceSnapshotAvailable = false;
    std::string routeDiagnostics() const {
        return std::string(" requested=") + (relay ? "relay" : "all") + route.diagnostics() +
            " iceSnapshotAvailable=" + std::to_string(iceSnapshotAvailable) + " localPolicy=all routeKnown=" + std::to_string(routeSelected) + " routeCurrent=" + std::to_string(currentRoute()) + " routeSnapshot=" + (running ? "live" : "final") +
            iceWorkerDiagnostics() + " candidateProofs=" + std::to_string(remoteCandidateProofs.size()) + " candidateProofDrops=" + std::to_string(remoteCandidateProofs.dropped()) +
            " routeSelections=" + std::to_string(routeSelections) + " routeEventDrops=" + std::to_string(routeEventDrops);
    }
    std::string iceWorkerTimings() const {
        if (!iceWorker) return {};
        const auto stats = iceWorker->state();
        const auto text = [](const IceWorker::Timing& t, const char* tag) {
            return TransportStageTiming{t.calls, t.totalUs, t.maxUs}.text(tag);
        };
        return text(stats.diagnostic, "iceDiag") + text(stats.pair, "icePair") + text(stats.schedule, "iceSchedule") + text(stats.wait, "iceWorkerIdle") + text(stats.work, "iceWork");
    }
    std::string iceWorkerDiagnostics() const {
        if (!iceWorker) return " iceGeneration=0 iceInFlight=0 iceSnapshotAgeMs=-1";
        const auto stats = iceWorker->state();
        const auto age = routeCapturedAt == Clock::time_point{} ? -1LL :
            static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - routeCapturedAt).count());
        return " iceGeneration=" + std::to_string(stats.revision) + " icePublishedGeneration=" + std::to_string(routeRevision) +
            " iceInFlight=" + std::to_string(stats.active) + " iceInFlightMs=" + std::to_string(stats.activeMs) +
            " icePending=" + std::to_string(stats.pending) + " iceRequested=" + std::to_string(stats.requested) +
            " icePublishedSerial=" + std::to_string(publishedSerial) + " iceCompleted=" + std::to_string(stats.completed) + " iceDiscarded=" + std::to_string(stats.discarded) +
            " iceSnapshotAgeMs=" + std::to_string(age) + " iceStopping=" + std::to_string(stats.stopping);
    }
    std::uint64_t routeNow() const {
#ifdef FOG_STREAM_TESTING
        if (fixedRouteClock) return routeClock;
#endif
        return videoNow();
    }
    void publishIce() {
        TransportStageScope timing{icePublishTiming};
        if (!iceWorker) return;
        auto completed = iceWorker->take();
        if (!completed || completed->revision != iceWorker->state().revision) return;
        publishedSerial = completed->serial;
        auto& value = completed->value;
        iceSnapshotAvailable = !value.diagnostics.empty();
        if (iceSnapshotAvailable) finalIceDiagnostics = std::move(value.diagnostics);
        finalDtlsDiagnostics = value.dtls;
        routeCapturedAt = completed->capturedAt; routeRevision = completed->revision;
        routeCurrent = !value.pairFailed && value.pair;
        {
            TimedMutex::Guard lock(shared->mutex, TimedMutex::Scope::Client);
            // Every completion replaces authorization, including no pair and
            // exceptions from an ordinary periodic read of the same epoch.
            shared->turnAuthorized = false;
            shared->routeRevision = completed->revision; shared->routeCapturedAt = completed->capturedAt;
            if (relay && controlSender) controlSender->allow(false);
        }
        if (value.pairFailed) { routeSnapshotSchedule.failed(routeNow()); if (relay) isReady = false; return; }
        // Re-read provenance only after the generation check. A delayed result
        // can never bring a previous endpoint's authorization into a new epoch.
        routeSnapshotSchedule.queried(routeNow());
        if (!value.pair) { if (relay) isReady = false; return; }
        const auto& local = value.local; const auto& remote = value.remote;
        const SelectedRoute selected{local.type(), remote.type(), local.transportType(), remote.transportType(), remoteCandidateProofs.find(remote)};
        {
            TimedMutex::Guard lock(shared->mutex, TimedMutex::Scope::Client);
            shared->turnAuthorized = selected.isTurn();
            shared->routeRevision = completed->revision; shared->routeCapturedAt = completed->capturedAt;
        }
        if (routeSelected && selected == route && local == selectedLocal && remote == selectedRemote) return;
        selectedLocal = local; selectedRemote = remote;
        const bool first = !routeSelected;
        route = selected; routeSelected = true; ++routeSelections;
        if (routeEvents.size() >= 16) { routeEvents.pop_front(); ++routeEventDrops; }
        routeEvents.push_back(std::string("routeEvent=") + (first ? "selected" : "changed") + routeDiagnostics());
    }
    void snapshotIce() {
        if (!peer || !iceWorker) return;
        publishIce();
        const auto ms = routeNow();
        if (!routeSnapshotSchedule.due(ms)) return;
        const auto transport = peer;
#ifdef FOG_STREAM_TESTING
        const auto fixture = iceReadFixture;
        const auto override = routeOverride, available = testPairAvailable, throws = testPairThrows;
        const auto testL = testLocal, testR = testRemote;
#endif
        const auto revision = iceWorker->state().revision;
        if (iceWorker->request(revision, [transport
#ifdef FOG_STREAM_TESTING
            , fixture, override, available, throws, testL, testR
#endif
        ] {
            IceData value;
#ifdef FOG_STREAM_TESTING
            fixture->wait(0);
#endif
            {
                TransportStageScope timing{value.diagnosticTiming};
                try { value.diagnostics = transport->iceDiagnostics(); } catch (...) {}
            }
            try { value.dtls = transport->getDtlsDiagnostics(); } catch (...) {}
#ifdef FOG_STREAM_TESTING
            fixture->wait(1);
#endif
            {
                TransportStageScope timing{value.pairTiming};
                try {
#ifdef FOG_STREAM_TESTING
                    if (throws) throw std::runtime_error(fog::constants::error::Stream::SelectedPairFixtureFailure);
                    if (override) { value.pair = available; value.local = testL; value.remote = testR; }
                    else
#endif
                    value.pair = transport->getSelectedCandidatePair(&value.local, &value.remote);
                } catch (...) { value.pairFailed = true; }
            }
            return value;
        })) routeSnapshotSchedule.queried(ms);
    }
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
        if (!qualityPending || !channelOpen || !routeAllowed()) return;
        const auto now = Clock::now();
        if (qualityAttempt != Clock::time_point{} && now - qualityAttempt < std::chrono::seconds(1)) return;
        // RTCP needs the real remote SSRC learned from video RTP first.
        { TimedMutex::Guard lock(shared->mutex, TimedMutex::Scope::Client);
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
    std::string finalVideoDiagnostics, finalTransportDiagnostics, finalQueueDiagnostics, finalGapDiagnostics;
    std::string finalMutexDiagnostics;
    std::deque<std::string> finalVideoEvents;
    const char* operation = "idle";
    unsigned offers = 0, candidatesReceived = 0, emptyCandidates = 0, heartbeatsSent = 0;
    // Application-thread counters. A non-throwing WebSocket send is local
    // acceptance; false means buffered, never a broker receipt or rejection.
    struct SignalingSendCounts {
        std::uint64_t attempts = 0, accepted = 0, buffered = 0, rejected = 0;
        std::string text(const char* name) const {
            return std::string(" ") + name + "Attempts=" + std::to_string(attempts) +
                " " + name + "LocalAccepted=" + std::to_string(accepted) +
                " " + name + "Buffered=" + std::to_string(buffered) +
                " " + name + "Rejected=" + std::to_string(rejected);
        }
    } subscribeSends, enabledSends;
    std::uint64_t wsMessages = 0, stompFrames = 0, stompConnectedFrames = 0;
    std::uint64_t stompMessageFrames = 0, stompErrorFrames = 0, stompOtherFrames = 0, signalInitMessages = 0;
#ifdef FOG_STREAM_TESTING
    SignalingSendCounts* testSignalingSend = nullptr;
    bool testSignalingReject = false;
#endif
    std::string diagnostics() const {
        return routeDiagnostics() + " " + finalIceDiagnostics + " operation=" + operation + " offers=" + std::to_string(offers) +
            " candidates=" + std::to_string(candidatesReceived) + " emptyCandidates=" + std::to_string(emptyCandidates) +
            " hbOutMs=" + std::to_string(heartbeat.count()) + " hbInMs=" + std::to_string(incomingHeartbeat.count()) +
            " hbSent=" + std::to_string(heartbeatsSent) +
            " wsAgeMs=" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - lastWs).count()) +
            " wsMessages=" + std::to_string(wsMessages) + " stompFrames=" + std::to_string(stompFrames) +
            " stompConnected=" + std::to_string(stompConnectedFrames) + " stompMessage=" + std::to_string(stompMessageFrames) +
            " stompError=" + std::to_string(stompErrorFrames) + " stompOther=" + std::to_string(stompOtherFrames) +
            " signalInit=" + std::to_string(signalInitMessages) + subscribeSends.text("subscribe") + enabledSends.text("enabled") +
            controlDiagnostics() +
            transportDiagnostics();
    }
    std::string transportDiagnostics() const {
        if (!shared) return finalTransportDiagnostics;
        TimedMutex::Guard lock(shared->mutex, TimedMutex::Scope::Diagnostics);
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
    void sendSignaling(const std::string& frame, SignalingSendCounts* counts = nullptr) {
        if (closingForRetry) return;
        if (counts) ++counts->attempts;
        try {
            transportSend([&] {
#ifdef FOG_STREAM_TESTING
                const bool overridden = counts && testSignalingSend == counts;
                if (overridden) {
                    testSignalingSend = nullptr;
                    if (testSignalingReject) throw std::runtime_error("Fixture signaling send rejected");
                }
#endif
                bool immediate = ws->send(frame);
#ifdef FOG_STREAM_TESTING
                if (overridden) immediate = false;
#endif
                if (counts) {
                    ++counts->accepted;
                    if (!immediate) ++counts->buffered;
                }
            }, "Signaling send failed");
        } catch (...) {
            if (counts) ++counts->rejected;
            throw;
        }
    }
    void signal(const Json& payload, SignalingSendCounts* counts = nullptr) {
        if (!ws || !ws->isOpen() || !stompConnected) throw TransportError(fog::constants::error::Stream::SignalingUnavailable, true);
        if (ws->bufferedAmount() > 256 * 1024) throw TransportError(fog::constants::error::Stream::SignalingBackpressure, true);
        const auto frame = protocol::frame("SEND", {{"destination", "/topic/" + hash + "/server"},
                  {"content-type", "application/json"}, {"client-hash", clientHash}}, payload.dump());
        sendSignaling(frame, counts);
    }
    std::string controlDiagnostics() const {
        const auto out = controlSender ? controlSender->snapshot() : finalControl;
        return " controlPending=" + std::to_string(out.pending) + " controlBytes=" + std::to_string(out.bytes) +
            " controlRejected=" + std::to_string(out.rejected) + " controlSent=" + std::to_string(out.sent) + " controlPongCoalesced=" + std::to_string(out.pongCoalesced) +
            " controlAgeMs=" + std::to_string(out.ageMs) + " controlAgeMaxMs=" + std::to_string(out.ageMaxMs) +
            " controlTransportActive=" + std::to_string(out.transportActive) + " controlInFlight=" + std::to_string(out.inFlight) + " controlInFlightMs=" + std::to_string(out.inFlightMs) +
            " controlSendCalls=" + std::to_string(out.sendCalls) + " controlSendAvgUs=" +
            std::to_string(out.sendCalls ? out.sendTotalUs / out.sendCalls : 0) + " controlSendMaxUs=" + std::to_string(out.sendMaxUs) +
            " controlWorkerIdleCalls=" + std::to_string(out.waitCalls) + " controlWorkerIdleAvgUs=" +
            std::to_string(out.waitCalls ? out.waitTotalUs / out.waitCalls : 0) + " controlWorkerIdleMaxUs=" + std::to_string(out.waitMaxUs);
    }
    void flushControl() {
        // Historical controlFlush stage now measures publication and health only;
        // transport work is measured by controlSend* on the dedicated worker.
        TransportStageScope timing{controlTiming};
        if (!controlSender) return;
        {
            // Serialize authorization with callback invalidation. An event that
            // revokes a route cannot be overwritten by an older allow(true).
            TimedMutex::Guard lock(shared->mutex, TimedMutex::Scope::Client);
            controlSender->allow(routeAllowed());
        }
        const auto out = controlSender->snapshot();
        if (out.failure == AsyncControlSender::Failure::Transport) throw TransportError(fog::constants::error::Stream::ControlSendFailed, true);
        if (out.failure == AsyncControlSender::Failure::Unexpected) throw TransportError(fog::constants::error::Stream::ControlSenderFailed);
        // A stalled synchronous sender must reach rental termination before any
        // cleanup join; retry cleanup could otherwise wait on the stalled call.
        if (out.stalled) throw TransportError(fog::constants::error::Stream::ControlChannelStalled);
    }
    bool control(const std::string& payload, ControlQueue::Mode mode = ControlQueue::Mode::Ordered,
                 const std::string& input = {}, const std::string& release = {}) {
        if (!controlSender || !routeAllowed() || controlSender->snapshot().stalled) return false;
        return controlSender->enqueue(payload, mode, input, release);
    }
    void openChannel() {
        if (closingForRetry || channelOpen || !channel || !channel->isOpen()) return;
        if (!routeAllowed()) {
            if (currentRoute() && route.isDirect()) throw TransportError(fog::constants::error::Stream::TURNRequiredButDirectPathSelected);
            state = "Waiting for TURN route";
            return;
        }
        control(Json{{"id", "connection_ready"}}.dump());
        channelOpen = true;
        if (!isReady) state = "Waiting for video";
    }
    std::string neutralControl() const noexcept {
        try { if (channelOpen) return protocol::gamepad(Gamepad{}, playStation).dump(); } catch (...) {}
        return {};
    }
    void stop(bool notify) {
        // Publish stop intent before querying any RTC locks. The worker drops
        // pending old-generation states immediately, then releases held input.
        if (controlSender) controlSender->requestStop(neutralControl());
        if (rtcpWorker) rtcpWorker->requestStop();
        // Cached evidence only: final diagnostics must not enter RTC locks.
        if (iceWorker) { iceWorker->requestStop(); iceWorker->stop(); }
        // Join outside callback/RTC locks, before closing channel/peer handles.
        if (controlSender) {
            controlSender->stop();
            finalControl = controlSender->snapshot();
        }
        if (rtcpWorker) {
            rtcpWorker->join();
            finalRtcp = rtcpWorker->snapshot();
        }
        // No shutdown exception can prevent release of the remaining transports.
        if (notify) {
            closingForRetry = false; // A rejected retry follows ordinary final-close notification.
            try { if (stompConnected && ws && ws->isOpen()) signal({{"id", "close"}}); }
            catch (const std::exception&) { if (failure.empty()) failure = fog::constants::error::Stream::CloseNotificationFailed; }
        }
        running = false; isReady = false; channelOpen = false; stompConnected = false;
        // Queued events can own tracks/channels. Release them outside the queue lock:
        // their destruction may release RTC handles while a callback is draining.
        std::deque<Event> abandonedEvents;
        std::deque<Packet> abandonedPackets;
        if (shared) {
            TimedMutex::Guard lock(shared->mutex, TimedMutex::Scope::Client);
            shared->stopped = true;
            // Diagnostics must never interrupt transport teardown, including
            // allocation failure while snapshotting counters or event strings.
            try {
                finalVideoDiagnostics = shared->videoDiagnostics() + qualityDiagnostics();
                finalGapDiagnostics = shared->video.gapDiagnostics();
                finalQueueDiagnostics = shared->media.pressureText();
                finalTransportDiagnostics = shared->transportDiagnostics();
                finalMutexDiagnostics = TimedMutex::diagnostics(lock.snapshot());
                std::string videoEvent;
                while (shared->video.pollEvent(videoEvent)) finalVideoEvents.push_back(std::move(videoEvent));
            } catch (...) {}
            abandonedEvents.swap(shared->events);
            abandonedPackets.swap(shared->media.packets);
            shared->eventBytes = 0;
        }
        if (channel) {
            try { channel->resetCallbacks(); channel->close(); }
            catch (const std::exception&) { if (failure.empty()) failure = fog::constants::error::Stream::ControlShutdownFailed; }
            channel.reset();
        }
        for (auto& track : tracks) {
            try { track->resetCallbacks(); track->close(); }
            catch (const std::exception&) { if (failure.empty()) failure = fog::constants::error::Stream::TrackShutdownFailed; }
        }
        tracks.clear();
        if (peer) {
            try { peer->resetCallbacks(); peer->close(); }
            catch (const std::exception&) { if (failure.empty()) failure = fog::constants::error::Stream::PeerShutdownFailed; }
            peer.reset();
        }
        if (ws) {
            try { ws->resetCallbacks(); ws->close(); }
            catch (const std::exception&) { if (failure.empty()) failure = fog::constants::error::Stream::SignalingShutdownFailed; }
            ws.reset();
        }
        shared.reset();
        controlSender.reset(); rtcpWorker.reset(); pendingTransportFailure.clear(); decoderWaiting = false;
        pendingCandidates.clear(); mids.clear();
        state = failure.empty() ? "Closed" : "Failed";
        hash.clear(); clientHash.clear(); suffix.clear();
    }
    void fail(const char* reason, bool mayRetry = false) {
        // Terminal evidence wins over concurrent transport loss, even when the FIFO is full.
        if (mayRetry && shared) {
            TimedMutex::Guard lock(shared->mutex, TimedMutex::Scope::Client);
            if (shared->terminalControl == IncomingControl::Kind::Close) { reason = fog::constants::error::Stream::RemoteControlClosed; mayRetry = false; }
            else if (shared->terminalControl == IncomingControl::Kind::Error) { reason = fog::constants::error::Stream::RemoteControlError; mayRetry = false; }
            else if (shared->terminalControl == IncomingControl::Kind::Invalid) { reason = fog::constants::error::Stream::MalformedControlMessage; mayRetry = false; }
            else if (shared->eventOverflow) { reason = fog::constants::error::Stream::SignalingControlEventQueueCapacityExceeded; mayRetry = false; }
            else if (shared->callbackFailure) { reason = fog::constants::error::Stream::StreamCallbackFailed; mayRetry = false; }
        }
        // Seal queued input before ICE's final-neutral allowance becomes
        // visible. No ordinary press may start in the shutdown handoff gap.
        if (controlSender)
            controlSender->requestStop(mayRetry ? std::string{} : neutralControl(), !mayRetry);
        if (rtcpWorker) {
            rtcpWorker->requestStop();
            if (mayRetry && !rtcpWorker->quiescent()) {
                reason = fog::constants::error::Stream::RTCPSendInFlightEndingRentalBeforeTransportCleanup; mayRetry = false;
            }
        }
        if (iceWorker) {
            iceWorker->requestStop();
            if (mayRetry && iceWorker->state().active) {
                reason = fog::constants::error::Stream::ICESnapshotInFlightEndingRentalBeforeTransportCleanup; mayRetry = false;
            }
        }
        if (controlSender) {
            // A completed failure may retry, but never join a still-running RTC
            // call before the owner can dispatch its rental-stop request.
            if (mayRetry && controlSender->snapshot().transportActive) {
                reason = fog::constants::error::Stream::ControlOperationBlockedDuringTransportFailure; mayRetry = false;
            }
        }
        if (failure.empty()) { failure = reason; retryableFailure = mayRetry; }
        // Publish failure without waiting for an in-flight synchronous RTC send.
        // The owner sees error() this frame and can dispatch rental termination
        // before close()/retry joins the worker. Keep transport ownership intact
        // until that explicit lifecycle boundary; callbacks cannot grow queues.
        if (controlSender || iceWorker || rtcpWorker) {
            if (shared) {
                TimedMutex::Guard lock(shared->mutex, TimedMutex::Scope::Client);
                shared->stopped = true;
            }
            running = false; isReady = false; state = "Failed";
            return;
        }
        // A recoverable transport loss must never send the remote close notification.
        stop(!retryableFailure);
    }
    void createPeer(const Json& init) {
        operation = "init.ice-servers";
        if (peer) throw TransportError(fog::constants::error::Stream::DuplicatePeerInitialization);
        rtc::Configuration config;
        config.disableAutoNegotiation = true;
        config.maxMessageSize = 64 * 1024;
        // Match the Android protocol: relay constrains the provider peer.
        // Local ALL permits a direct candidate paired with a remote TURN relay.
        config.iceTransportPolicy = rtc::TransportPolicy::All;
        for (const auto& item : protocol::iceServers(init)) {
            rtc::IceServer ice(item.url);
            ice.username = item.username; ice.password = item.credential;
            config.iceServers.push_back(std::move(ice));
        }
        // A frozen retry shutdown validates pending protocol but starts no new transport.
        if (closingForRetry) return;
        operation = "init.peer-create";
        peer = std::make_shared<rtc::PeerConnection>(config);
        iceWorker = std::make_shared<IceWorker>();
        rtcpWorker = std::make_shared<RtcpFeedbackWorker>();
        rtcpWorker->start();
        {
            TimedMutex::Guard lock(shared->mutex, TimedMutex::Scope::Client);
            shared->iceWorker = iceWorker; shared->relay = relay;
            shared->rtcpWorker = rtcpWorker;
        }
        invalidateRoute();
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
                dc->onMessage([weak](rtc::message_variant msg) {
                    callback(weak, [&msg](Shared& st) {
                        if (const auto text = std::get_if<std::string>(&msg))
                            st.controlMessage(*text);
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
                        TimedMutex::Guard lock(s.mutex, TimedMutex::Scope::Callback);
                        if (s.stopped) return;
                        if (s.videoTrack) throw TransportError(fog::constants::error::Stream::DuplicateVideoTrack);
                        s.videoTrack = true;
                        s.repair = RtpRepair(protocol::videoRepairConfig(static_cast<std::string>(desc)));
                        s.video.enableNack(s.repair.config().nack());
                    }
                    depacketizer = std::make_shared<VideoRtpHandler>([weak](const std::byte* data, std::size_t size, const rtc::message_callback& send) {
                        callback(weak, [data, size, &send](Shared& st) { st.videoRtp(data, size, send); });

                    });
                } else if (desc.type() == "audio") {
                    kind = Kind::Audio; depacketizer = std::make_shared<rtc::OpusRtpDepacketizer>();
                } else return;
                // incomingChain runs next first: RTCP sees original RTP before depacketization.
                auto receiver = std::make_shared<rtc::RtcpReceivingSession>();
                receiver->setDeferredReports(true);
                std::shared_ptr<RtcpFeedbackWorker> feedback;
                {
                    TimedMutex::Guard lock(s.mutex, TimedMutex::Scope::Callback);
                    if (s.stopped) return;
                    feedback = s.rtcpWorker;
                }
                if (!feedback || !feedback->addSession(receiver))
                    throw TransportError(fog::constants::error::Stream::RTCPReceiverRegistrationFailed);
                depacketizer->addToChain(receiver);
                if (kind == Kind::Video) depacketizer->addToChain(std::make_shared<RtxRtpHandler>([weak](rtc::binary& packet, bool control) {
                    bool accepted = false;
                    callback(weak, [&packet, &accepted, control](Shared& st) { accepted = st.normalizeVideo(packet, control); });
                    return accepted;
                }));
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
                {"control_only", false}, {"iceTransportPolicy", relay ? "relay" : "all"}, {"gamepad", true}, {"isPS", playStation}});
    }
    void candidate(const Json& candidate) {
        operation = "candidate.validate";
        if (!candidate.is_object() || candidate.at("candidate").get<std::string>().size() > 8192)
            throw TransportError(fog::constants::error::Stream::InvalidICECandidate);
        // Browser end-of-candidates objects need neither a mid nor an ICE parser.
        if (candidate.at("candidate").get<std::string>().empty()) { ++emptyCandidates; return; }
        if (!peer || !remoteSet) {
            if (pendingCandidates.size() >= 64) throw TransportError(fog::constants::error::Stream::ICEQueueLimit);
            pendingCandidates.push_back(candidate); return;
        }
        std::string mid;
        if (candidate.contains("sdpMid") && candidate["sdpMid"].is_string()) mid = candidate["sdpMid"].get<std::string>();
        else {
            const auto index = candidate.at("sdpMLineIndex").get<int>();
            if (index < 0 || static_cast<std::size_t>(index) >= mids.size()) throw TransportError(fog::constants::error::Stream::InvalidICEMediaIndex);
            mid = mids[index];
        }
        if (std::find(mids.begin(), mids.end(), mid) == mids.end()) throw TransportError(fog::constants::error::Stream::InvalidICEMid);
        const auto value = candidate.at("candidate").get<std::string>();
        operation = "candidate.apply";
        rtc::Candidate parsed(value, mid);
        peer->addRemoteCandidate(parsed);
        addCandidateProof(std::move(parsed));
    }
    void message(const Json& message) {
        operation = "signal.envelope";
        const auto id = message.at("id").get<std::string>();
        if (id == "init") { ++signalInitMessages; createPeer(message); }
        else if (id == "offer") {
            operation = "offer.envelope";
            const auto encoded = message.at("data").get<std::string>();
            if (encoded.empty()) return; // Official Android ignores empty offer notifications.
            if (!peer) throw TransportError(fog::constants::error::Stream::UnexpectedOffer);
            if (remoteSet) throw TransportError(fog::constants::error::Stream::RemoteRenegotiationUnsupported);
            ++offers;
            operation = "offer.base64";
            const auto decoded = protocol::base64Decode(encoded);
            operation = "offer.json";
            const auto offer = Json::parse(decoded);
            if (offer.at("type") != "offer") throw TransportError(fog::constants::error::Stream::InvalidOfferType);
            operation = "offer.filter";
            const auto sdp = protocol::filterOffer(offer.at("sdp").get<std::string>(), videoSettings.codec);
            selectedCodec = protocol::offeredVideoCodec(sdp);
            {
                TimedMutex::Guard lock(shared->mutex, TimedMutex::Scope::Client);
                shared->video = H264RtpAssembler(selectedCodec);
            }
            operation = "offer.mids";
            mids = protocol::mediaMids(sdp);
            if (mids.empty() || mids.size() > 16 || std::any_of(mids.begin(), mids.end(), [](const std::string& mid) { return mid.empty() || mid.size() > 64; }))
                throw TransportError(fog::constants::error::Stream::MissingSDPMid);
            auto uniqueMids = mids;
            std::sort(uniqueMids.begin(), uniqueMids.end());
            if (std::adjacent_find(uniqueMids.begin(), uniqueMids.end()) != uniqueMids.end())
                throw TransportError(fog::constants::error::Stream::DuplicateSDPMid);
            operation = "offer.parse-sdp";
            rtc::Description remote(sdp, "offer");
            // v0.24.1 demux indexes individual SSRC attributes, not FID groups.
            // Register only sources explicitly associated by this offer; no
            // guessed source IDs or fabricated CNAMEs are introduced.
            for (int index = 0; index < remote.mediaCount(); ++index) {
                auto entry = remote.media(index);
                const auto media = std::get_if<rtc::Description::Media*>(&entry);
                if (!media || (*media)->type() != "video" || (*media)->isRemoved()) continue;
                const auto repair = protocol::videoRepairConfig(static_cast<std::string>(**media));
                if (repair.rtx.empty() || !repair.mediaSsrc || !repair.rtxSsrc) continue;
                if (!(*media)->hasSSRC(repair.mediaSsrc)) (*media)->addSSRC(repair.mediaSsrc, std::nullopt);
                if (!(*media)->hasSSRC(repair.rtxSsrc)) (*media)->addSSRC(repair.rtxSsrc, std::nullopt);
            }
            operation = "offer.set-remote";
            peer->setRemoteDescription(remote);
            for (const auto& embedded : remote.candidates()) addCandidateProof(embedded);
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
            if (id == "close") throw TransportError(fog::constants::error::Stream::RemoteSignalingClosedStream);
            if (id == "error") throw TransportError(fog::constants::error::Stream::RemoteSignalingError);
            throw TransportError(relay ? fog::constants::error::Stream::RelayTransportUnavailableRemoteInitializationRejected : fog::constants::error::Stream::RemoteWebRTCInitializationRejected);
        } else if (id == "ping") {
            Json pong{{"id", "pong"}};
            if (message.contains("ts")) pong["ts"] = message["ts"];
            signal(pong);
        }
    }
    void event(Event& event) {
        switch (event.type) {
#ifdef FOG_STREAM_TESTING
        case EventType::TestCandidateProof:
            addCandidateProof(rtc::Candidate(event.text)); break;
#endif
        case EventType::WsOpen:
            state = "Connecting signaling";
            sendSignaling(protocol::frame("CONNECT", {{"accept-version", "1.1,1.2"},
                       {"heart-beat", "4000,4000"}, {"host", "remoteplay.mts.ru"}}));
            break;
        case EventType::WsData: {
            ++wsMessages;
            operation = "stomp.parse";
            lastWs = Clock::now();
            std::string frameTransportFailure;
            for (const auto& frame : parser.feed(event.text)) {
                ++stompFrames;
                try {
                if (frame.command == "CONNECTED") {
                    ++stompConnectedFrames;
                    if (stompConnected) throw TransportError(fog::constants::error::Stream::DuplicateSTOMPConnection);
                    stompConnected = true;
                    const auto hb = frame.headers.find("heart-beat");
                    if (hb != frame.headers.end()) {
                        const auto comma = hb->second.find(',');
                        if (comma == std::string::npos) throw TransportError(fog::constants::error::Stream::InvalidHeartbeatNegotiation);
                        const auto interval = [](const std::string& value) {
                            if (value.empty() || value.size() > 5 ||
                                !std::all_of(value.begin(), value.end(), [](unsigned char c) { return c >= '0' && c <= '9'; }))
                                throw TransportError(fog::constants::error::Stream::InvalidHeartbeatInterval);
                            const auto parsed = std::stoi(value);
                            if (parsed > 60000) throw TransportError(fog::constants::error::Stream::InvalidHeartbeatInterval);
                            return parsed;
                        };
                        const auto outgoing = interval(hb->second.substr(comma + 1));
                        const auto incoming = interval(hb->second.substr(0, comma));
                        heartbeat = outgoing == 0 ? std::chrono::milliseconds(0) : std::chrono::milliseconds(std::max(4000, outgoing));
                        incomingHeartbeat = incoming == 0 ? std::chrono::milliseconds(0) : std::chrono::milliseconds(std::max(4000, incoming));
                    }
                    sendSignaling(protocol::frame("SUBSCRIBE", {{"destination", "/topic/" + clientHash + "/client"},
                               {"ack", "auto"}, {"id", "nx-" + suffix}}), &subscribeSends);
                    signal({{"id", "enabled"}, {"client_hash", clientHash}}, &enabledSends);
                    state = "Waiting for remote host";
                } else if (frame.command == "MESSAGE") {
                    ++stompMessageFrames; operation = "signal.json"; message(Json::parse(frame.body));
                } else if (frame.command == "ERROR") {
                    ++stompErrorFrames; throw TransportError(fog::constants::error::Stream::STOMPRejectedStream);
                } else ++stompOtherFrames;
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
            if (it == mids.end()) throw TransportError(fog::constants::error::Stream::UnknownLocalICEMid);
            const Json c{{"candidate", event.text}, {"sdpMid", event.mid}, {"sdpMLineIndex", std::distance(mids.begin(), it)}};
            signal({{"id", "ice_candidate"}, {"data", protocol::base64Encode(c.dump())}});
            break;
        }
        case EventType::Channel:
            if (channel) throw TransportError(fog::constants::error::Stream::DuplicateControlChannel);
            channel = event.channel;
            {
                const auto dc = channel;
                const std::weak_ptr<Shared> weak(shared);
                controlSender = std::make_shared<AsyncControlSender>(
                    [dc, weak] {
                        {
                            const auto s = weak.lock();
                            if (!s) return false;
                            TimedMutex::Guard lock(s->mutex, TimedMutex::Scope::Callback);
                            if (s->relay) {
                                if (!s->turnAuthorized || !s->iceWorker) return false;
                                const auto ice = s->iceWorker->state();
                                // A final neutral/release may use the last fresh
                                // authorized route after sealing this worker.
                                const bool same = s->routeRevision == ice.revision ||
                                    (ice.stopping && s->routeRevision + 1 == ice.revision);
                                if (!same || Clock::now() - s->routeCapturedAt >=
                                    std::chrono::milliseconds(RouteSnapshotSchedule::IntervalMs * 2)) return false;
                            }
                        }
                        return dc->isOpen() && dc->bufferedAmount() <= 64 * 1024;
                    },
                    [dc
#ifdef FOG_STREAM_TESTING
                     , fixture = controlSendFixture
#endif
                    ](const std::string& payload) {
#ifdef FOG_STREAM_TESTING
                        fixture->wait();
#endif
                        dc->send(payload);
                    },
                    [weak](bool sent) {
                        callback(weak, [sent](Shared& s) {
                            TimedMutex::Guard lock(s.mutex, TimedMutex::Scope::Callback);
                            if (!s.stopped) { if (sent) s.incoming.pongSent(); else s.incoming.pongFailed(); }
                        });
                    });
                TimedMutex::Guard lock(shared->mutex, TimedMutex::Scope::Client);
                if (!shared->pendingPong.empty()) {
                    controlSender->enqueue(std::move(shared->pendingPong), ControlQueue::Mode::Pong);
                    shared->pendingPong.clear();
                }
                shared->controlSender = controlSender;
            }
            break;
        case EventType::ChannelOpen:
            // Open only after this entire batch's ICE/provenance is verified.
            break;
        case EventType::Control:
            operation = "control.json";
            if (event.text == "close") throw TransportError(fog::constants::error::Stream::RemoteControlClosed);
            if (event.text == "error") throw TransportError(fog::constants::error::Stream::RemoteControlError);
            throw TransportError(fog::constants::error::Stream::MalformedControlMessage);
        case EventType::Track: tracks.push_back(event.track); break;
        case EventType::PeerState:
            qualityEvent("transportEvent=peer state=" + std::to_string(static_cast<int>(event.peerState)) + " ms=" + std::to_string(event.arrivedMs));
            if (event.peerState == rtc::PeerConnection::State::Failed)
                throw TransportError(relay ? fog::constants::error::Stream::RelayTransportUnavailablePeerFailed : fog::constants::error::Stream::PeerConnectionFailed, true);
            if (event.peerState == rtc::PeerConnection::State::Closed) throw TransportError(fog::constants::error::Stream::PeerConnectionClosed, true);
            if (!isReady) {
                if (event.peerState == rtc::PeerConnection::State::Connecting) state = "Connecting peer";
                else if (event.peerState == rtc::PeerConnection::State::Connected) state = channelOpen ? "Waiting for video" : "Waiting for video/control";
            }
            break;
        case EventType::IceState:
            if (event.iceState == rtc::PeerConnection::IceState::Failed)
                throw TransportError(relay ? fog::constants::error::Stream::RelayTransportUnavailableICEFailed : fog::constants::error::Stream::ICEConnectionFailed, true);
            if (event.iceState == rtc::PeerConnection::IceState::Closed)
                throw TransportError(fog::constants::error::Stream::ICEConnectionClosed, true);
            qualityEvent("transportEvent=ice state=" + std::to_string(static_cast<int>(event.iceState)) + " ms=" + std::to_string(event.arrivedMs));
            break;
        case EventType::GatheringState:
            qualityEvent("transportEvent=gathering state=" + std::to_string(static_cast<int>(event.gatheringState)) + " ms=" + std::to_string(event.arrivedMs));
            break;
        case EventType::WsClosed:
            throw TransportError(fog::constants::error::Stream::SignalingTransportClosed, true);
        case EventType::Failure:
            throw TransportError(event.text.empty() ? fog::constants::error::Stream::TransportFailed : event.text.c_str(), true);
        }
    }
};

Client::Client() : impl_(std::make_unique<Impl>()) {}
Client::Client(std::string caPath, std::string random5suffix)
    : impl_(std::make_unique<Impl>()), caPath_(std::move(caPath)), random5suffix_(std::move(random5suffix)) {}
bool Client::start(const StreamConnection& connection) {
    if (connection.rental.provider != fog::ProviderId::Mts) return false;
    return start(connection.rental.value, random5suffix_, caPath_);
}
Client::~Client() { close(); }
bool Client::start(const std::string& hash, const std::string& suffix, const std::string& caPath) {
    return startEndpoint(hash, suffix, caPath, "wss://remoteplay.mts.ru/ws/");
}
#ifdef FOG_STREAM_TESTING
void Client::overrideSignalingSendForTesting(bool enabled, bool reject) {
    impl_->testSignalingSend = enabled ? &impl_->enabledSends : &impl_->subscribeSends;
    impl_->testSignalingReject = reject;
}
bool Client::startForTesting(const std::string& hash, const std::string& suffix, const std::string& url) {
    if (url.rfind("ws://127.0.0.1:", 0) != 0) return false;
    return startEndpoint(hash, suffix, "unused-loopback.pem", url);
}
void Client::advanceQualityForTesting(unsigned milliseconds) {
    impl_->qualityTimeOffset += milliseconds;
    if (impl_->shared) {
        TimedMutex::Guard lock(impl_->shared->mutex, TimedMutex::Scope::Client);
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
    } else impl_->shared->controlMessage(text);
}
void Client::setSelectedCandidatesForTesting(const std::string& local, const std::string& remote) {
    impl_->testLocal = rtc::Candidate(local); impl_->testRemote = rtc::Candidate(remote);
    impl_->testLocal.resolve(rtc::Candidate::ResolveMode::Simple);
    impl_->testRemote.resolve(rtc::Candidate::ResolveMode::Simple);
    impl_->routeOverride = impl_->testPairAvailable = true;
    impl_->invalidateRoute();
}
void Client::clearSelectedCandidatesForTesting(bool invalidate) {
    impl_->routeOverride = true; impl_->testPairAvailable = false;
    if (invalidate) impl_->invalidateRoute();
}
void Client::failSelectedPairForTesting(bool fail) {
    impl_->testPairThrows = fail;
    impl_->invalidateRoute();
}
void Client::setRouteSnapshotTimeForTesting(std::uint64_t now) {
    impl_->fixedRouteClock = now != 0; impl_->routeClock = now;
}
void Client::repeatIceStateForTesting() {
    Impl::Event event(Impl::EventType::IceState);
    {
        TimedMutex::Guard lock(impl_->shared->mutex, TimedMutex::Scope::Client);
        event.iceState = impl_->shared->iceState;
    }
    impl_->shared->event(std::move(event));
}
void Client::addCandidateProofForTesting(const std::string& text) {
    Impl::Event event(Impl::EventType::TestCandidateProof); event.text = text;
    impl_->shared->event(std::move(event));
}
bool Client::selectedRemoteMatchesForTesting(const std::string& text) const {
    rtc::Candidate candidate(text);
    candidate.resolve(rtc::Candidate::ResolveMode::Simple);
    return impl_->routeSelected && candidate.address() == impl_->selectedRemote.address() &&
        candidate.port() == impl_->selectedRemote.port() && candidate.transportType() == impl_->selectedRemote.transportType();
}
void Client::receiveControlForTesting(const std::string& text) {
    if (impl_->shared) impl_->shared->controlMessage(text);
}
void Client::blockIceForTesting(bool blocked, unsigned stage) {
    const auto fixture = impl_->iceReadFixture;
    std::lock_guard<std::mutex> lock(fixture->mutex);
    fixture->blocked = blocked; fixture->stage = stage; fixture->condition.notify_all();
    if (blocked) impl_->invalidateRoute();
}
void Client::blockControlForTesting(bool blocked) { if (impl_->controlSender) impl_->controlSender->block(blocked); }
void Client::blockControlSendForTesting(bool blocked) {
    const auto fixture = impl_->controlSendFixture;
    std::lock_guard<std::mutex> lock(fixture->mutex);
    fixture->blocked = blocked; fixture->condition.notify_all();
}
void Client::advanceControlForTesting(unsigned milliseconds) { if (impl_->controlSender) impl_->controlSender->advance(milliseconds); }
void Client::elapseForTesting(unsigned seconds) {
    const auto elapsed = std::chrono::seconds(seconds);
    auto& i = *impl_;
    i.qualityTimeOffset += static_cast<std::uint64_t>(seconds) * 1000;
    i.started -= elapsed;
    i.lastWs -= elapsed;
    i.lastHeartbeat -= elapsed;
    i.lastPli -= elapsed;
    if (i.unprovenRouteAt != Clock::time_point{}) i.unprovenRouteAt -= elapsed;
    if (i.shared) {
        TimedMutex::Guard lock(i.shared->mutex, TimedMutex::Scope::Client);
        i.shared->qualityTimeOffset = i.qualityTimeOffset;
        if (i.shared->lastVideo != Clock::time_point{}) i.shared->lastVideo -= elapsed;
    }
}
#endif
bool Client::startEndpoint(const std::string& hash, const std::string& suffix,
                           const std::string& caPath, const std::string& url) {
    const auto preferences = impl_->videoSettings;
    const auto relay = impl_->relay;
    const auto playStation = impl_->playStation;
    close();
    impl_ = std::make_unique<Impl>();
    impl_->videoSettings = preferences;
    impl_->relay = relay;
    impl_->playStation = playStation;
    auto& i = *impl_;
    // Do not permit STOMP header/destination injection.
    const auto safe = [](const std::string& s) {
        return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        });
    };
    if (hash.size() > 256 || !safe(hash) || suffix.size() != 5 || !safe(suffix) || caPath.empty()) {
        i.failure = fog::constants::error::Stream::InvalidStreamConfiguration; i.state = "Failed"; return false;
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
    } catch (const std::exception&) { i.fail(fog::constants::error::Stream::StreamStartupFailed); return false; }
}
void Client::pump() {
    auto& i = *impl_;
    if (!i.running || !i.shared) return;
    TransportStageScope pumpTiming{i.pumpTiming};
    { TransportStageScope timing{i.nackTiming}; i.shared->sendNacks(); }
    bool eventOverflow, callbackFailure, needsPli, statsPending;
    quality::Telemetry stats;
    Clock::time_point lastVideo;
    std::deque<Impl::Event> events;
    try {
        TransportStageScope timing{i.receiveTiming};
        TimedMutex::Guard lock(i.shared->mutex, TimedMutex::Scope::Client);
        i.shared->video.tick(videoNow(), i.shared->videoFrameBudget()); i.shared->drainVideo();
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
        i.fail(fog::constants::error::Stream::VideoReceiveProcessingFailed); return;
    }
    if (eventOverflow) { i.fail(fog::constants::error::Stream::SignalingControlEventQueueCapacityExceeded); return; }
    if (callbackFailure) { i.fail(fog::constants::error::Stream::StreamCallbackFailed); return; }
    try {
        // Inspect the whole bounded batch before committing a retryable failure.
        // A later remote close/error or malformed protocol must remain terminal.
        // A channel event can precede a state/candidate event in the same batch.
        // Invalidate before dispatching any event that might send control.
        for (const auto& event : events)
            if (event.type == Impl::EventType::PeerState || event.type == Impl::EventType::IceState ||
                event.type == Impl::EventType::GatheringState || event.type == Impl::EventType::Candidate ||
                event.type == Impl::EventType::Description || event.type == Impl::EventType::Channel ||
                event.type == Impl::EventType::ChannelOpen) {
                i.invalidateRoute(); break;
            }
        std::string transportFailure;
        {
            TransportStageScope timing{i.eventsTiming};
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
        }
        if (i.closingForRetry) return;
        // New proof/state changes win over the unproven-route deadline, even
        // between periodic queries. Unknown pairs also obey the interval.
        try { i.snapshotIce(); } catch (...) {} // Failed query leaves required TURN gated.
        if (i.relay && i.currentRoute() && i.route.isDirect())
            throw TransportError(fog::constants::error::Stream::TURNRequiredButDirectPathSelected);
        const auto routeNow = Clock::now();
        if (!i.routeAllowed()) {
            i.isReady = false; i.state = "Waiting for TURN route";
            if (i.unprovenRouteAt == Clock::time_point{}) i.unprovenRouteAt = routeNow;
            if (routeNow - i.unprovenRouteAt >= std::chrono::seconds(30)) {
                i.fail(fog::constants::error::Stream::RelayTransportUnavailableSelectedRouteUnproven, true); return;
            }
        } else i.unprovenRouteAt = {};
        {
            TransportStageScope timing{i.qualityTiming};
            i.openChannel(); // A pending channel may now have a verified TURN route.
            i.applyQuality();
        }
        i.flushControl();
        const auto now = Clock::now();
        if (i.channelOpen && i.routeAllowed() && lastVideo != Clock::time_point{}) {
            i.isReady = true; i.wasReady = true; i.state = "Streaming";
        }
        if (!i.wasReady && !i.isReady && now - i.started >= std::chrono::seconds(30)) {
            i.fail(i.relay ? fog::constants::error::Stream::RelayTransportUnavailableConnectionTimedOut : fog::constants::error::Stream::StreamConnectionTimedOut, true); return;
        }
        if (i.isReady && now - lastVideo >= std::chrono::seconds(15)) { i.fail(fog::constants::error::Stream::VideoStreamStalled, true); return; }
        // STOMP's server interval is the first negotiated value. A zero value
        // disables incoming heartbeats; setup and received-video deadlines still apply.
        if (i.stompConnected && i.incomingHeartbeat.count() &&
            now - i.lastWs >= std::max(std::chrono::milliseconds(15000), i.incomingHeartbeat * 3)) {
            i.fail(fog::constants::error::Stream::SignalingStreamStalled, true); return;
        }
        if (i.stompConnected && i.heartbeat.count() && now - i.lastHeartbeat >= i.heartbeat) {
            TransportStageScope timing{i.heartbeatTiming};
            i.sendSignaling(std::string("\n"));
            i.lastHeartbeat = now; ++i.heartbeatsSent;
        }
        if (i.routeAllowed() && ((i.videoSettings.keyframeSeconds > 0 && now - i.lastPli >= std::chrono::seconds(i.videoSettings.keyframeSeconds)) ||
            (needsPli && now - i.lastPli >= std::chrono::milliseconds(250)))) {
            TransportStageScope timing{i.pliTiming};
            bool requested = false;
            for (const auto& track : i.tracks)
                if (track->description().type() == "video" && track->isOpen()) {
                    track->requestKeyframe(); requested = true;
                }
            if (requested) {
                i.lastPli = now;
                TimedMutex::Guard lock(i.shared->mutex, TimedMutex::Scope::Client);
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
    if (!i.shared || !i.running || !i.routeAllowed()) return false;
    TimedMutex::Guard lock(i.shared->mutex, TimedMutex::Scope::Client);
    i.shared->expireMedia(videoNow());
    return i.shared->media.pop(packet);
}
bool Client::pollAudio(Packet& packet) {
    auto& i = *impl_;
    if (!i.shared || !i.running || !i.routeAllowed()) return false;
    TimedMutex::Guard lock(i.shared->mutex, TimedMutex::Scope::Client);
    i.shared->expireMedia(videoNow());
    return i.shared->media.popAudio(packet);
}
std::uint64_t Client::queuedVideoAgeMs() const {
    if (!impl_->shared) return 0;
    TimedMutex::Guard lock(impl_->shared->mutex, TimedMutex::Scope::Client);
    return impl_->shared->media.age(Kind::Video, videoNow());
}
bool Client::configureVideo(const fog::video::Settings& settings) {
    if (impl_->running || !fog::video::valid(settings)) return false;
    impl_->videoSettings = settings;
    return true;
}
bool Client::configureRelay(bool relay) {
    if (impl_->running) return false;
    impl_->relay = relay;
    return true;
}
bool Client::configurePlayStation(bool enabled) {
    if (impl_->running || impl_->shared) return false;
    impl_->playStation = enabled;
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
    TimedMutex::Guard lock(impl_->shared->mutex, TimedMutex::Scope::Client);
    return impl_->shared->media.videoCount;
}
void Client::observePlayback(const PlaybackObservation& observation) {
    auto& i = *impl_;
    if (!i.running || !i.shared || !i.channelOpen || !i.routeAllowed() || i.videoSettings.quality != 0 || i.qualityPending) return;
    quality::Sample sample;
    sample.frames = observation.presentedFrames; sample.decodedFrames = observation.decodedFrames;
    sample.supersededFrames = observation.supersededFrames;
    sample.workUs = observation.workUs; sample.errors = observation.errors;
    sample.audioUnderruns = observation.audioUnderruns; sample.audioResets = observation.audioResets;
    sample.recovering = observation.recovering; sample.width = observation.width; sample.height = observation.height;
    {
        TimedMutex::Guard lock(i.shared->mutex, TimedMutex::Scope::Client);
        const auto& stats = i.shared->video.stats();
        sample.missing = stats.missing; sample.faults = stats.faultSerial; sample.recoveries = stats.resyncs; sample.pressure = stats.pressure;
        sample.recovering |= i.shared->video.waiting();
        // Auto sees only work left AFTER this frame's bounded drain. A transient
        // predrain burst is not evidence of sustained decoder congestion.
        sample.audioDropped = i.shared->media.audioDropped;
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
bool Client::ready() const { return impl_->isReady && impl_->routeAllowed(); }
std::string Client::status() const { return impl_->state; }
std::string Client::error() const { return impl_->failure; }
bool Client::retryableError() const { return !impl_->failure.empty() && impl_->retryableFailure; }
bool Client::disconnected() const {
    if (!impl_->running || !impl_->shared) return false;
    TimedMutex::Guard lock(impl_->shared->mutex, TimedMutex::Scope::Client);
    return impl_->shared->peerState == rtc::PeerConnection::State::Disconnected ||
           impl_->shared->iceState == rtc::PeerConnection::IceState::Disconnected;
}
bool Client::disconnectForRetry() {
    auto& i = *impl_;
    if (!i.running || !i.shared) {
        if (!i.failure.empty() && !i.retryableFailure) return false;
        if (i.shared) i.stop(false);
        return true;
    }
    // Seal before inspecting activity: after this boundary no new probe/send or
    // shutdown-neutral operation may start on a transport selected for retry.
    if (i.controlSender) i.controlSender->requestStop({}, false);
    if (i.iceWorker) i.iceWorker->requestStop();
    if (i.rtcpWorker) i.rtcpWorker->requestStop();
    {
        TimedMutex::Guard lock(i.shared->mutex, TimedMutex::Scope::Client);
        i.shared->stopped = true;
    }
    i.closingForRetry = true;
    pump(); // Frozen terminal evidence still wins over transport-only recovery.
    if (!i.running) {
        if (i.retryableFailure) i.stop(false);
        return i.retryableFailure;
    }
    if (i.rtcpWorker && !i.rtcpWorker->quiescent()) {
        i.fail(fog::constants::error::Stream::RTCPSendInFlightEndingRentalBeforeTransportCleanup);
        return false; // HTTP rental stop precedes an uncancellable send join.
    }
    if (i.iceWorker && i.iceWorker->state().active) {
        i.fail(fog::constants::error::Stream::ICESnapshotInFlightEndingRentalBeforeTransportCleanup);
        return false; // Rental HTTP stop precedes the unavoidable close join.
    }
    if (i.controlSender) {
        const auto sender = i.controlSender->snapshot();
        if (sender.stalled || sender.transportActive) {
            i.fail(sender.stalled ? fog::constants::error::Stream::ControlChannelStalled : fog::constants::error::Stream::ControlOperationBlockedDuringTransportFailure);
            return false; // Owner dispatches rental stop BEFORE close/destructor.
        }
        if (sender.failure == AsyncControlSender::Failure::Unexpected) {
            i.fail(fog::constants::error::Stream::ControlSenderFailed); return false;
        }
    }
    i.stop(false);
    return true;
}
std::string Client::diagnostics() const { return impl_->diagnostics(); }
std::string Client::transportTimings() const { return impl_->transportTimings(); }
std::string Client::rtcpDiagnostics() const {
    return (impl_->rtcpWorker ? impl_->rtcpWorker->snapshot() : impl_->finalRtcp).text();
}
std::string Client::dtlsDiagnostics() const {
    // Worker-owned snapshot only; logging never enters RTC transport locks.
    return dtlsDiagnosticText(impl_->finalDtlsDiagnostics);
}

std::string Client::mutexDiagnostics() const {
    if (!impl_->shared) return impl_->finalMutexDiagnostics;
    TimedMutex::Snapshot snapshot;
    {
        TimedMutex::Guard lock(impl_->shared->mutex, TimedMutex::Scope::Diagnostics);
        snapshot = lock.snapshot();
    }
    return TimedMutex::diagnostics(snapshot);
}
std::string Client::queueDiagnostics() const {
    if (!impl_->shared) return impl_->finalQueueDiagnostics;
    TimedMutex::Guard lock(impl_->shared->mutex, TimedMutex::Scope::Diagnostics);
    return impl_->shared->media.pressureText();
}
std::string Client::gapDiagnostics() const {
    if (!impl_->shared) return impl_->finalGapDiagnostics;
    TimedMutex::Guard lock(impl_->shared->mutex, TimedMutex::Scope::Diagnostics);
    return impl_->shared->video.gapDiagnostics();
}
std::string Client::videoDiagnostics() const {
    if (!impl_->shared) return impl_->finalVideoDiagnostics;
    TimedMutex::Guard lock(impl_->shared->mutex, TimedMutex::Scope::Diagnostics);
    return impl_->shared->videoDiagnostics() + impl_->qualityDiagnostics();
}
bool Client::pollVideoDiagnostic(std::string& event) {
    if (!impl_->routeEvents.empty()) {
        event = std::move(impl_->routeEvents.front()); impl_->routeEvents.pop_front(); return true;
    }
    if (!impl_->qualityEvents.empty()) {
        event = std::move(impl_->qualityEvents.front()); impl_->qualityEvents.pop_front(); return true;
    }
    if (impl_->autoQuality.pollEvent(event)) return true;
    if (impl_->shared) {
        TimedMutex::Guard lock(impl_->shared->mutex, TimedMutex::Scope::Diagnostics);
        return impl_->shared->video.pollEvent(event);
    }
    if (impl_->finalVideoEvents.empty()) return false;
    event = std::move(impl_->finalVideoEvents.front()); impl_->finalVideoEvents.pop_front(); return true;
}
bool Client::waitingForVideoKeyframe() const {
    if (!impl_->shared) return false;
    TimedMutex::Guard lock(impl_->shared->mutex, TimedMutex::Scope::Client);
    return impl_->shared->videoTrack && impl_->shared->video.waiting();
}
void Client::setDecoderWaitingForKeyframe(bool waiting) { impl_->decoderWaiting = waiting; }
bool Client::requestVideoKeyframe() {
    if (!impl_->running || !impl_->routeAllowed() || Clock::now() - impl_->lastPli < std::chrono::milliseconds(250)) return false;
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
                TimedMutex::Guard lock(impl_->shared->mutex, TimedMutex::Scope::Client);
                ++impl_->shared->pliRequests;
            }
        }
    } catch (...) { return false; }
    return requested;
}
std::string Client::inputDiagnostics() const {
    const auto out = impl_->controlSender ? impl_->controlSender->snapshot() : impl_->finalControl;
    auto text = "PS input scope=transport-generation isPS=" + std::to_string(impl_->playStation) +
        " frames=" + std::to_string(out.psFrames) + " accepted=" + std::to_string(out.psAccepted) +
        " rejected=" + std::to_string(out.psRejected) + " unavailable=" + std::to_string(impl_->psUnavailableFrames) +
        " touchQueuedDown=" + std::to_string(out.touchQueuedDown) + " touchQueuedUp=" + std::to_string(out.touchQueuedUp) +
        " touchRejectedDown=" + std::to_string(out.touchRejectedDown) + " touchRejectedUp=" + std::to_string(out.touchRejectedUp) +
        " touchAttemptDown=" + std::to_string(out.touchAttemptDown) + " touchAttemptUp=" + std::to_string(out.touchAttemptUp) +
        " touchSentDown=" + std::to_string(out.touchSentDown) + " touchSentUp=" + std::to_string(out.touchSentUp) +
        " touchFailedDown=" + std::to_string(out.touchFailedDown) + " touchFailedUp=" + std::to_string(out.touchFailedUp) +
        " touchDroppedAtStop=" + std::to_string(out.touchDropped) + " stateCoalesced=" + std::to_string(out.gamepadCoalesced) +
        " lastQueuedSeq=" + std::to_string(out.lastQueuedSeq) + " lastAttemptSeq=" + std::to_string(out.lastAttemptSeq) +
        " lastSentSeq=" + std::to_string(out.lastSentSeq) + " invalidNative=" + std::to_string(impl_->psInvalidNative);
    static constexpr const char* names[]{"unused", "nativeDown", "nativeMove", "nativeUp", "accel", "gyro"};
    for (std::size_t n = 1; n < out.nativeQueued.size(); ++n) {
        const std::string name = names[n];
        text += " " + name + "Queued=" + std::to_string(out.nativeQueued[n]) +
            " " + name + "Rejected=" + std::to_string(out.nativeRejected[n]) +
            " " + name + "Attempt=" + std::to_string(out.nativeAttempt[n]) +
            " " + name + "Sent=" + std::to_string(out.nativeSent[n]) +
            " " + name + "Failed=" + std::to_string(out.nativeFailed[n]) +
            " " + name + "Dropped=" + std::to_string(out.nativeDropped[n]) +
            " " + name + "Coalesced=" + std::to_string(out.nativeCoalesced[n]);
    }
    return text + " sendSuccess=local-rtc-acceptance ack=unsupported";
}
bool Client::sendGamepad(const Gamepad& state) {
    if (!impl_->running || !impl_->channelOpen || !impl_->routeAllowed() || !impl_->controlSender) {
        if (impl_->playStation) ++impl_->psUnavailableFrames;
        return false;
    }
    try {
        if (impl_->controlSender->snapshot().stalled) { if (impl_->playStation) ++impl_->psUnavailableFrames; return false; }
        const auto payload = protocol::gamepad(state, impl_->playStation).dump();
        if (impl_->playStation) return impl_->controlSender->enqueueGamepad(payload, state.buttons[17] != 0);
        return impl_->control(payload, ControlQueue::Mode::Gamepad);
    }
    catch (const TransportError& error) {
        // Public send may run before pump. Examine queued terminal signaling first.
        if (error.retryable) {
            if (impl_->pendingTransportFailure.empty()) impl_->pendingTransportFailure = error.what();
        } else impl_->fail(error.what());
        return false;
    } catch (const std::exception&) { impl_->fail(fog::constants::error::Stream::ControlSendFailed); return false; }
}
bool Client::sendInput(const fog::remote::Input& input) {
    try { return sendControl(protocol::remoteInput(input).dump()); }
    catch (const std::exception&) { return false; }
}
bool Client::sendControl(const std::string& json) {
    if (!impl_->running || !impl_->channelOpen || !impl_->routeAllowed() || json.size() > 64 * 1024) return false;
    const auto data = Json::parse(json, nullptr, false);
    if (!data.is_object()) return false;
    auto mode = ControlQueue::Mode::Ordered;
    std::string input, release;
    bool psGamepad = false, psTouchDown = false;
    unsigned nativeInput = 0;
    const auto id = data.find("id");
    if (id != data.end() && id->is_string()) {
        const auto& name = id->get_ref<const std::string&>();
        if (name == "gamepad_touch" || name == "gamepad_sensor") {
            const auto reject = [&] { ++impl_->psInvalidNative; return false; };
            const auto index = data.find("index"), details = data.find("data");
            if (!impl_->playStation || index == data.end() || !index->is_number_integer() || *index != 0 ||
                details == data.end() || !details->is_object()) return reject();
            const auto number = [&](const char* key, double low, double high) {
                const auto value = details->find(key);
                if (value == details->end() || !value->is_number()) return false;
                const auto numeric = value->get<double>();
                return std::isfinite(numeric) && numeric >= low && numeric <= high;
            };
            if (name == "gamepad_touch") {
                const auto type = details->find("type"), finger = details->find("finger");
                if (type == details->end() || !type->is_number_integer() ||
                    (*type != 0x656 && *type != 0x657 && *type != 0x658) ||
                    finger == details->end() || !finger->is_number_integer() || (*finger != 0 && *finger != 1) ||
                    !number("x", 0, 1) || !number("y", 0, 1)) return reject();
                const auto event = type->get<unsigned>();
                nativeInput = event == 0x656 ? 1 : event == 0x657 ? 2 : 3;
                mode = nativeInput == 1 ? ControlQueue::Mode::Ordered : nativeInput == 2 ? ControlQueue::Mode::TouchMove : ControlQueue::Mode::Release;
                input = "touch:" + finger->dump();
                if (nativeInput != 3) { auto up = data; up["data"]["type"] = 0x658; release = up.dump(); }
            } else {
                const auto sensor = details->find("sensor");
                const auto limit = static_cast<double>(std::numeric_limits<float>::max());
                if (sensor == details->end() || !sensor->is_number_integer() || (*sensor != 1 && *sensor != 2) ||
                    !number("x", -limit, limit) || !number("y", -limit, limit) || !number("z", -limit, limit)) return reject();
                nativeInput = sensor->get<unsigned>() == 1 ? 4 : 5;
                mode = ControlQueue::Mode::Motion; input = "sensor:" + sensor->dump();
                auto neutral = data;
                neutral["data"]["x"] = 0.0; neutral["data"]["y"] = nativeInput == 4 ? 9.80665 : 0.0; neutral["data"]["z"] = 0.0;
                release = neutral.dump();
            }
        }
        else if (name == "gamepad") {
            // The negotiated controller type applies to every input path.
            const auto playStation = data.find("isPS");
            if (playStation == data.end() || !playStation->is_boolean() || *playStation != impl_->playStation) return false;
            mode = ControlQueue::Mode::Gamepad;
            if (impl_->playStation) {
                const auto state = data.find("state");
                if (state == data.end() || !state->is_object()) return false;
                const auto buttons = state->find("buttons");
                if (buttons == state->end() || !buttons->is_array() || buttons->size() != 18 || !(*buttons)[17].is_number()) return false;
                psGamepad = true; psTouchDown = (*buttons)[17].get<double>() != 0;
            }
        }
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
        if (!input.empty() && (name == "keydown" || name == "mousedown")) {
            auto up = data; up["id"] = name == "keydown" ? "keyup" : "mouseup"; release = up.dump();
        }
    }
    try {
        if (nativeInput != 0) return impl_->controlSender && !impl_->controlSender->snapshot().stalled &&
            impl_->controlSender->enqueueNative(json, mode, input, release, nativeInput);
        if (psGamepad) return impl_->controlSender && !impl_->controlSender->snapshot().stalled && impl_->controlSender->enqueueGamepad(json, psTouchDown);
        return impl_->control(json, mode, input, release);
    }
    catch (const TransportError& error) {
        // Public send may run before pump. Examine queued terminal signaling first.
        if (error.retryable) {
            if (impl_->pendingTransportFailure.empty()) impl_->pendingTransportFailure = error.what();
        } else impl_->fail(error.what());
        return false;
    } catch (const std::exception&) { impl_->fail(fog::constants::error::Stream::ControlSendFailed); return false; }
}
void Client::close() {
    if (impl_ && (impl_->running || impl_->shared))
        impl_->stop(impl_->failure.empty() || !impl_->retryableFailure);
}
void Client::cleanupRuntime() {
    auto cleanup = rtc::Cleanup();
    cleanup.wait();
    rtc::JoinCleanupThreads();
    cleanup.get();
}
}
