#include "features/streaming/infrastructure/assembly_telemetry_cache.hpp"
#include "features/streaming/infrastructure/bounded_network_events.hpp"
#include "features/streaming/infrastructure/network_telemetry_capture.hpp"
#include "features/streaming/infrastructure/ice_telemetry_capture.hpp"
#include "features/streaming/application/network_diagnostics_formatter.hpp"
#include "features/streaming/infrastructure/telemetry_capture.hpp"
#include "features/streaming/application/stream_diagnostics_formatter.hpp"
#include "features/streaming/infrastructure/input_protocol.hpp"
#include "core/constants/error_text.hpp"
#include "features/streaming/domain/input_experiments.hpp"
#include "features/streaming/infrastructure/stream.hpp"
#include "features/streaming/infrastructure/protocol_telemetry_capture.hpp"
#include "features/streaming/application/network_diagnostics_formatter.hpp"
#include "features/streaming/infrastructure/dtls_diagnostics.hpp"
#include "features/streaming/infrastructure/stream_protocol.hpp"
#include "features/streaming/domain/h264_rtp.hpp"
#include "features/streaming/domain/recovery_pli_schedule.hpp"
#include "features/streaming/infrastructure/remote_candidates.hpp"
#include "features/streaming/infrastructure/video_rtp_handler.hpp"
#include "features/streaming/infrastructure/stream_queue.hpp"
#include "features/streaming/infrastructure/spsc_video_queue.hpp"
#include "features/streaming/infrastructure/async_control_sender.hpp"
#include "features/streaming/infrastructure/async_ice_snapshot.hpp"
#include "features/streaming/infrastructure/udp_poll_capture.hpp"
#include "features/streaming/infrastructure/rtcp_feedback_worker.hpp"
#include "features/streaming/infrastructure/incoming_control.hpp"
#include "features/streaming/infrastructure/server_performance.hpp"
#include "features/streaming/infrastructure/control_delivery.hpp"
#include "features/streaming/infrastructure/rumble_mailbox.hpp"
#include "features/streaming/infrastructure/transport_history.hpp"
#include "features/streaming/infrastructure/timed_mutex.hpp"
#include "features/streaming/infrastructure/stream_route.hpp"
#include "features/streaming/domain/auto_quality.hpp"
#include "features/streaming/domain/resolution_scale.hpp"
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
#include <condition_variable>
#include <thread>
#include "core/diagnostics/thread_observation.hpp"

namespace stream {
using Clock = std::chrono::steady_clock;
using Json = protocol::Json;
static std::optional<ResolutionScale::Table> resolutionTable(const Json& message) {
    const auto entries = message.find("resolutions");
    if (entries == message.end() || !entries->is_array() || entries->empty() ||
        entries->size() > ResolutionScale::MaxEntries) return std::nullopt;
    ResolutionScale::Table table;
    table.reserve(entries->size());
    for (const auto& entry : *entries) {
        if (!entry.is_array() || entry.size() != 2 ||
            !entry[0].is_number_integer() || !entry[1].is_number_integer() ||
            entry[0] < 0 || entry[0] > 16384 || entry[1] < 0 || entry[1] > 16384) return std::nullopt;
        table.push_back({entry[0].get<int>(), entry[1].get<int>()});
    }
    if (!ResolutionScale::valid(table)) return std::nullopt;
    return table;
}
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
static std::uint64_t videoNow() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}
struct Client::Impl {
    struct IceData {
        rtc::IceDiagnostics ice;
        rtc::IceUdpPollDiagnostics udp;
        std::uint64_t udpQueryBeforeUs=0,udpQueryAfterUs=0;
        Clock::time_point iceCapturedAt{};
        rtc::DtlsDiagnostics dtls;
        std::optional<std::chrono::milliseconds> rtt;
        Clock::time_point rttCapturedAt{};
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
        telemetry::protocol::Reliability reliability{};
        std::shared_ptr<rtc::DataChannel> channel;
        std::shared_ptr<rtc::Track> track;
        rtc::PeerConnection::State peerState = rtc::PeerConnection::State::New;
        rtc::PeerConnection::IceState iceState = rtc::PeerConnection::IceState::New;
        rtc::PeerConnection::GatheringState gatheringState = rtc::PeerConnection::GatheringState::New;
        explicit Event(EventType type_) : type(type_) {}
    };
    struct Shared {
        TimedMutex mutex;
        mutable TimedMutex videoMutex;
        mutable std::mutex telemetryMutex;
        mutable std::mutex audioMutex;
        SpscVideoQueue videoQueue;
        // A single atomic authorization snapshot: zero is closed, a finite
        // deadline is verified TURN, max is unrestricted direct transport.
        // In particular, zero never means unbounded authorization.
        std::atomic<std::uint64_t> mediaAllowedUntilMs{0}, publishedVideoGeneration{0};
        std::atomic<VideoCodec> workerCodec{VideoCodec::H264};
        std::shared_ptr<MediaSignals> signals;
        struct VideoIngress {
            std::vector<std::byte> bytes;
            std::uint64_t receivedMs = 0;
        };
        std::mutex ingressMutex;
        // Shared accepts exactly one video track. Its RTC feedback closure
        // captures that track weakly, so register it once instead of copying a
        // std::function for every RTP packet. Transfer to the assembly owner
        // under ingressMutex; only that owner subsequently uses videoFeedback.
        rtc::message_callback ingressFeedback;
        bool feedbackRegistered = false;
        std::condition_variable ingressWake;
        std::deque<VideoIngress> ingress;
        std::size_t ingressBytes = 0;
        std::uint64_t assemblyWakeVersion = 0;
        bool assemblyStopping = false, ingressOverflow = false;
#ifdef FOG_STREAM_TESTING
        bool assemblyPaused = false;
#endif
        std::optional<VideoCodec> assemblyCodec;
        std::atomic<std::uint64_t> ingressDrops{0};
        std::thread assemblyWorker;
        ~Shared() { stopAssembly(); }
        void revokeMedia() {
            mediaAllowedUntilMs.store(0, std::memory_order_release);
            if (signals) signals->notifyAll();
        }
        bool allowsMedia() const {
            const auto until = mediaAllowedUntilMs.load(std::memory_order_acquire);
            return until && videoNow() < until;
        }
        void stopAssembly() noexcept {
            {
                std::lock_guard<std::mutex> lock(ingressMutex);
                assemblyStopping = true;
            }
            revokeMedia(); ingressWake.notify_all();
            if (assemblyWorker.joinable()) assemblyWorker.join();
        }
        std::uint64_t telemetryTransportEpoch=0,telemetryStreamEpoch=0,cacheGeneration=0,nextNetworkAtUs=0;
        std::atomic<bool> telemetryRequested{false};
        telemetry::network::AssemblyTelemetryCache assemblyTelemetry;
        telemetry::network::SnapshotSlot<std::array<telemetry::protocol::MutexStats,3>> videoMutexTelemetry;
        telemetry::network::SnapshotSlot<telemetry::network::ControlSnapshot> controlTelemetry;
        telemetry::network::SnapshotSlot<telemetry::network::HistorySnapshot> historyTelemetry;
        std::shared_ptr<telemetry::network::EventJournal<telemetry::network::NetworkEvent>> networkEvents;
        void requestTelemetry() {
            std::lock_guard<std::mutex> lock(ingressMutex);
            if(assemblyStopping)return;
            telemetryRequested=true;ingressWake.notify_one();
        }
        void publishControlTelemetry(bool final=false) {
            telemetry::network::ControlSnapshot value{incoming.numericSnapshot(),incomingEvents.numericSnapshot(),serverPerformance.numericSnapshot(videoNow())};
            controlTelemetry.publish(telemetryTransportEpoch,0,value,fog::metrics::steadyNowUs(),final);
        }
        void publishHistoryTelemetry(bool final=false) {
            historyTelemetry.publish(telemetryTransportEpoch,0,transportHistory.numericSnapshot(),fog::metrics::steadyNowUs(),final);
        }
        telemetry::network::QueueSnapshot queueSnapshotLocked() const {
            telemetry::network::QueueSnapshot q;const auto now=videoNow();
            {
                std::lock_guard<std::mutex> audioLock(audioMutex);
                q.nackSendErrors=nackSendErrors;q.videoQueue=videoQueue.size();q.audioQueue=media.audioCount;
                q.queue=q.videoQueue+q.audioQueue;q.queueBytes=videoQueue.bytes()+media.audioBytes;q.videoAgeMs=videoQueue.age(now);q.audioAgeMs=media.age(Kind::Audio,now);
                q.videoEmptyObserved=q.videoQueue==0;q.audioEmptyObserved=q.audioQueue==0;q.videoAgeKnown=q.videoAgeMs>0;q.audioAgeKnown=q.audioQueue!=0;
                q.videoAgeHighMs=media.videoAgeHighMs;q.audioAgeHighMs=media.audioAgeHighMs;q.videoHigh=media.videoHigh;q.audioHigh=media.audioHigh;q.bytesHigh=media.bytesHigh;
                q.resyncs=queueResyncs;q.capacity=queueCapacity;q.age=queueAge;q.audioDrop=media.audioDropped;q.audioReject=media.audioRejected;q.generation=videoGeneration;q.pli=pliRequests;
                const auto& pressure=media.lastPressure;q.pressureReason=static_cast<unsigned>(pressure.reason);q.pressureCount=media.pressureCount;q.pressureAtMs=pressure.atMs;
                q.pressureVideoCount=pressure.videoCount;q.pressureAudioCount=pressure.audioCount;q.pressureVideoBytes=pressure.videoBytes;q.pressureAudioBytes=pressure.audioBytes;
                q.pressureVideoAgeMs=pressure.videoAgeMs;q.pressureAudioAgeMs=pressure.audioAgeMs;q.pressureIncomingBytes=pressure.incomingBytes;
                q.audioAgeDropped=media.audioAgeDropped;q.audioCapacityDropped=media.audioCapacityDropped;q.audioPriorityPopped=media.audioPriorityPopped;
                q.audioCadence=audioCadence.snapshot();
            }
            q.ingressDrops=ingressDrops.load(std::memory_order_relaxed);q.ingressWaitSamples=ingressWaitSamples;q.ingressWaitTotalMs=ingressWaitTotalMs;q.ingressWaitMaxMs=ingressWaitMaxMs;
            return q;
        }
        void publishAssemblyLocked(const TimedMutex::Snapshot& mutexSnapshot,bool force=false,bool final=false) {
            using namespace telemetry::network;
            RtpEvent event;
            while(video.pollNumericEvent(event)){
                NetworkEvent out;out.provider=Provider::Mts;out.kind=EventKind::Rtp;out.streamEpoch=telemetryStreamEpoch;
                out.transportEpoch=telemetryTransportEpoch;out.videoGeneration=videoGeneration;out.transportBindingKnown=true;out.videoBindingKnown=true;out.rtp=event;
                networkEvents->push(out);
            }
            const auto nowUs=fog::metrics::steadyNowUs();
            force=telemetryRequested.exchange(false)||force;
            if(cacheGeneration!=videoGeneration){assemblyTelemetry.reset(telemetryTransportEpoch,videoGeneration);videoMutexTelemetry.reset(telemetryTransportEpoch,videoGeneration);cacheGeneration=videoGeneration;force=true;}
            if(!force&&!final&&nowUs<nextNetworkAtUs)return;
            videoMutexTelemetry.publish(telemetryTransportEpoch,videoGeneration,telemetry::protocol::captureMutex(mutexSnapshot),nowUs,final);
            const auto q=queueSnapshotLocked();
            assemblyTelemetry.publishIfDue(telemetryTransportEpoch,videoGeneration,video,repair,nowUs,final,q);nextNetworkAtUs=nowUs+1000000;
        }
        void publishAssembly(bool force=false,bool final=false)noexcept {
            try{TimedMutex::Guard lock(videoMutex,TimedMutex::Scope::Diagnostics);publishAssemblyLocked(lock.snapshot(),force,final);}catch(...){}
        }
        void startAssembly() {
            assemblyTelemetry.reset(telemetryTransportEpoch,videoGeneration);videoMutexTelemetry.reset(telemetryTransportEpoch,videoGeneration);cacheGeneration=videoGeneration;
            controlTelemetry.reset(telemetryTransportEpoch,0);historyTelemetry.reset(telemetryTransportEpoch,0);
            publishControlTelemetry();publishHistoryTelemetry();
            assemblyWorker = std::thread([this] { runAssembly(); });
        }
        void wakeAssembly() {
            { std::lock_guard<std::mutex> lock(ingressMutex); ++assemblyWakeVersion; }
            ingressWake.notify_one();
        }
        void configureAssembly(VideoCodec codec) {
            {
                std::lock_guard<std::mutex> lock(ingressMutex);
                assemblyCodec = codec;
                ingress.clear(); ingressBytes = 0;
            }
            workerCodec.store(codec, std::memory_order_release);
            ingressWake.notify_one();
        }
        void runAssembly() noexcept {
            fog::diagnostics::observeThread("mts-assembly");
            struct FinalPublication {Shared& owner;~FinalPublication(){owner.publishAssembly(true);}} finalPublication{*this};
            publishAssembly(true);
            auto maintenanceAt = Clock::time_point::max();
            std::uint64_t observedWake = 0;
            for (;;) {
                VideoIngress input;
                bool hasInput = false, overflow = false;
                std::optional<VideoCodec> codec;
                {
                    std::unique_lock<std::mutex> lock(ingressMutex);
                    // No idle poll: only pending repair/assembly or queue-age
                    // deadlines use a timer. Packets, commands and dequeues wake
                    // the owner through a revision checked under this mutex.
                    ingressWake.wait_until(lock, maintenanceAt, [this, observedWake] {
                        return assemblyStopping || telemetryRequested.load() || ingressOverflow || assemblyCodec || !ingress.empty() ||
                            observedWake != assemblyWakeVersion;
                    });
                    if (assemblyStopping) return;
#ifdef FOG_STREAM_TESTING
                    if (assemblyPaused) {
                        ingressWake.wait(lock, [this] { return assemblyStopping || !assemblyPaused; });
                        if (assemblyStopping) return;
                        continue;
                    }
#endif
                    observedWake = assemblyWakeVersion;
                    overflow = std::exchange(ingressOverflow, false);
                    codec = std::exchange(assemblyCodec, std::nullopt);
                    if (ingressFeedback) videoFeedback = std::exchange(ingressFeedback, rtc::message_callback{});
                    if (!ingress.empty()) {
                        input = std::move(ingress.front()); ingress.pop_front();
                        ingressBytes -= input.bytes.size(); hasInput = true;
                    }
                }
                try {
                    if (codec) {
                        TimedMutex::Guard lock(videoMutex, TimedMutex::Scope::Callback);
                        if (!stopped) {
                            if (video.stats().received) ++videoGeneration;
                            video = H264RtpAssembler(*codec); videoFault = 0;
                            video.enableNack(repair.config().nack());
                            publishedVideoGeneration.store(videoGeneration, std::memory_order_release);
                            if (signals) signals->video.notify();
                        }
                    }
                    if (overflow) {
                        TimedMutex::Guard lock(videoMutex, TimedMutex::Scope::Callback);
                        if (!stopped) resyncQueue(true, videoNow());
                    }
                    if (hasInput) processVideoRtp(input.bytes.data(), input.bytes.size(), input.receivedMs);
                    else {
                        {
                            TimedMutex::Guard lock(videoMutex, TimedMutex::Scope::Client);
                            if (stopped) return;
                            video.tick(videoNow(), videoFrameBudget()); drainVideo();
                        }
                        sendNacks();
                    }
                    {
                        TimedMutex::Guard lock(videoMutex, TimedMutex::Scope::Client);
                        publishAssemblyLocked(lock.snapshot());
                        maintenanceAt = Clock::time_point::max();
                        if (!stopped && (video.pendingPackets() || video.assemblyBytes()))
                            maintenanceAt = Clock::now() + std::chrono::milliseconds(10);
                        std::uint64_t oldestGeneration = 0;
                        if (!stopped && videoQueue.oldestGeneration(oldestGeneration) && oldestGeneration == videoGeneration) {
                            const auto now = videoNow();
                            const auto age = videoQueue.age(now);
                            const auto untilExpiry = age > MediaQueue::MaxVideoAgeMs ? 0 : MediaQueue::MaxVideoAgeMs + 1 - age;
                            maintenanceAt = std::min(maintenanceAt, Clock::now() + std::chrono::milliseconds(untilExpiry));
                        }
                    }
                } catch (...) {
                    TimedMutex::Guard lock(mutex, TimedMutex::Scope::Callback);
                    callbackFailure = true; revokeMedia();
                    return;
                }
            }
        }
        std::atomic<bool> stopped{false};
        bool eventOverflow = false;
        bool callbackFailure = false;
        bool terminalMediaEvent = false;
        IncomingControl::Kind terminalControl = IncomingControl::Kind::Ignored;
        std::size_t eventBytes = 0, eventHigh = 0;
        std::uint64_t eventDropped = 0;
        std::deque<Event> events;
        MediaQueue media;
        AudioRtpCadence audioCadence; // audioMutex, transport lifetime; never reset on video resync.
        TransportHistory transportHistory;
        IncomingControl incoming;
        ServerPerformance serverPerformance;
        std::optional<ResolutionScale::Table> resolutions;
        bool resolutionsPending = false;
        std::uint64_t invalidResolutionTables = 0;
        IncomingControlDiagnostics incomingEvents;
        RumbleMailbox rumble;
        std::shared_ptr<AsyncControlSender> controlSender;
        std::shared_ptr<IceWorker> iceWorker;
        std::shared_ptr<RtcpFeedbackWorker> rtcpWorker;
        bool relay = false, turnAuthorized = false;
        std::uint64_t routeRevision = 0;
        Clock::time_point routeCapturedAt{};
        std::string pendingPong;
        std::atomic<std::uint64_t> qualityTimeOffset{0};
        std::uint64_t eventDelayHighMs = 0;
        Clock::time_point lastVideo{};
        H264RtpAssembler video;
        RtpRepair repair;
        rtc::message_callback videoFeedback;
        std::uint64_t nackSendErrors = 0;
        std::uint64_t ingressWaitSamples = 0, ingressWaitTotalMs = 0, ingressWaitMaxMs = 0;
        std::atomic<bool> videoTrack{false};
        std::uint64_t videoFault = 0, videoGeneration = 0, queueResyncs = 0, queueCapacity = 0, queueAge = 0;
        std::uint64_t pliRequests = 0;
        std::atomic<std::uint64_t> receivedVideoBytes{0}, lastRtpMs{0}, lastAudioMs{0};
        std::uint64_t lastControlMs = 0;
        std::uint64_t lastPeerMs = 0, lastIceMs = 0, lastGatheringMs = 0;
        rtc::PeerConnection::State peerState = rtc::PeerConnection::State::New;
        rtc::PeerConnection::IceState iceState = rtc::PeerConnection::IceState::New;
        rtc::PeerConnection::GatheringState gatheringState = rtc::PeerConnection::GatheringState::New;
        void reconcileFault() {
            if (videoFault == video.stats().faultSerial) return;
            videoFault = video.stats().faultSerial; ++videoGeneration;
            publishedVideoGeneration.store(videoGeneration, std::memory_order_release);
            if (signals) signals->video.notify();
        }
        void resyncQueue(bool capacity, std::uint64_t now) {
            ++queueResyncs;
            if (capacity) ++queueCapacity; else ++queueAge;
            video.requestResync(now); reconcileFault();
        }
        void recordVideoPressure(MediaQueue::PressureReason reason, std::uint64_t now, std::size_t size = 0) {
            std::lock_guard<std::mutex> audioLock(audioMutex);
            media.recordVideoPressure(reason, now, size);
            media.lastPressure.videoCount = videoQueue.size();
            media.lastPressure.videoBytes = videoQueue.bytes();
            media.lastPressure.videoAgeMs = videoQueue.age(now);
        }
        void expireMedia(std::uint64_t now) {
            {
                std::lock_guard<std::mutex> audioLock(audioMutex);
                media.expireAudio(now);
                media.videoAgeHighMs = std::max(media.videoAgeHighMs, videoQueue.age(now));
                media.bytesHigh = std::max(media.bytesHigh, videoQueue.bytes() + media.audioBytes);
            }
            std::uint64_t oldestGeneration = 0;
            if (videoQueue.oldestGeneration(oldestGeneration) && oldestGeneration == videoGeneration &&
                videoQueue.age(now) > MediaQueue::MaxVideoAgeMs) {
                recordVideoPressure(MediaQueue::PressureReason::Age, now); resyncQueue(false, now);
            }
        }
        std::size_t videoFrameBudget() const {
            const auto count = videoQueue.size();
            return count >= MediaQueue::MaxVideo ? 0 :
                std::min(std::size_t(2), MediaQueue::MaxVideo - count);
        }
        void drainVideo() {
            const auto now = videoNow();
            expireMedia(now); reconcileFault();
            H264RtpAssembler::Frame frame;
            while (video.poll(frame)) {
                Packet packet{Kind::Video, std::move(frame.data), frame.timestamp,
                              frame.firstSequence, frame.lastSequence, now, videoGeneration, frame.firstPacketMs};
                const auto size = packet.data.size();
                if (!videoQueue.push(std::move(packet))) {
                    recordVideoPressure(videoQueue.size() >= MediaQueue::MaxVideo ?
                        MediaQueue::PressureReason::Count : MediaQueue::PressureReason::Bytes, now, size);
                    resyncQueue(true, now); return;
                }
                { std::lock_guard<std::mutex> audioLock(audioMutex);
                  media.videoHigh = std::max(media.videoHigh, std::max(std::size_t(1), videoQueue.size()));
                  media.bytesHigh = std::max(media.bytesHigh, videoQueue.bytes() + media.audioBytes); }
                if (signals) signals->video.notify();
                lastVideo = Clock::now();
            }
        }
        bool normalizeVideo(rtc::binary& packet, bool control) {
            TimedMutex::Guard lock(videoMutex, TimedMutex::Scope::Callback);
            if (stopped) return false;
            if (control) return repair.acceptControl(packet);
            const auto now = videoNow();
            return repair.normalize(packet, [this, now](std::uint16_t seq) {
                return video.missingSequence(seq) || video.requestedSequence(seq, now);
            });
        }
        void sendNacks() {
            std::vector<std::byte> packet;
            {
                TimedMutex::Guard lock(videoMutex, TimedMutex::Scope::Client);
                if (stopped || !videoFeedback) return;
                packet = repair.nack(video.takeNacks(videoNow()));
            }
            // Weak transport callback, outside the shared mutex. A failed feedback
            // send does not terminate a rental; the bounded IDR fallback remains.
            transmitNack(std::move(packet), videoFeedback, TimedMutex::Scope::Client);
        }
        void transmitNack(std::vector<std::byte> packet, const rtc::message_callback& send, TimedMutex::Scope scope) {
            if (!packet.empty()) try { send(rtc::make_message(std::move(packet), rtc::Message::Control)); }
            catch (...) { TimedMutex::Guard lock(videoMutex, scope); ++nackSendErrors; }
        }
        void videoRtp(const std::byte* bytes, std::size_t size, std::uint64_t receivedMs, const rtc::message_callback& send) {
            if (!size || size > H264RtpAssembler::MaxPendingBytes) return;
            {
                std::lock_guard<std::mutex> lock(ingressMutex);
                if (assemblyStopping) return;
                if (!feedbackRegistered && send) {
                    ingressFeedback = send;
                    feedbackRegistered = true;
                }
                if (ingress.size() >= 1024 || size > 4 * 1024 * 1024 - ingressBytes) {
                    ingress.clear(); ingressBytes = 0; ingressOverflow = true; ++ingressDrops;
                } else {
                    ingress.push_back({{bytes, bytes + size}, receivedMs});
                    ingressBytes += size;
                }
            }
            ingressWake.notify_one();
        }
        void processVideoRtp(const std::byte* bytes, std::size_t size, std::uint64_t receivedMs) {
            std::vector<std::byte> feedback;
            {
                TimedMutex::Guard lock(videoMutex, TimedMutex::Scope::Callback);
                if (stopped) return;
                const auto now = videoNow();
                lastRtpMs = now; receivedVideoBytes += size;
                // The receive stamp precedes normalization/RTCP and ingress.
                // Reuse the existing processing clock read: this measures
                // wall-clock receive-to-assembly delay, not assembly CPU time.
                if (receivedMs && now >= receivedMs) {
                    const auto waitMs = now - receivedMs;
                    ++ingressWaitSamples; ingressWaitTotalMs += waitMs;
                    ingressWaitMaxMs = std::max(ingressWaitMaxMs, waitMs);
                }
                expireMedia(lastRtpMs);
                video.receive(bytes, size, lastRtpMs, videoFrameBudget(), receivedMs); drainVideo();
                feedback = repair.nack(video.takeNacks(lastRtpMs));
            }
            transmitNack(std::move(feedback), videoFeedback, TimedMutex::Scope::Callback);
        }
        std::string videoDiagnostics() const {
            TimedMutex::Guard videoLock(videoMutex,TimedMutex::Scope::Diagnostics);
            auto value=video.numericSnapshot();value.counters.eventOverflow+=networkEvents?networkEvents->lost():0;
            return telemetry::network::rtpText(value)+telemetry::network::repairText(repair.numericSnapshot())+telemetry::network::queueVideoText(queueSnapshotLocked());
        }
        std::string transportDiagnostics() const {
            std::lock_guard<std::mutex> telemetryLock(telemetryMutex);
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
                transportHistory.diagnostics() + incoming.diagnostics() + incomingEvents.diagnostics() +
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
                if (relay) revokeMedia();
            }
            if (e.type == EventType::WsClosed || e.type == EventType::Failure ||
                (e.type == EventType::PeerState && (e.peerState == rtc::PeerConnection::State::Disconnected ||
                    e.peerState == rtc::PeerConnection::State::Failed || e.peerState == rtc::PeerConnection::State::Closed)) ||
                (e.type == EventType::IceState && (e.iceState == rtc::PeerConnection::IceState::Disconnected ||
                    e.iceState == rtc::PeerConnection::IceState::Failed || e.iceState == rtc::PeerConnection::IceState::Closed))) revokeMedia();
            // Unlike live peer/ICE state, a queued close/failure has no other
            // sticky state field. Preserve it across a pump's batch-swap gap so
            // its final publication cannot reopen a revoked media gate.
            if (e.type == EventType::WsClosed || e.type == EventType::Failure) terminalMediaEvent = true;
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
            publishHistoryTelemetry();
            e.arrivedMs = now;
            const auto bytes = e.text.size() + e.mid.size();
            if (events.size() >= 128 || eventBytes + bytes > 1024 * 1024) { eventOverflow = true; ++eventDropped; revokeMedia(); return; }
            eventBytes += bytes; events.push_back(std::move(e));
            eventHigh = std::max(eventHigh, events.size());
        }
        void controlMessage(const std::string& text) {
            const auto message = IncomingControl::classify(text);
            const auto now = videoNow();
            std::shared_ptr<AsyncControlSender> sender;
            {
                TimedMutex::Guard lock(mutex, TimedMutex::Scope::Callback);
                if (stopped) return;
                lastControlMs = now;
                if (message.kind == IncomingControl::Kind::Close || message.kind == IncomingControl::Kind::Error ||
                    message.kind == IncomingControl::Kind::Invalid) {
                    terminalControl = message.kind;
                    revokeMedia();
                    rumble.clear();
                }
                if (message.kind == IncomingControl::Kind::Rumble && terminalControl == IncomingControl::Kind::Ignored) {
                    auto value = message.rumble;
                    value.receivedAtMs = now;
                    rumble.put(value);
                }
                sender = controlSender;
                if (!sender && message.kind == IncomingControl::Kind::Ping) pendingPong = message.pong;
            }
            {
                std::lock_guard<std::mutex> telemetryLock(telemetryMutex);
                if (stopped) return;
                incoming.receive(message, now + qualityTimeOffset.load(std::memory_order_relaxed));
                if (message.kind == IncomingControl::Kind::Stats) {
                    try { serverPerformance.receive(message.data, now); }
                    catch (...) {} // Optional telemetry cannot terminate playback.
                }
                if (message.kind == IncomingControl::Kind::Stats && message.data.contains("resolutions")) {
                    if (auto table = resolutionTable(message.data)) {
                        if (!resolutions || *resolutions != *table) {
                            resolutions = std::move(table);
                            resolutionsPending = true;
                        }
                    } else ++invalidResolutionTables;
                }
                try {
                    auto description = message.diagnostic;
                    description.classification=static_cast<unsigned>(message.kind);
                    incomingEvents.receive(description, now);
                }
                catch (...) {} // Losing a diagnostic sample must not end the stream.
                publishControlTelemetry();
            }
            if (message.kind == IncomingControl::Kind::Ping) {
                // Pong dispatch is independent of the application's frame pump,
                // but shares the same serialized, route-gated sender as input.
                if (!sender) return; // Channel publication will transfer the bounded latest pong.
                if (!sender->enqueue(message.pong, ControlQueue::Mode::Pong)) {
                    {
                        std::lock_guard<std::mutex> telemetryLock(telemetryMutex);
                        if (stopped) return;
                        incoming.pongFailed();publishControlTelemetry();
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
            if (stopped) return;
            const auto now = videoNow();
            if (kind != Kind::Audio) throw std::logic_error("Video packet bypassed assembly owner");
            lastAudioMs = now;
            {
                std::lock_guard<std::mutex> audioLock(audioMutex);
                if (stopped) return;
                audioCadence.received(timestamp,bytes.size(),now);
                media.push({kind, std::move(bytes), timestamp, 0, 0, now,
                    publishedVideoGeneration.load(std::memory_order_acquire)}, now);
                media.bytesHigh = std::max(media.bytesHigh, videoQueue.bytes() + media.audioBytes);
            }
            if (signals) signals->audio.notify();
        }
    };
    template<class F> static void callback(const std::weak_ptr<Shared>& weak, F&& fn) noexcept {
        auto shared = weak.lock();
        if (!shared) return;
        if (shared->stopped.load(std::memory_order_acquire)) return;
        try { fn(*shared); }
        catch (const std::exception&) {
            TimedMutex::Guard lock(shared->mutex, TimedMutex::Scope::Callback);
            shared->callbackFailure = true;
            shared->revokeMedia();
        } catch (...) {
            // Unknown callback failures must also reach the owner, without leaking SDP/credentials.
            TimedMutex::Guard lock(shared->mutex, TimedMutex::Scope::Callback);
            shared->callbackFailure = true;
            shared->revokeMedia();
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
    bool finalControlAvailable = false;
    std::uint64_t psUnavailableFrames = 0;
    std::uint64_t psInvalidNative = 0;
#ifdef FOG_STREAM_TESTING
    struct ControlSendFixture {
        std::mutex mutex;
        std::condition_variable condition;
        bool blocked = false, reject = false;
        void wait() {
            std::unique_lock<std::mutex> lock(mutex);
            condition.wait(lock, [this] { return !blocked; });
            if (reject) throw std::runtime_error("Fixture control send rejected");
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
    RecoveryPliSchedule recoveryPli;
    bool running = false, stompConnected = false, remoteSet = false, channelOpen = false;
    bool isReady = false, decoderWaiting = false, retryableFailure = false, closingForRetry = false;
    bool relay = false, routeSelected = false, routeCurrent = false, wasReady = false;
    bool playStation = false;
    bool retryInputReset = false;
    std::vector<ControlQueue::Message> retryInputReleases;
    Clock::time_point unprovenRouteAt{};
    SelectedRoute route;
    CandidateProofs remoteCandidateProofs;
    RouteSnapshotSchedule routeSnapshotSchedule;
    std::uint64_t telemetryStreamEpoch=0;
    telemetry::network::NetworkSnapshot finalNetwork;
    telemetry::protocol::ProtocolSnapshot finalProtocol;
    std::shared_ptr<telemetry::network::EventJournal<telemetry::network::NetworkEvent>> networkEvents=std::make_shared<telemetry::network::EventJournal<telemetry::network::NetworkEvent>>();
    std::uint64_t telemetryEpoch = 0;
    mutable std::uint64_t telemetrySequence = 0;
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
        if (relay && shared) shared->revokeMedia();
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
    void publishMediaGate() {
        if (!shared) return;
        TimedMutex::Guard lock(shared->mutex, TimedMutex::Scope::Client);
        const bool peerBad = shared->peerState == rtc::PeerConnection::State::Disconnected ||
            shared->peerState == rtc::PeerConnection::State::Failed || shared->peerState == rtc::PeerConnection::State::Closed;
        const bool iceBad = shared->iceState == rtc::PeerConnection::IceState::Disconnected ||
            shared->iceState == rtc::PeerConnection::IceState::Failed || shared->iceState == rtc::PeerConnection::IceState::Closed;
        const bool allowed = running && !closingForRetry && !shared->stopped && !shared->eventOverflow &&
            !shared->callbackFailure && !shared->terminalMediaEvent && shared->terminalControl == IncomingControl::Kind::Ignored &&
            !peerBad && !iceBad && routeAllowed();
        const auto deadline = !allowed ? 0 : !relay ? std::numeric_limits<std::uint64_t>::max() :
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                routeCapturedAt.time_since_epoch()).count()) + RouteSnapshotSchedule::IntervalMs * 2;
        const bool changed = shared->mediaAllowedUntilMs.exchange(deadline, std::memory_order_acq_rel) != deadline;
        if (changed && shared->signals) shared->signals->notifyAll();
    }
    rtc::Candidate selectedLocal, selectedRemote;
    unsigned routeSelections = 0, routeEventDrops = 0;
    std::deque<telemetry::network::NetworkEvent> routeEvents;
    telemetry::network::IceCounters finalIceCounters;
    telemetry::network::UdpPollCapture udpPoll;
    telemetry::network::Observation finalIceObservation;
    bool finalIceClockKnown=false;
    std::optional<std::chrono::milliseconds> sctpRtt;
    Clock::time_point rttCapturedAt{};
    std::shared_ptr<ControlDelivery> controlDelivery = std::make_shared<ControlDelivery>();
    telemetry::protocol::Reliability numericReliability{};
    std::string channelReliability = " reliabilityKnown=0";
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
        udpPoll.publish(value.udp,value.udpQueryBeforeUs,value.udpQueryAfterUs,[this](const auto& udp){
            telemetry::network::NetworkEvent event;event.kind=telemetry::network::EventKind::UdpPoll;
            event.transition.setUdp(udp);stampEvent(event);event.videoBindingKnown=false;event.videoGeneration=0;
            return networkEvents->tryPushReserved(event,16);
        });
        const auto numericIce=telemetry::network::captureIce(value.ice);
        iceSnapshotAvailable=numericIce.availability==telemetry::network::IceCounters::Availability::Available;
        if(iceSnapshotAvailable){
            finalIceCounters=numericIce;
            finalIceObservation.hasValue=true;
            finalIceObservation.epoch=telemetryEpoch;
            finalIceObservation.generation=completed->revision;
            finalIceObservation.revision=completed->serial;
            telemetry::network::updateIceSourceClock(numericIce.provenance,static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(value.iceCapturedAt.time_since_epoch()).count()),finalIceObservation.capturedAtUs,finalIceClockKnown);
            finalIceObservation.state=numericIce.provenance==telemetry::network::IceCounters::Provenance::FinalRetained?telemetry::network::State::FinalRetained:telemetry::network::State::Live;
        }else{
            // Retain the last successful numeric value and its original clock;
            // a failed read cannot refresh its age or turn it into measured zero.
            finalIceCounters.availability=numericIce.availability;
            finalIceObservation.state=numericIce.availability==telemetry::network::IceCounters::Availability::NotSupported?
                telemetry::network::State::NotSupported:telemetry::network::State::CaptureFailed;
        }
        finalDtlsDiagnostics = value.dtls;
        sctpRtt = value.rtt;
        rttCapturedAt = value.rttCapturedAt;
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
        telemetry::network::NetworkEvent event;event.kind=first?telemetry::network::EventKind::RouteSelected:telemetry::network::EventKind::RouteChanged;event.transition.setRoute(routeNumeric());stampEvent(event);routeEvents.push_back(event);
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
                value.udpQueryBeforeUs=fog::metrics::steadyNowUs();
                try { value.udp = transport->getIceUdpPollDiagnostics(); }
                catch (...) { value.udp.ice.availability=rtc::IceDiagnostics::Availability::ReadFailed; }
                value.udpQueryAfterUs=fog::metrics::steadyNowUs();
                value.ice=value.udp.ice;
                value.iceCapturedAt=Clock::now();
            }
            try { value.dtls = transport->getDtlsDiagnostics(); } catch (...) {}
            // SCTP round-trip estimate, not an application input acknowledgement.
            try { value.rtt = transport->rtt(); } catch (...) {}
            value.rttCapturedAt = Clock::now();
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
    std::string offeredLossProtection = "none";
    bool qualityPending = true, bitratePending = true;
    std::uint64_t observedVideoGeneration = 0;
    unsigned qualityRequests = 0;
    Clock::time_point qualityAttempt{}, bitrateAttempt{}, rateAt{};
    std::uint64_t lastRateBytes = 0;
    double receiveMbps = 0;
    quality::Telemetry serverStats;
    quality::Controller autoQuality;
    ResolutionScale resolutionScale;
    std::uint64_t invalidResolutionTables = 0;
    std::deque<telemetry::network::NetworkEvent> qualityEvents;
    void stampEvent(telemetry::network::NetworkEvent& e) const {e.provider=telemetry::network::Provider::Mts;e.streamEpoch=telemetryStreamEpoch;e.transportEpoch=telemetryEpoch;e.videoGeneration=shared?shared->publishedVideoGeneration.load(std::memory_order_acquire):finalNetwork.videoGeneration;e.transportBindingKnown=true;e.videoBindingKnown=true;}
    void qualityEvent(telemetry::network::FixedEvent value) {
        if(qualityEvents.size()>=32){qualityEvents.pop_front();}
        telemetry::network::NetworkEvent e;e.kind=telemetry::network::EventKind::Fixed;e.transition.setFixed(value);stampEvent(e);qualityEvents.push_back(e);
    }
    void publishTransitionEvents(){
        // Preserve bounded producer buckets and their FIFO. SDK/RTP events
        // already in the journal retain their admission sequence.
        while(networkEvents->size()<32){
            telemetry::network::NetworkEvent e;
            if(!routeEvents.empty()){e=routeEvents.front();routeEvents.pop_front();}
            else if(!qualityEvents.empty()){e=qualityEvents.front();qualityEvents.pop_front();}
            else{quality::NumericEvent q;if(!autoQuality.pollNumericEvent(q))break;e.kind=telemetry::network::EventKind::Quality;e.transition.setQuality(q);stampEvent(e);e.videoBindingKnown=false;e.videoGeneration=0;}
            networkEvents->push(e);
        }
    }
    std::uint64_t qualityTimeOffset = 0;
    bool rateKnown = false;
    std::uint64_t qualityNow() const { return videoNow() + qualityTimeOffset; }
    telemetry::network::QualitySnapshot qualityNumeric() const noexcept {
        telemetry::network::QualitySnapshot out;const auto now=qualityNow();
        out.negotiatedCodecKnown=shared&&shared->videoTrack.load();out.codecH265=selectedCodec==VideoCodec::H265;out.requestedH265=videoSettings.codec==VideoCodec::H265;
        out.capMbps=videoSettings.bitrateMbps;out.quality=videoSettings.quality;out.pliSeconds=videoSettings.keyframeSeconds;out.requestedFps=videoSettings.framesPerSecond;
        out.receiveMbps=receiveMbps;out.targetKnown=serverStats.targetMbps.known;out.targetMbps=out.targetKnown?serverStats.targetMbps.value:-1;
        out.profileKnown=serverStats.profile.known;out.profile=static_cast<int>(serverStats.profile.value);
        out.automaticKnown=serverStats.automatic.known;out.automatic=out.automaticKnown?int(serverStats.automatic.value):-1;
        out.fpsKnown=serverStats.fps.known;out.fps=out.fpsKnown?serverStats.fps.value:-1;
        out.codecKnown=serverStats.codec.known;out.serverCodec=out.codecKnown?(serverStats.codec.value=="H265"?2:1):0;
        out.statsKnown=serverStats.count!=0;out.statsCount=serverStats.count;
        out.targetAgeKnown=out.targetKnown&&now>=serverStats.targetMbps.at;out.presetAgeKnown=out.profileKnown&&now>=serverStats.profile.at;
        out.statsAgeKnown=out.statsKnown&&now>=serverStats.at;out.autoAgeKnown=out.automaticKnown&&now>=serverStats.automatic.at;
        out.fpsAgeKnown=out.fpsKnown&&now>=serverStats.fps.at;out.codecAgeKnown=out.codecKnown&&now>=serverStats.codec.at;
        out.targetAgeMs=serverStats.targetMbps.age(now);out.presetAgeMs=serverStats.profile.age(now);out.autoAgeMs=serverStats.automatic.age(now);
        out.fpsAgeMs=serverStats.fps.age(now);out.codecAgeMs=serverStats.codec.age(now);out.statsAgeMs=out.statsKnown?std::int64_t(now-serverStats.at):-1;
        out.qualityRequests=qualityRequests;out.qualityPending=qualityPending;out.automaticPolicy=autoQuality.numericSnapshot();
        out.scaleAttempts=resolutionScale.attempts();out.scaleQueued=resolutionScale.accepted();out.scaleRejected=resolutionScale.rejected();out.scaleInvalidTables=invalidResolutionTables;
        out.scaleTableRevision=resolutionScale.tableRevision();out.scaleTableEntries=resolutionScale.tableEntries();
        const auto native=resolutionScale.nativeSize();out.scaleNativeWidth=native[0];out.scaleNativeHeight=native[1];
        out.scalePreset=resolutionScale.preset()?static_cast<int>(*resolutionScale.preset()):-1;
        out.scaleLastPreset=resolutionScale.lastSubmittedPreset()?static_cast<int>(*resolutionScale.lastSubmittedPreset()):-1;
        out.scalePendingReason=static_cast<unsigned>(resolutionScale.pendingState());
        // This is the already normalized SDP codec family classification, not
        // diagnostic text. Export only its fixed allowlisted enumeration.
        out.offeredLossProtection=8;
        for(unsigned mask=0;mask<8;++mask)if(offeredLossProtection==telemetry::network::lossProtectionName(mask)){out.offeredLossProtection=mask;break;}
        return out;
    }
    telemetry::network::NetworkSnapshot networkSnapshot() const {
        using namespace telemetry::network;
        const auto nowUs=fog::metrics::steadyNowUs();
        NetworkSnapshot out=shared?NetworkSnapshot{}:finalNetwork;
        out.provider=Provider::Mts;out.streamEpoch=telemetryStreamEpoch;out.transportEpoch=telemetryEpoch;out.capturedAtSteadyUs=nowUs;
        if(shared){
            shared->requestTelemetry();
            out.videoGeneration=shared->publishedVideoGeneration.load(std::memory_order_acquire);
            const auto assembly=shared->assemblyTelemetry.read(nowUs);
            out.assembly=observation(assembly);
            if(assembly.hasValue&&assembly.generation==out.videoGeneration){out.mts=assembly.value;}
            else{out.assembly.hasValue=false;out.assembly.state=State::NotPublished;out.assembly.stale=true;}
            const auto control=shared->controlTelemetry.read(nowUs,1000000);out.control=observation(control);if(control.hasValue)out.mtsControl=control.value;
            const auto history=shared->historyTelemetry.read(nowUs,1000000);out.history=observation(history);if(history.hasValue)out.mtsHistory=history.value;
            out.mtsQuality=qualityNumeric();
        }
        // Cache published by the existing ICE worker; this capture never calls SDK.
        if(shared){
            out.mtsDtls=captureDtls(finalDtlsDiagnostics);
            out.mtsIce=finalIceCounters;out.mtsUdpPoll=udpPoll.summary();out.ice=finalIceObservation;out.iceSourceClockKnown=finalIceClockKnown;
            if(shared->stopped){
                out.ice.state=out.ice.hasValue?State::FinalRetained:State::ClosedWithoutFinal;
                if(out.ice.hasValue)out.mtsIce.provenance=IceCounters::Provenance::FinalRetained;
                if(out.mtsUdpPoll.hasValue)out.mtsUdpPoll.provenance=static_cast<unsigned>(rtc::IceDiagnostics::Provenance::FinalRetained);
            }
            out.dtls.epoch=telemetryEpoch;out.dtls.generation=routeRevision;out.dtls.revision=publishedSerial;
            out.dtls.hasValue=finalDtlsDiagnostics.available;
            out.dtls.state=out.dtls.hasValue?(shared->stopped?State::FinalRetained:State::Live):(shared->stopped?State::ClosedWithoutFinal:State::NotPublished);
            out.dtls.capturedAtUs=routeCapturedAt==Clock::time_point{}?0:static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(routeCapturedAt.time_since_epoch()).count());
        }
        ageObservation(out.dtls,nowUs,RouteSnapshotSchedule::IntervalMs*2000);
        out.detailedIceMigrated=true;
        ageObservation(out.ice,nowUs,RouteSnapshotSchedule::IntervalMs*2000);
        if(!out.iceSourceClockKnown){out.ice.ageKnown=false;out.ice.ageUs=0;out.ice.stale=true;}
        if(out.mtsIce.availability!=IceCounters::Availability::Available)out.ice.stale=true;
        if(rtcpWorker){const auto rtcp=rtcpWorker->numericSnapshot(nowUs);out.rtcp=observation(rtcp);if(rtcp.hasValue)out.mtsRtcp=rtcp.value;
            out.rtcpWorkerActive=rtcp.workerActive;out.rtcpWorkerActiveMs=rtcp.workerActiveMs;out.rtcpStopped=rtcp.stopRequested;}
        agePerformance(out.mtsControl.performance,videoNow());
        ageObservation(out.assembly,nowUs,2000000);ageObservation(out.control,nowUs,1000000);ageObservation(out.history,nowUs,1000000);ageObservation(out.rtcp,nowUs,1000000);
        out.eventMissed=networkEvents->lost();out.eventCursor=networkEvents->cursor();out.completedAtSteadyUs=fog::metrics::steadyNowUs();return out;
    }
    std::string qualityDiagnostics() const {return telemetry::network::qualityText(qualityNumeric());}
    void readQualityStats(const quality::Telemetry& data) {
        // Each valid optional field owns its timestamp. Empty/malformed extensions
        // cannot refresh an old profile/target and never reach logs as raw text.
        const auto before = serverStats.profile.value;
        serverStats = data;
        if (before != serverStats.profile.value)
            qualityEvent({telemetry::network::FixedEventKind::StatsProfile,0,static_cast<std::uint64_t>(serverStats.profile.value)});
    }
    void applyQuality() {
        if ((!qualityPending && !bitratePending) || !channelOpen || !routeAllowed()) return;
        const auto now = Clock::now();
        const auto due = [now](Clock::time_point attempt) {
            return attempt == Clock::time_point{} || now - attempt >= std::chrono::seconds(1);
        };
        // REMB and the quality command keep independent 1-s retry intervals:
        // a video-generation REMB repeat must not hasten an unsent command.
        // A pending REMB goes first; the command follows only once it was sent.
        const bool bitrateDue = bitratePending && due(bitrateAttempt);
        const bool qualityDue = !bitratePending && qualityPending && due(qualityAttempt);
        if (!bitrateDue && !qualityDue) return;
        // RTCP needs the real remote SSRC learned from video RTP first.
        { TimedMutex::Guard lock(shared->videoMutex, TimedMutex::Scope::Client);
          if (!shared->video.stats().received) return; }
        try {
            if (bitratePending) {
                bitrateAttempt = now;
                bool sent = false;
                for (const auto& track : tracks)
                    if (track->description().type() == "video" && track->isOpen())
                        sent |= track->requestBitrate(static_cast<unsigned>(videoSettings.bitrateMbps) * 1000000u);
                if (!sent) return;
                bitratePending = false;
            }
            if (!qualityPending || !due(qualityAttempt)) return;
            qualityAttempt = now;
            static constexpr const char* commands[]{"set_autoquality", "set_lowquality", "set_midquality", "set_highquality", "set_ultraquality"};
            if (!control(Json{{"id", commands[videoSettings.quality]}}.dump())) return;
            ++qualityRequests;
            qualityPending = false; // Sent/queued, not an acknowledgment by the encoder.
        } catch (const std::exception&) {
            // Keep retryable UI changes separate from terminal streaming errors.
        }
    }
    void applyResolution() {
        // Quality is enqueued first on the same ordered control channel. The
        // 720p probe preserves the chosen Auto/manual profile and never repeats
        // merely because later stats or quality feedback arrive.
        if (qualityPending || !channelOpen || !routeAllowed()) return;
        const auto preset = resolutionScale.pending();
        if (!preset) return;
        bool accepted = false;
        try { accepted = control(Json{{"id", "apply_scale"}, {"preset", *preset}, {"enabled", *preset != 0}}.dump()); }
        catch (...) {}
        resolutionScale.submitted(accepted);
        qualityEvent({telemetry::network::FixedEventKind::ScaleRequest,0,static_cast<std::uint64_t>(*preset),resolutionScale.attempts(),true,*preset!=0,accepted});
    }
    std::string finalVideoDiagnostics, finalTransportDiagnostics, finalQueueDiagnostics, finalGapDiagnostics;
    std::string finalMutexDiagnostics;
    std::deque<std::string> finalVideoEvents;
    IncomingControlDiagnostics finalControlDiscovery;
    const char* operation = "idle";
    unsigned offers = 0, candidatesReceived = 0, emptyCandidates = 0, hostnameCandidatesSkipped = 0, heartbeatsSent = 0;
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
    telemetry::protocol::Route routeNumeric() const {
        telemetry::protocol::Route r;
        r.relay=relay;r.selected=routeSelected;r.current=currentRoute();r.live=running;r.iceSnapshotAvailable=iceSnapshotAvailable;
        r.local=static_cast<unsigned>(route.local);r.remote=static_cast<unsigned>(route.remote);r.localProtocol=static_cast<unsigned>(route.localProtocol);r.remoteProtocol=static_cast<unsigned>(route.remoteProtocol);r.proof=static_cast<unsigned>(route.remoteProof);r.actual=route.isTurn()?2:route.isDirect()?1:0;
        r.workerPresent=bool(iceWorker);r.publishedRevision=routeRevision;r.serial=publishedSerial;
        r.ageMs=routeCapturedAt==Clock::time_point{}?-1:std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-routeCapturedAt).count();
        if(iceWorker){const auto st=iceWorker->state();r.revision=st.revision;r.active=st.active;r.activeMs=st.activeMs;r.pending=st.pending;r.requested=st.requested;r.completed=st.completed;r.discarded=st.discarded;r.stopping=st.stopping;}
        r.candidateProofs=remoteCandidateProofs.size();r.candidateProofDrops=remoteCandidateProofs.dropped();r.selections=routeSelections;r.eventDrops=routeEventDrops;
        return r;
    }
    telemetry::protocol::ProtocolSnapshot protocolSnapshot(bool eventLocked=false) const {
        using namespace telemetry::protocol;
        ProtocolSnapshot out=shared?ProtocolSnapshot{}:finalProtocol;
        out.provider=Provider::Mts;out.transportEpoch=telemetryEpoch;out.streamEpoch=telemetryStreamEpoch;
        out.capturedAtSteadyUs=fog::metrics::steadyNowUs();if(shared)out.sourceCapturedAtSteadyUs=out.capturedAtSteadyUs;out.state=running?State::Live:telemetryEpoch?State::FinalRetained:State::NotPublished;
        auto& m=out.mts;m.route=routeNumeric();
        if(std::string_view(operation)=="idle")m.operation=Operation::Op0;
        if(std::string_view(operation)=="init.ice-servers")m.operation=Operation::Op1;
        if(std::string_view(operation)=="init.peer-create")m.operation=Operation::Op2;
        if(std::string_view(operation)=="init.send-request")m.operation=Operation::Op3;
        if(std::string_view(operation)=="candidate.validate")m.operation=Operation::Op4;
        if(std::string_view(operation)=="candidate.apply")m.operation=Operation::Op5;
        if(std::string_view(operation)=="signal.envelope")m.operation=Operation::Op6;
        if(std::string_view(operation)=="offer.envelope")m.operation=Operation::Op7;
        if(std::string_view(operation)=="offer.base64")m.operation=Operation::Op8;
        if(std::string_view(operation)=="offer.json")m.operation=Operation::Op9;
        if(std::string_view(operation)=="offer.filter")m.operation=Operation::Op10;
        if(std::string_view(operation)=="offer.mids")m.operation=Operation::Op11;
        if(std::string_view(operation)=="offer.parse-sdp")m.operation=Operation::Op12;
        if(std::string_view(operation)=="offer.candidates")m.operation=Operation::Op13;
        if(std::string_view(operation)=="offer.set-remote")m.operation=Operation::Op14;
        if(std::string_view(operation)=="offer.create-answer")m.operation=Operation::Op15;
        if(std::string_view(operation)=="candidate.envelope")m.operation=Operation::Op16;
        if(std::string_view(operation)=="candidate.base64")m.operation=Operation::Op17;
        if(std::string_view(operation)=="candidate.json")m.operation=Operation::Op18;
        if(std::string_view(operation)=="stomp.parse")m.operation=Operation::Op19;
        if(std::string_view(operation)=="signal.json")m.operation=Operation::Op20;
        if(std::string_view(operation)=="answer.send")m.operation=Operation::Op21;
        if(std::string_view(operation)=="candidate.send")m.operation=Operation::Op22;
        if(std::string_view(operation)=="control.json")m.operation=Operation::Op23;
        m.offers=offers;
        m.candidates=candidatesReceived;
        m.emptyCandidates=emptyCandidates;
        m.hostnameCandidatesSkipped=hostnameCandidatesSkipped;
        m.hbOutMs=heartbeat.count();
        m.hbInMs=incomingHeartbeat.count();
        m.hbSent=heartbeatsSent;
        m.wsMessages=wsMessages;
        m.stompFrames=stompFrames;
        m.stompConnected=stompConnectedFrames;
        m.stompMessage=stompMessageFrames;
        m.stompError=stompErrorFrames;
        m.stompOther=stompOtherFrames;
        m.signalInit=signalInitMessages;
        m.wsAgeMs=std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-lastWs).count();
        m.subscribe={subscribeSends.attempts,subscribeSends.accepted,subscribeSends.buffered,subscribeSends.rejected};m.enabled={enabledSends.attempts,enabledSends.accepted,enabledSends.buffered,enabledSends.rejected};
        const auto sender=controlSender?controlSender->snapshot():finalControl;m.sender=captureSender(sender);m.input=captureInput(sender);m.input.playStation=playStation;m.input.unavailable=psUnavailableFrames;m.input.invalidNative=psInvalidNative;
        const auto d=controlDelivery->snapshot();m.delivery.known=d.known;m.delivery.bytes=d.currentBytes;m.delivery.maxBytes=d.maxBytes;m.delivery.sampledAtMs=d.sampledAt;m.delivery.blocked=d.blocked;m.delivery.blockedSinceMs=d.blockedSince;
        m.delivery.reliability=numericReliability;m.delivery.rttKnown=sctpRtt.has_value();m.delivery.rttMs=sctpRtt?sctpRtt->count():0;
        m.delivery.rttAgeMs=rttCapturedAt==Clock::time_point{}?-1:std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-rttCapturedAt).count();
        m.delivery.rttCurrent=iceWorker&&routeRevision==iceWorker->state().revision&&m.delivery.rttAgeMs>=0&&static_cast<std::uint64_t>(m.delivery.rttAgeMs)<RouteSnapshotSchedule::IntervalMs*2;
        if(shared){
            // Keep the established event -> telemetry lock order. No video lock
            // or SDK query occurs here; assembly publishes its own mutex copy.
            if(eventLocked)captureProtocolShared(out,nullptr);
            else {TimedMutex::Guard lock(shared->mutex,TimedMutex::Scope::Diagnostics);const auto completed=lock.snapshot();captureProtocolShared(out,&completed);}
        }
        out.completedAtSteadyUs=fog::metrics::steadyNowUs();return out;
    }
    void captureProtocolShared(telemetry::protocol::ProtocolSnapshot& out,const TimedMutex::Snapshot* completed)const {
        auto& m=out.mts;const auto now=videoNow();
        if(completed){m.eventMutex=telemetry::protocol::captureMutex(*completed);m.eventMutexKnown=true;}
        m.mutexEnabled=TimedMutex::DiagnosticsEnabled;out.videoGeneration=shared->publishedVideoGeneration.load(std::memory_order_acquire);
        const auto video=shared->videoMutexTelemetry.read(out.capturedAtSteadyUs,1000000);
        if(video.hasValue){m.videoMutex=video.value;m.videoMutexKnown=true;m.videoMutexCapturedAtUs=video.capturedAtUs;}
        m.peerState=static_cast<int>(shared->peerState);m.iceState=static_cast<int>(shared->iceState);m.gatheringState=static_cast<int>(shared->gatheringState);
        const std::array<std::uint64_t,6> stamps{{shared->lastPeerMs,shared->lastIceMs,shared->lastGatheringMs,shared->lastRtpMs.load(),shared->lastAudioMs.load(),shared->lastControlMs}};
        for(std::size_t i=0;i<stamps.size();++i)m.ages[i]=stamps[i]&&now>=stamps[i]?static_cast<std::int64_t>(now-stamps[i]):-1;
        m.eventQueue=shared->events.size();m.eventBytes=shared->eventBytes;m.eventHigh=shared->eventHigh;m.eventDrop=shared->eventDropped;m.eventOverflow=shared->eventOverflow;m.callbackFailure=shared->callbackFailure;m.eventDelayHighMs=shared->eventDelayHighMs;
        m.history=shared->transportHistory.numericSnapshot();
        {std::lock_guard<std::mutex> lock(shared->telemetryMutex);m.incoming=shared->incoming.numericSnapshot();m.schemas=shared->incomingEvents.numericSnapshot();}
    }
    std::string diagnostics() const {
        return routeDiagnostics() + " " + telemetry::network::iceText(finalIceCounters,finalIceObservation.hasValue) + " operation=" + operation + " offers=" + std::to_string(offers) +
            " candidates=" + std::to_string(candidatesReceived) + " emptyCandidates=" + std::to_string(emptyCandidates) +
            " hostnameCandidatesSkipped=" + std::to_string(hostnameCandidatesSkipped) +
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
        return telemetry::mtsControlText(telemetry::captureMtsControl(controlSender ? controlSender->snapshot() : finalControl));
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
        const auto out = controlSender->inspect();
        // A latched operation deadline is terminal even when the callback
        // subsequently returns a retryable transport exception before this pump.
        if (out.operationStalled) throw TransportError(fog::constants::error::Stream::ControlChannelStalled);
        if (out.failure == AsyncControlSender::Failure::Transport) throw TransportError(fog::constants::error::Stream::ControlSendFailed, true);
        if (out.failure == AsyncControlSender::Failure::Unexpected) throw TransportError(fog::constants::error::Stream::ControlSenderFailed);
        // Backpressure seals new work in the sender. Let an already-started
        // probe/send return before retry; a call exceeding its own deadline is
        // fatal above, preserving HTTP rental-stop before any unavoidable join.
        if (out.backpressure && !out.transportActive) throw TransportError("Control channel backpressure", true);
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
        control(Json{{"id", "connection_ready"}}.dump(), ControlQueue::Mode::Initialize);
        channelOpen = true;
        if (!isReady) state = "Waiting for video";
    }
    std::string neutralControl() const noexcept {
        try { if (channelOpen) return protocol::gamepad(Gamepad{}, playStation).dump(); } catch (...) {}
        return {};
    }
    void stop(bool notify) {
        if (shared) {
            TimedMutex::Guard lock(shared->mutex, TimedMutex::Scope::Client);
            shared->rumble.clear();
            shared->revokeMedia();
        }
        if (shared) shared->stopAssembly();
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
            finalControlAvailable = true;
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
            shared->publishAssembly(true,true);
            shared->publishHistoryTelemetry(true);
            shared->rumble.clear();
            // Diagnostics must never interrupt transport teardown, including
            // allocation failure while snapshotting counters or event strings.
            try {
                finalVideoDiagnostics = shared->videoDiagnostics() + qualityDiagnostics();
                { TimedMutex::Guard videoLock(shared->videoMutex, TimedMutex::Scope::Diagnostics);
                  finalGapDiagnostics = shared->video.gapDiagnostics(); }
                { std::lock_guard<std::mutex> audioLock(shared->audioMutex);
                  finalQueueDiagnostics = shared->media.pressureText(); }
                finalTransportDiagnostics = shared->transportDiagnostics();
                finalMutexDiagnostics = TimedMutex::diagnostics(lock.snapshot(), "event") + " | " +
                    TimedMutex::diagnostics(shared->videoMutex.snapshot(), "video");

            } catch (...) {}
            {std::lock_guard<std::mutex> telemetryLock(shared->telemetryMutex);shared->publishControlTelemetry(true);finalControlDiscovery=shared->incomingEvents;}
            finalProtocol=protocolSnapshot(true);
            // Assembly and callbacks are sealed. Retain the last completed
            // diagnostic scopes as well; regular UI capture only reads the
            // assembly publication and never takes this video mutex.
            finalProtocol.mts.videoMutex=telemetry::protocol::captureMutex(shared->videoMutex.snapshot());finalProtocol.mts.videoMutexKnown=true;finalProtocol.mts.videoMutexCapturedAtUs=fog::metrics::steadyNowUs();
            finalProtocol.mts.eventMutex=telemetry::protocol::captureMutex(lock.snapshot());finalProtocol.mts.eventMutexKnown=true;
            finalNetwork=networkSnapshot();
            abandonedEvents.swap(shared->events);
            { std::lock_guard<std::mutex> audioLock(shared->audioMutex);
              abandonedPackets.swap(shared->media.packets); }
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
            const auto sender = controlSender->inspect();
            if (mayRetry && (sender.operationStalled || sender.transportActive)) {
                reason = sender.operationStalled ? fog::constants::error::Stream::ControlChannelStalled :
                    fog::constants::error::Stream::ControlOperationBlockedDuringTransportFailure;
                mayRetry = false;
            }
        }
        if (failure.empty()) { failure = reason; retryableFailure = mayRetry; }
        // Publish failure without waiting for an in-flight synchronous RTC send.
        // The owner sees error() this frame and can dispatch rental termination
        // before close()/retry joins the worker. Keep transport ownership intact
        // until that explicit lifecycle boundary; callbacks cannot grow queues.
        // Media consumers can already exist before createPeer installs any RTC
        // worker. Preserve Shared/Impl until their owner has joined them too.
        if (shared || controlSender || iceWorker || rtcpWorker) {
            if (shared) {
                TimedMutex::Guard lock(shared->mutex, TimedMutex::Scope::Client);
                shared->stopped = true;
                shared->revokeMedia();
                shared->rumble.clear();
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
        thread_local const bool observed = [] { fog::diagnostics::observeThread("rtc-control"); return true; }();
        (void)observed;
                    callback(weak, [&msg](Shared& st) {
                        if (const auto text = std::get_if<std::string>(&msg))
                            st.controlMessage(*text);
                        else if (const auto bytes = std::get_if<rtc::binary>(&msg)) {
                            std::lock_guard<std::mutex> telemetryLock(st.telemetryMutex);
                            if (!st.stopped) {
                                try { st.incomingEvents.receive(IncomingControlDiagnostics::specialDescription(bytes->size(),true), videoNow()); st.publishControlTelemetry(); }
                                catch (...) {}
                            }
                        }
                    });
                });
                Event e(EventType::Channel); e.channel = dc;
                try {
                    const auto reliability = dc->reliability();
                    e.reliability={true,reliability.unordered,reliability.maxRetransmits.has_value(),reliability.maxPacketLifeTime.has_value(),reliability.maxRetransmits.value_or(0),static_cast<std::uint64_t>(reliability.maxPacketLifeTime?reliability.maxPacketLifeTime->count():0)};
                    e.text = " reliabilityKnown=1 unordered=" + std::to_string(reliability.unordered) +
                        " maxRetransmits=" + (reliability.maxRetransmits ? std::to_string(*reliability.maxRetransmits) : "unset") +
                        " maxLifetimeMs=" + (reliability.maxPacketLifeTime ? std::to_string(reliability.maxPacketLifeTime->count()) : "unset");
                } catch (...) {}
                s.event(std::move(e));
            });
        });
        peer->onTrack([weak](std::shared_ptr<rtc::Track> track) {
            callback(weak, [&track, &weak](Shared& s) {
                auto desc = track->description();
                if (desc.isRemoved()) return;
                desc.setDirection(rtc::Description::Direction::RecvOnly);
                track->setDescription(desc);
                auto receiver = std::make_shared<rtc::RtcpReceivingSession>();
                receiver->setDeferredReports(true);
                Kind kind;
                std::shared_ptr<rtc::MediaHandler> depacketizer;
                if (desc.type() == "video") {
                    kind = Kind::Video;
                    {
                        TimedMutex::Guard lock(s.videoMutex, TimedMutex::Scope::Callback);
                        if (s.stopped) return;
                        if (s.videoTrack) throw TransportError(fog::constants::error::Stream::DuplicateVideoTrack);
                        s.videoTrack = true;
                        s.repair = RtpRepair(protocol::videoRepairConfig(static_cast<std::string>(desc)));
                        s.video.enableNack(s.repair.config().nack());
                    }
                    depacketizer = std::make_shared<VideoRtpHandler>(receiver,
                        [weak](rtc::binary& packet, bool control) {
                            bool accepted = false;
                            callback(weak, [&packet, &accepted, control](Shared& st) { accepted = st.normalizeVideo(packet, control); });
                            return accepted;
                        },
                        [weak](const std::byte* data, std::size_t size, std::uint64_t receivedMs, const rtc::message_callback& send) {
                            callback(weak, [data, size, receivedMs, &send](Shared& st) { st.videoRtp(data, size, receivedMs, send); });
                        });
                } else if (desc.type() == "audio") {
                    kind = Kind::Audio; depacketizer = std::make_shared<rtc::OpusRtpDepacketizer>();
                } else return;
                // Video handler owns normalization/RTCP ordering and ingress time.
                std::shared_ptr<RtcpFeedbackWorker> feedback;
                {
                    TimedMutex::Guard lock(s.mutex, TimedMutex::Scope::Callback);
                    if (s.stopped) return;
                    feedback = s.rtcpWorker;
                }
                if (!feedback || !feedback->addSession(receiver))
                    throw TransportError(fog::constants::error::Stream::RTCPReceiverRegistrationFailed);
                if (kind == Kind::Audio) depacketizer->addToChain(receiver);
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
        signal({{"id", "init_webrtc"}, {"client_hash", clientHash}, {"limit_fps", videoSettings.framesPerSecond},
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
        // One hostname candidate must not end a paid session: skip it and keep
        // the numeric ones. If none can connect, the retryable connection
        // deadline still applies. Embedded offer candidates: offer.candidates.
        if (!detail::resolvableWithoutLookup(parsed)) { ++hostnameCandidatesSkipped; return; }
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
            const auto originalSdp = offer.at("sdp").get<std::string>();
            offeredLossProtection = protocol::offeredVideoLossProtection(originalSdp);
            const auto sdp = protocol::filterOffer(originalSdp, videoSettings.codec);
            selectedCodec = protocol::offeredVideoCodec(sdp);
            shared->configureAssembly(selectedCodec);
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
            // setRemoteDescription() applies embedded candidates through the
            // same hostname rejection as trickled ones; filter them first.
            operation = "offer.candidates";
            hostnameCandidatesSkipped += detail::dropUnresolvableCandidates(remote);
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
                if (!event.text.empty()) channelReliability = event.text;
                numericReliability=event.reliability;
                const auto delivery = controlDelivery;
                const std::weak_ptr<Shared> weak(shared);
                controlSender = std::make_shared<AsyncControlSender>(
                    [dc, weak, delivery] {
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
                        if (!dc->isOpen()) return false;
                        const auto buffered = dc->bufferedAmount();
                        delivery->sample(buffered, videoNow());
                        return buffered <= 64 * 1024;
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
                            std::lock_guard<std::mutex> telemetryLock(s.telemetryMutex);
                            if (!s.stopped) { if (sent) s.incoming.pongSent(); else s.incoming.pongFailed(); s.publishControlTelemetry(); }
                        });
                    }, protocol::gamepad(Gamepad{}, playStation).dump(), retryInputReleases, retryInputReset);
                const std::weak_ptr<AsyncControlSender> weakSender(controlSender);
                dc->setBufferedAmountLowThreshold(64 * 1024);
                dc->onBufferedAmountLow([weakSender] {
                    if (const auto sender = weakSender.lock()) sender->notifyWritable();
                });
                retryInputReleases.clear(); retryInputReset = false;
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
            qualityEvent({telemetry::network::FixedEventKind::Peer,event.arrivedMs,static_cast<std::uint64_t>(event.peerState)});
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
            qualityEvent({telemetry::network::FixedEventKind::Ice,event.arrivedMs,static_cast<std::uint64_t>(event.iceState)});
            break;
        case EventType::GatheringState:
            qualityEvent({telemetry::network::FixedEventKind::Gathering,event.arrivedMs,static_cast<std::uint64_t>(event.gatheringState)});
            break;
        case EventType::WsClosed:
            throw TransportError(fog::constants::error::Stream::SignalingTransportClosed, true);
        case EventType::Failure:
            throw TransportError(event.text.empty() ? fog::constants::error::Stream::TransportFailed : event.text.c_str(), true);
        }
    }
};

Client::Client() : impl_(std::make_unique<Impl>()), telemetryStreamEpoch_(telemetry::nextStreamEpoch()) {}
Client::Client(std::string caPath, std::string random5suffix)
    : impl_(std::make_unique<Impl>()), caPath_(std::move(caPath)), random5suffix_(std::move(random5suffix)), telemetryStreamEpoch_(telemetry::nextStreamEpoch()) {}
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
void Client::blockControlSendForTesting(bool blocked, bool reject) {
    const auto fixture = impl_->controlSendFixture;
    std::lock_guard<std::mutex> lock(fixture->mutex);
    fixture->blocked = blocked; fixture->reject = reject; fixture->condition.notify_all();
}
void Client::advanceControlForTesting(unsigned milliseconds) { if (impl_->controlSender) impl_->controlSender->advance(milliseconds); }
void Client::publishMediaAdmissionForTesting() { impl_->publishMediaGate(); }
void Client::pauseAssemblyForTesting(bool paused) {
    const auto shared = impl_->shared;
    if (!shared) return;
    { std::lock_guard<std::mutex> lock(shared->ingressMutex); shared->assemblyPaused = paused; }
    shared->ingressWake.notify_all();
}
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
        { TimedMutex::Guard videoLock(i.shared->videoMutex, TimedMutex::Scope::Client);
          if (i.shared->lastVideo != Clock::time_point{}) i.shared->lastVideo -= elapsed; }
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
    impl_->telemetryEpoch = ++telemetryEpoch_;
    impl_->telemetryStreamEpoch=telemetryStreamEpoch_;
    impl_->videoSettings = preferences;
    impl_->relay = relay;
    impl_->playStation = playStation;
    if (retryInputReset_ && retryInputHash_ == hash) {
        impl_->retryInputReset = true;
        impl_->retryInputReleases = retryInputReleases_;
    } else {
        retryInputReset_ = false; retryInputReleases_.clear(); retryInputHash_.clear();
    }
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
    i.shared->telemetryTransportEpoch=i.telemetryEpoch;i.shared->telemetryStreamEpoch=telemetryStreamEpoch_;i.shared->networkEvents=i.networkEvents;
    i.shared->signals = mediaSignals();
    i.started = i.lastWs = i.lastHeartbeat = i.lastPli = i.rateAt = Clock::now();
    i.running = true; i.state = "Opening signaling";
    i.autoQuality.configureFramesPerSecond(i.videoSettings.framesPerSecond);
    i.autoQuality.enable(i.videoSettings.quality == 0, i.qualityNow());
    try {
        i.shared->startAssembly();
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
        thread_local const bool observed = [] { fog::diagnostics::observeThread("rtc-signaling"); return true; }();
        (void)observed;
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
    struct PublishGate {
        Impl& impl;
        ~PublishGate() { impl.publishMediaGate(); }
    } publishGate{i};
    TransportStageScope pumpTiming{i.pumpTiming};
    bool eventOverflow, callbackFailure, needsPli, statsPending;
    quality::Telemetry stats;
    std::optional<ResolutionScale::Table> resolutions;
    Clock::time_point lastVideo;
    std::deque<Impl::Event> events;
    try {
        TransportStageScope timing{i.receiveTiming};
        TimedMutex::Guard lock(i.shared->mutex, TimedMutex::Scope::Client);
        eventOverflow = i.shared->eventOverflow; callbackFailure = i.shared->callbackFailure;
        { TimedMutex::Guard videoLock(i.shared->videoMutex, TimedMutex::Scope::Client);
          needsPli = i.shared->videoTrack && (i.shared->video.needsPli() || i.decoderWaiting);
          lastVideo = i.shared->lastVideo;
          i.shared->video.observeRoundTrip(i.sctpRtt ? static_cast<std::uint64_t>(std::max<std::int64_t>(0, i.sctpRtt->count())) : 0);
          if (i.observedVideoGeneration != i.shared->videoGeneration) {
              i.observedVideoGeneration = i.shared->videoGeneration;
              i.bitratePending = true; i.bitrateAttempt = {}; // The quality command keeps its own interval.
              i.qualityEvent({telemetry::network::FixedEventKind::VideoGeneration,0,i.observedVideoGeneration});
          } }
        const auto now = Clock::now();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - i.rateAt).count();
        if (ms >= 1000) {
            i.receiveMbps = static_cast<double>(i.shared->receivedVideoBytes - i.lastRateBytes) * 8.0 / (static_cast<double>(ms) * 1000.0);
            i.lastRateBytes = i.shared->receivedVideoBytes; i.rateAt = now;
            i.rateKnown = true;
        }
        { std::lock_guard<std::mutex> telemetryLock(i.shared->telemetryMutex);
          const auto priorControl=i.shared->incoming.numericSnapshot();
          statsPending = i.shared->incoming.takeStats(stats, i.qualityNow());
          if(priorControl.burst||priorControl.statsPending||priorControl.firstPendingKnown)i.shared->publishControlTelemetry();
          i.invalidResolutionTables = i.shared->invalidResolutionTables;
          if (i.shared->resolutionsPending) {
              resolutions = i.shared->resolutions;
              i.shared->resolutionsPending = false;
          } }
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
            if (resolutions && i.resolutionScale.observe(std::move(*resolutions))) {
                const auto preset = i.resolutionScale.preset();
                const auto native=i.resolutionScale.nativeSize();
                telemetry::network::FixedEvent tableEvent{telemetry::network::FixedEventKind::ScaleTable,0,static_cast<std::uint64_t>(preset.value_or(0)),i.resolutionScale.tableRevision(),preset.has_value()};
                tableEvent.nativeWidth=static_cast<unsigned>(native[0]);tableEvent.nativeHeight=static_cast<unsigned>(native[1]);
                tableEvent.tableEntries=static_cast<unsigned>(i.resolutionScale.tableEntries());i.qualityEvent(tableEvent);
            }
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
            i.applyResolution();
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
        // Manual quality paces keyframe requests within and across recovery
        // episodes; Auto keeps its fixed interval and restarts the schedule.
        if (i.videoSettings.quality == 0) i.recoveryPli.reset();
        else i.recoveryPli.update(needsPli, lastVideo != Clock::time_point{} && now - lastVideo < std::chrono::seconds(1), now);
        const auto recoveryPliInterval = i.videoSettings.quality == 0 ? std::chrono::milliseconds(250) : i.recoveryPli.interval();
        if (i.routeAllowed() && ((i.videoSettings.keyframeSeconds > 0 && now - i.lastPli >= std::chrono::seconds(i.videoSettings.keyframeSeconds)) ||
            (needsPli && now - i.lastPli >= recoveryPliInterval))) {
            TransportStageScope timing{i.pliTiming};
            bool requested = false;
            for (const auto& track : i.tracks)
                if (track->description().type() == "video" && track->isOpen()) {
                    track->requestKeyframe(); requested = true;
                }
            if (requested) {
                i.lastPli = now;
                if (needsPli && i.videoSettings.quality != 0) i.recoveryPli.requested();
                TimedMutex::Guard lock(i.shared->videoMutex, TimedMutex::Scope::Client);
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
    return pollVideo(packet) || pollAudio(packet);
}
bool Client::pollVideo(Packet& packet) {
    const auto shared = impl_->shared;
    if (!shared) return false;
    std::uint64_t generation = 0;
    while (shared->videoQueue.nextGeneration(generation)) {
        if (generation != shared->publishedVideoGeneration.load(std::memory_order_acquire)) {
            Packet discarded;
            shared->videoQueue.pop(discarded);
            shared->wakeAssembly();
            continue;
        }
        if (!shared->allowsMedia()) return false;
        if (!shared->videoQueue.pop(packet)) return false;
        shared->wakeAssembly();
        // A producer can invalidate the chain while the slot is moved. Never
        // expose its data as the new generation or after a revoked route.
        if (packet.videoGeneration == shared->publishedVideoGeneration.load(std::memory_order_acquire) &&
            shared->allowsMedia()) return true;
    }
    return false;
}
bool Client::mediaAdmissionAllowed() const {
    const auto shared = impl_->shared;
    return shared && shared->allowsMedia();
}
bool Client::pollAudio(Packet& packet) {
    const auto shared = impl_->shared;
    if (!shared || !shared->allowsMedia()) return false;
    std::lock_guard<std::mutex> lock(shared->audioMutex);
    shared->media.expireAudio(videoNow());
    return shared->allowsMedia() && shared->media.popAudio(packet);
}
bool Client::pollRumble(Rumble& rumble) {
    auto& i = *impl_;
    if (!i.shared || !i.running || i.closingForRetry) return false;
    TimedMutex::Guard lock(i.shared->mutex, TimedMutex::Scope::Client);
    if (i.shared->stopped || i.shared->terminalControl != IncomingControl::Kind::Ignored ||
        i.shared->eventOverflow || i.shared->callbackFailure) {
        i.shared->rumble.clear();
        return false;
    }
    return i.shared->rumble.take(rumble);
}
std::uint64_t Client::queuedVideoAgeMs() const {
    if (!impl_->shared) return 0;
    return impl_->shared->videoQueue.age(videoNow());
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
bool Client::configureController(ControllerProfile profile) {
    if (impl_->running || impl_->shared || !validControllerProfile(profile)) return false;
    impl_->playStation = profile == ControllerProfile::DualSense;
    return true;
}
bool Client::updateVideoSettings(const fog::video::Settings& settings) {
    if (!impl_->running || !impl_->channelOpen || !fog::video::valid(settings) ||
        settings.codec != impl_->videoSettings.codec || settings.framesPerSecond != impl_->videoSettings.framesPerSecond) return false;
    const bool remoteChanged = settings.bitrateMbps != impl_->videoSettings.bitrateMbps ||
                               settings.quality != impl_->videoSettings.quality;
    impl_->videoSettings = settings;
    if (remoteChanged) {
        impl_->qualityPending = true; impl_->bitratePending = true; impl_->qualityAttempt = impl_->bitrateAttempt = {};
        impl_->qualityEvent({telemetry::network::FixedEventKind::Settings,0,static_cast<std::uint64_t>(settings.quality),static_cast<std::uint64_t>(settings.bitrateMbps)});
        impl_->autoQuality.enable(false, impl_->qualityNow());
        impl_->autoQuality.enable(settings.quality == 0, impl_->qualityNow());
    }
    return true;
}
VideoCodec Client::selectedVideoCodec() const {
    const auto shared = impl_->shared;
    return shared ? shared->workerCodec.load(std::memory_order_acquire) : impl_->selectedCodec;
}
std::string Client::qualityStatus() const {
    const auto& i = *impl_;
    char rates[160];
    std::snprintf(rates, sizeof(rates), "%.1f Мбит/с", i.receiveMbps);
    std::string result = i.remoteSet ? (i.selectedCodec == VideoCodec::H265 ? "H.265" : "H.264") : "Кодек: подключение";
    result += " · Запрос: " + std::to_string(i.videoSettings.framesPerSecond) + " FPS";
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
    return impl_->shared->videoQueue.size();
}
void Client::observePlayback(const PlaybackObservation& observation) {
    auto& i = *impl_;
    if (!i.running || !i.shared || !i.channelOpen || !i.routeAllowed() || i.videoSettings.quality != 0 || i.qualityPending) return;
    quality::Sample sample;
    sample.frames = observation.presentedFrames; sample.decodedFrames = observation.decodedFrames;
    sample.supersededFrames = observation.supersededFrames;
    sample.workUs = observation.workUs; sample.errors = observation.errors;
    sample.decoderWorkUs = observation.decoderWorkUs; sample.videoGeneration = observation.videoGeneration;
    sample.workerVideo = observation.workerVideo;
    sample.audioUnderruns = observation.audioUnderruns; sample.audioResets = observation.audioResets;
    sample.recovering = observation.recovering; sample.width = observation.width; sample.height = observation.height;
    {
        TimedMutex::Guard lock(i.shared->mutex, TimedMutex::Scope::Client);
        { TimedMutex::Guard videoLock(i.shared->videoMutex, TimedMutex::Scope::Client);
          const auto& stats = i.shared->video.stats();
          sample.missing = stats.missing; sample.faults = stats.faultSerial; sample.recoveries = stats.resyncs; sample.pressure = stats.pressure;
          sample.receivedPackets = stats.received; sample.repairedPackets = i.shared->video.repairedPackets();
          sample.recovering |= i.shared->video.waiting(); }
        // Auto sees only work left AFTER this frame's bounded drain. A transient
        // predrain burst is not evidence of sustained decoder congestion.
        { std::lock_guard<std::mutex> audioLock(i.shared->audioMutex);
          sample.audioDropped = i.shared->media.audioDropped; }
        sample.queuedVideo = i.shared->videoQueue.size();
        sample.oldestVideoMs = i.shared->videoQueue.age(videoNow());
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
        if (i.controlSender) {
            const auto sender = i.controlSender->inspect();
            if (sender.operationStalled || sender.transportActive || sender.failure == AsyncControlSender::Failure::Unexpected) {
                const auto reason = sender.operationStalled ? fog::constants::error::Stream::ControlChannelStalled :
                    sender.transportActive ? fog::constants::error::Stream::ControlOperationBlockedDuringTransportFailure :
                    fog::constants::error::Stream::ControlSenderFailed;
                i.failure.clear(); i.fail(reason);
                return false;
            }
        }
        if (i.controlSender) retryInputReleases_ = i.controlSender->releasesForRetry();
        retryInputReset_ = true;
        if (!i.hash.empty()) retryInputHash_ = i.hash;
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
        i.shared->revokeMedia();
        i.shared->rumble.clear();
    }
    i.closingForRetry = true;
    pump(); // Frozen terminal evidence still wins over transport-only recovery.
    if (!i.running) {
        if (i.retryableFailure) {
            if (i.controlSender) retryInputReleases_ = i.controlSender->releasesForRetry();
            retryInputReset_ = true; retryInputHash_ = i.hash;
            i.stop(false);
        }
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
        if (sender.operationStalled || sender.transportActive) {
            i.fail(sender.operationStalled ? fog::constants::error::Stream::ControlChannelStalled : fog::constants::error::Stream::ControlOperationBlockedDuringTransportFailure);
            return false; // Owner dispatches rental stop BEFORE close/destructor.
        }
        if (sender.failure == AsyncControlSender::Failure::Unexpected) {
            i.fail(fog::constants::error::Stream::ControlSenderFailed); return false;
        }
    }
    if (i.controlSender) retryInputReleases_ = i.controlSender->releasesForRetry();
    retryInputReset_ = true; retryInputHash_ = i.hash;
    i.stop(false);
    return true;
}
std::string Client::diagnostics() const { return impl_->diagnostics(); }
telemetry::Snapshot Client::captureTelemetry(telemetry::Capture mask) const noexcept {
    using namespace telemetry;
    Snapshot out; out.provider=Provider::Mts;
    out.capturedAtSteadyUs=fog::metrics::steadyNowUs();
    out.input.scope=Scope::TransportRolling5s;
    try {
        const auto& i=*impl_;
        out.streamEpoch=telemetryStreamEpoch_;out.transportEpoch=i.telemetryEpoch;out.sequence=++i.telemetrySequence;
        if(i.shared) out.videoGeneration=i.shared->publishedVideoGeneration.load(std::memory_order_acquire);
        const Status status{i.telemetryEpoch ? (i.shared?Availability::Available:Availability::FinalRetained):Availability::NotStarted,
            i.telemetryEpoch?Reason::None:Reason::NotYetPublished,Scope::TransportLifetime};
        if(includes(mask,Capture::Transport)) {
            out.transport=status;
            out.mts.stages={captureTiming(i.pumpTiming),captureTiming(i.nackTiming),captureTiming(i.icePublishTiming),
                Timing{},Timing{},Timing{},Timing{},Timing{},captureTiming(i.receiveTiming),captureTiming(i.eventsTiming),
                captureTiming(i.qualityTiming),captureTiming(i.controlTiming),captureTiming(i.pliTiming),captureTiming(i.heartbeatTiming)};
            if(i.iceWorker){ const auto ice=i.iceWorker->state();out.mts.iceWorkerPresent=true;
                out.mts.stages[3]=captureTiming(ice.diagnostic);out.mts.stages[4]=captureTiming(ice.pair);
                out.mts.stages[5]=captureTiming(ice.schedule);out.mts.stages[6]=captureTiming(ice.wait);out.mts.stages[7]=captureTiming(ice.work); }
        }
        if(includes(mask,Capture::Control)){
            if(i.controlSender){out.control=status;out.mtsControl=captureMtsControl(i.controlSender->snapshot());}
            else if(i.finalControlAvailable){out.control={Availability::FinalRetained,Reason::None,Scope::TransportLifetime};out.mtsControl=captureMtsControl(i.finalControl);}
            else out.control={i.telemetryEpoch?Availability::TemporarilyUnavailable:Availability::NotStarted,
                i.shared?Reason::NotYetPublished:(i.telemetryEpoch?Reason::ClosedWithoutFinal:Reason::NotYetPublished),Scope::TransportLifetime};
        }
        if(includes(mask,Capture::Input)) {
            if(i.controlSender){out.input={Availability::Available,Reason::None,Scope::TransportRolling5s};
                out.inputLatency=capturePercentiles(i.controlSender->inputLatencySnapshot(out.capturedAtSteadyUs));}
            else out.input={i.telemetryEpoch?Availability::TemporarilyUnavailable:Availability::NotStarted,
                i.shared?Reason::NotYetPublished:(i.telemetryEpoch?Reason::ClosedWithoutFinal:Reason::NotYetPublished),Scope::TransportRolling5s};
        }
    } catch (...) {
        const Status failed{Availability::TemporarilyUnavailable,Reason::CaptureFailed,Scope::TransportLifetime};
        if(includes(mask,Capture::Transport))out.transport=failed;
        if(includes(mask,Capture::Control))out.control=failed;
        if(includes(mask,Capture::Input)){out.input=failed;out.input.scope=Scope::TransportRolling5s;}
    }
    out.captureCompletedAtSteadyUs=fog::metrics::steadyNowUs();return out;
}
telemetry::protocol::ProtocolSnapshot Client::captureProtocolTelemetry() const noexcept {
    try{auto out=impl_->protocolSnapshot();out.streamEpoch=telemetryStreamEpoch_;return out;}
    catch(...){telemetry::protocol::ProtocolSnapshot out;out.provider=telemetry::protocol::Provider::Mts;out.state=telemetry::protocol::State::CaptureFailed;out.streamEpoch=telemetryStreamEpoch_;out.transportEpoch=impl_->telemetryEpoch;return out;}
}
telemetry::network::NetworkSnapshot Client::captureNetworkTelemetry() const noexcept {
    try{auto out=impl_->networkSnapshot();out.streamEpoch=telemetryStreamEpoch_;return out;}
    catch(...){telemetry::network::NetworkSnapshot out;out.provider=telemetry::network::Provider::Mts;
        out.streamEpoch=telemetryStreamEpoch_;out.transportEpoch=impl_->telemetryEpoch;out.assembly.state=telemetry::network::State::CaptureFailed;return out;}
}
bool Client::peekNetworkEvent(telemetry::network::NetworkEvent& out) const noexcept {
    try{impl_->publishTransitionEvents();telemetry::network::EventJournal<telemetry::network::NetworkEvent>::Entry entry;
        if(!impl_->networkEvents->peek(entry))return false;
        out=entry.value;out.journalSequence=entry.sequence;out.journalMissed=entry.missed;return true;
    }catch(...){return false;}
}
std::size_t Client::peekNetworkEvents(telemetry::network::NetworkEvent* out,std::size_t capacity) const noexcept {
    if(!out||!capacity)return 0;
    try{
        impl_->publishTransitionEvents();
        using Journal=telemetry::network::EventJournal<telemetry::network::NetworkEvent>;
        std::array<Journal::Entry,Journal::Capacity> entries{};
        const auto count=impl_->networkEvents->peekBatch(entries.data(),std::min(capacity,entries.size()));
        for(std::size_t i=0;i<count;++i){out[i]=entries[i].value;out[i].journalSequence=entries[i].sequence;out[i].journalMissed=entries[i].missed;}
        return count;
    }catch(...){return 0;}
}
bool Client::retireNetworkEvent(std::uint64_t sequence,bool admitted) noexcept {
    try{return impl_->networkEvents->retire(sequence,admitted);}catch(...){return false;}
}
std::string Client::transportTimings() const { return telemetry::transportText(captureTelemetry(telemetry::Capture::Transport)); }
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
    return TimedMutex::diagnostics(snapshot, "event") + " | " +
        TimedMutex::diagnostics(impl_->shared->videoMutex.snapshot(), "video");
}
std::string Client::queueDiagnostics() const {
    if (!impl_->shared) return impl_->finalQueueDiagnostics;
    TimedMutex::Guard lock(impl_->shared->mutex, TimedMutex::Scope::Diagnostics);
    std::lock_guard<std::mutex> audioLock(impl_->shared->audioMutex);
    return impl_->shared->media.pressureText();
}
std::string Client::gapDiagnostics() const {
    if (!impl_->shared) return impl_->finalGapDiagnostics;
    TimedMutex::Guard lock(impl_->shared->videoMutex, TimedMutex::Scope::Diagnostics);
    return telemetry::network::gapText(impl_->shared->video.numericSnapshot());
}
std::string Client::videoDiagnostics() const {
    if (!impl_->shared) return impl_->finalVideoDiagnostics;
    TimedMutex::Guard lock(impl_->shared->mutex, TimedMutex::Scope::Diagnostics);
    return impl_->shared->videoDiagnostics() + impl_->qualityDiagnostics();
}
bool Client::pollUnmigratedNetworkEvent(std::string&) {return false;}
bool Client::pollVideoDiagnostic(std::string& event) {
    // Preserve the legacy aggregate's route -> quality -> AutoQuality -> RTP
    // priority. Typed consumers use peekNetworkEvents and its journal FIFO;
    // these two consumption modes must not be mixed on one client.
    enum class Pending { Route, Quality, Automatic, Journal };
    Pending pending=Pending::Journal;
    telemetry::network::NetworkEvent value;
    if(!impl_->routeEvents.empty()) {value=impl_->routeEvents.front();pending=Pending::Route;}
    else if(!impl_->qualityEvents.empty()) {value=impl_->qualityEvents.front();pending=Pending::Quality;}
    else {
        quality::NumericEvent automatic;
        if(impl_->autoQuality.pollNumericEvent(automatic)) {
            value.kind=telemetry::network::EventKind::Quality;value.transition.setQuality(automatic);
            impl_->stampEvent(value);value.videoBindingKnown=false;value.videoGeneration=0;
            pending=Pending::Automatic;
        } else {
            telemetry::network::EventJournal<telemetry::network::NetworkEvent>::Entry entry;
            if(!impl_->networkEvents->peek(entry))return false;
            value=entry.value;value.journalSequence=entry.sequence;value.journalMissed=entry.missed;
        }
    }
    auto text=telemetry::network::networkEventText(value,false);
    const std::string prefix="Video RTP event: ";if(text.rfind(prefix,0)==0)text.erase(0,prefix.size());
    event=std::move(text);
    if(pending==Pending::Route)impl_->routeEvents.pop_front();
    else if(pending==Pending::Quality)impl_->qualityEvents.pop_front();
    else if(pending==Pending::Journal)retireNetworkEvent(value.journalSequence,true);
    return true;
}
std::size_t Client::peekControlDiscoveryEvents(telemetry::network::ControlDiscoveryEvent* out,std::size_t capacity,bool final)const noexcept {
    try{if(impl_->shared){std::lock_guard<std::mutex> lock(impl_->shared->telemetryMutex);return impl_->shared->incomingEvents.peekBatch(out,capacity,videoNow(),final);}return impl_->finalControlDiscovery.peekBatch(out,capacity,videoNow(),final);}catch(...){return 0;}
}
bool Client::retireControlDiscoveryEvent(std::uint64_t sequence,bool admitted)noexcept {
    try{if(impl_->shared){std::lock_guard<std::mutex> lock(impl_->shared->telemetryMutex);const bool result=impl_->shared->incomingEvents.retire(sequence,admitted);impl_->shared->publishControlTelemetry();return result;}return impl_->finalControlDiscovery.retire(sequence,admitted);}catch(...){return false;}
}
bool Client::pollControlDiagnostic(std::string& event,bool final){
    telemetry::network::ControlDiscoveryEvent value;if(!peekControlDiscoveryEvents(&value,1,final))return false;event="provider=mts "+telemetry::network::controlDiscoveryText(value);return retireControlDiscoveryEvent(value.sequence,true);
}
bool Client::waitingForVideoKeyframe() const {
    if (!impl_->shared) return false;
    TimedMutex::Guard lock(impl_->shared->videoMutex, TimedMutex::Scope::Client);
    return impl_->shared->videoTrack && impl_->shared->video.waiting();
}
std::uint64_t Client::currentVideoGeneration() const {
    if (!impl_->shared) return 0;
    return impl_->shared->publishedVideoGeneration.load(std::memory_order_acquire);
}
void Client::setDecoderWaitingForKeyframe(bool waiting) { impl_->decoderWaiting = waiting; }
bool Client::requestVideoKeyframe() {
    if (!impl_->running || !impl_->routeAllowed()) return false;
    const auto now = Clock::now();
    // While the decoder waits for a keyframe this is a recovery request
    // (recoverVideoError): manual quality applies the same episode schedule as
    // pump() and counts the episode. Other requests keep the 250 ms guard.
    const bool recovery = impl_->decoderWaiting && impl_->videoSettings.quality != 0;
    if (recovery) impl_->recoveryPli.update(true, true, now);
    const auto interval = recovery ? impl_->recoveryPli.interval() : std::chrono::milliseconds(250);
    if (now - impl_->lastPli < interval) return false;
    bool requested = false;
    try {
        for (const auto& track : impl_->tracks) {
            if (track->description().type() == "video" && track->isOpen()) {
                track->requestKeyframe(); requested = true;
            }
        }
        if (requested) {
            impl_->lastPli = now;
            if (recovery) impl_->recoveryPli.requested();
            if (impl_->shared) {
                TimedMutex::Guard lock(impl_->shared->videoMutex, TimedMutex::Scope::Client);
                ++impl_->shared->pliRequests;
            }
        }
    } catch (...) { return false; }
    return requested;
}
std::string Client::serverPerformanceDiagnostics() const {
    if (!impl_->shared) return {};
    ServerPerformance snapshot;
    {
        std::lock_guard<std::mutex> telemetryLock(impl_->shared->telemetryMutex);
        snapshot = impl_->shared->serverPerformance;
    }
    return snapshot.diagnostics(videoNow());
}
std::string Client::controlDeliveryDiagnostics() const {
    const auto& i = *impl_;
    const auto age = i.rttCapturedAt == Clock::time_point{} ? -1LL :
        static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - i.rttCapturedAt).count());
    return i.controlDelivery->diagnostics(videoNow()) + i.channelReliability +
        " sctpRttMs=" + (i.sctpRtt ? std::to_string(i.sctpRtt->count()) : "unknown") +
        " rttSnapshotAgeMs=" + std::to_string(age) +
        " rttSnapshotCurrent=" + std::to_string(i.iceWorker && i.routeRevision == i.iceWorker->state().revision && age >= 0 && static_cast<std::uint64_t>(age) < RouteSnapshotSchedule::IntervalMs * 2) +
        " inputAck=unsupported";
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
std::string Client::inputLatencyDiagnostics() const {
    return telemetry::inputText(captureTelemetry(telemetry::Capture::Input));
}
bool Client::sendGamepad(const Gamepad& state) {
    if (!impl_->running || !impl_->channelOpen || !impl_->routeAllowed() || !impl_->controlSender) {
        if (impl_->playStation) ++impl_->psUnavailableFrames;
        return false;
    }
    try {
        if (impl_->controlSender->snapshot().stalled) { if (impl_->playStation) ++impl_->psUnavailableFrames; return false; }
        const auto payload = protocol::gamepad(state, impl_->playStation).dump();
        std::uint32_t buttons = 0;
        for (std::size_t i = 0; i < state.buttons.size(); ++i)
            if (state.buttons[i]) buttons |= std::uint32_t{1} << i;
        if (std::isfinite(state.leftTrigger) && state.leftTrigger > 0) buttons |= std::uint32_t{1} << 18;
        if (std::isfinite(state.rightTrigger) && state.rightTrigger > 0) buttons |= std::uint32_t{1} << 19;
        return impl_->controlSender->enqueueGamepad(payload, buttons, impl_->playStation, state.sampledAtUs);
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
    try {
        const auto data = protocol::remoteInput(input);
        const auto wire = data.dump();
        if constexpr (fog::input_options::typedPath) return sendControlData(wire, data);
        return sendControl(wire);
    }
    catch (const std::exception&) { return false; }
}
bool Client::sendControl(const std::string& json) {
    if (!impl_->running || !impl_->channelOpen || !impl_->routeAllowed() || json.size() > 64 * 1024) return false;
    const auto data = Json::parse(json, nullptr, false);
    return sendControlData(json, data);
}
bool Client::sendControlData(const std::string& json, const Json& data) {
    if (!impl_->running || !impl_->channelOpen || !impl_->routeAllowed() || json.size() > 64 * 1024) return false;
    if (!data.is_object()) return false;
    auto mode = ControlQueue::Mode::Ordered;
    std::string input, release;
    bool gamepad = false;
    std::uint32_t gamepadButtons = 0;
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
            const auto state = data.find("state");
            if (state == data.end() || !state->is_object()) return false;
            const auto buttons = state->find("buttons");
            if (buttons == state->end() || !buttons->is_array() || buttons->size() != 18) return false;
            for (std::size_t i = 0; i < buttons->size(); ++i) {
                const auto& value = (*buttons)[i];
                if (!value.is_number() || (value != 0 && value != 1)) return false;
                if (value == 1) gamepadButtons |= std::uint32_t{1} << i;
            }
            for (const auto& trigger : {std::pair<const char*, unsigned>{"leftTrigger", 18}, {"rightTrigger", 19}}) {
                const auto value = state->find(trigger.first);
                if (value == state->end() || !value->is_number()) return false;
                const auto amount = value->get<double>();
                if (!std::isfinite(amount) || amount < 0 || amount > 1) return false;
                if (amount > 0) gamepadButtons |= std::uint32_t{1} << trigger.second;
            }
            gamepad = true;
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
        if (gamepad) return impl_->controlSender && !impl_->controlSender->snapshot().stalled &&
            impl_->controlSender->enqueueGamepad(json, gamepadButtons, impl_->playStation);
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
