#pragma once
#include "features/streaming/application/player.hpp"
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace player_test {
using Player=fog::stream::Player;
using Media=fog::stream::PlayerMedia;
using namespace std::chrono_literals;
struct Trace {
    mutable std::mutex mutex;
    std::vector<std::string> values;
    void add(std::string value) { std::lock_guard<std::mutex> lock(mutex); values.push_back(std::move(value)); }
    std::vector<std::string> copy() const { std::lock_guard<std::mutex> lock(mutex); return values; }
};
struct Blocker {
    std::mutex mutex;
    std::condition_variable condition;
    bool entered=false,released=false;
    void block() {
        std::unique_lock<std::mutex> lock(mutex); entered=true; condition.notify_all();
        condition.wait(lock,[this]{return released;});
    }
    bool wait() {
        std::unique_lock<std::mutex> lock(mutex);
        return condition.wait_for(lock,2s,[this]{return entered;});
    }
    void release() { std::lock_guard<std::mutex> lock(mutex); released=true; condition.notify_all(); }
    ~Blocker() { release(); }
};
struct ClientState {
    Trace* trace=nullptr;
    std::atomic<unsigned> starts{0},closes{0},retries{0},destroyed{0};
    unsigned pumps=0,videoPolls=0,audioPolls=0,observations=0,plis=0,decoderClears=0,updates=0;
    bool startOk=true,retryOk=true,ready=true,disconnected=false,retryable=false,configOk=true;
    bool sourceAllowed=true,lossWaiting=false,decoderWaiting=false,relay=false;
    std::string error;
    std::optional<::stream::RecoveryState> recovery;
    ::stream::InputSuspension suspension;
    ::stream::StreamConnection connection;
    ::stream::ControllerProfile profile=::stream::ControllerProfile::Xbox;
    fog::video::Settings settings;
    ::stream::PlaybackObservation observed;
    ::stream::telemetry::Snapshot telemetry;
    ::stream::telemetry::network::NetworkSnapshot network;
    ::stream::telemetry::protocol::ProtocolSnapshot protocol;
    std::deque<::stream::telemetry::network::NetworkEvent> networkEvents;
    bool numericEvents=false,numericEventsAtStart=false,discoveryEnabled=false,statusAllocationFailure=false;
    std::deque<::stream::telemetry::network::ControlDiscoveryEvent> discoveryEvents;
    unsigned discoveryAdmitted=0,discoveryUncertain=0,legacyControlPolls=0;
    mutable unsigned legacyNetworkReads=0;
    unsigned eventAdmitted=0,eventUncertain=0,legacyEventPolls=0;
    unsigned finalEventDiscardBudget=32;
    ::stream::VideoCodec codec=::stream::VideoCodec::H264;
    std::uint64_t generation=0,queueAge=0;
    std::deque<::stream::Packet> packets,audio;
    std::function<void()> pumpAction,closeAction,retryAction;
};
class Client final : public ::stream::StreamingClient {
public:
    explicit Client(std::shared_ptr<ClientState> state) : s(std::move(state)) {}
    ~Client() override { ++s->destroyed; s->trace->add("client-destroy"); }
    bool start(const ::stream::StreamConnection& value) override {
        s->numericEventsAtStart=s->numericEvents;
        s->connection=value; ++s->starts; s->trace->add(value.resumeOnly?"resume-start":"initial-start"); return s->startOk;
    }
    bool configureVideo(const fog::video::Settings& value) override { s->settings=value; return s->configOk; }
    bool configureRelay(bool value) override { s->relay=value; return s->configOk; }
    bool configureController(::stream::ControllerProfile value) override { s->profile=value; return s->configOk; }
    bool updateVideoSettings(const fog::video::Settings& value) override { ++s->updates;s->settings=value; return s->configOk; }
    void pump() override { ++s->pumps; if (s->pumpAction) s->pumpAction(); }
    bool ready() const override { return s->ready; }
    std::string status() const override { if(s->statusAllocationFailure)throw std::bad_alloc();return "fixture"; }
    std::string error() const override { return s->error; }
    bool retryableError() const override { return s->retryable; }
    bool disconnected() const override { return s->disconnected; }
    ::stream::RecoveryState recoveryState() const override {
        return s->recovery ? *s->recovery : ::stream::StreamLifecycle::recoveryState();
    }
    bool disconnectForRetry() override {
        ++s->retries; s->trace->add("retry-close"); if(s->retryAction)s->retryAction();
        if (s->retryOk) { s->error.clear(); s->disconnected=false; s->recovery.reset(); }
        return s->retryOk;
    }
    void close() override { ++s->closes; s->trace->add("final-close"); if(s->closeAction)s->closeAction(); }
    ::stream::VideoCodec selectedVideoCodec() const override { return s->codec; }
    bool poll(::stream::Packet& packet) override {
        if(s->packets.empty())return false;
        packet=std::move(s->packets.front());s->packets.pop_front();return true;
    }
    bool pollVideo(::stream::Packet& packet) override { ++s->videoPolls; return poll(packet); }
    bool pollAudio(::stream::Packet& packet) override {
        ++s->audioPolls;if(s->audio.empty())return false;packet=std::move(s->audio.front());s->audio.pop_front();return true;
    }
    bool mediaAdmissionAllowed() const override { return s->sourceAllowed; }
    std::uint64_t currentVideoGeneration() const override { return s->generation; }
    std::uint64_t queuedVideoAgeMs() const override { return s->queueAge; }
    std::size_t queuedVideoPackets() const override { return s->packets.size(); }
    bool sendGamepad(const ::stream::Gamepad&) override { return true; }
    bool sendInput(const fog::remote::Input&) override { return true; }
    ::stream::InputSuspension inputSuspension() const override { return s->suspension; }
    void observePlayback(const ::stream::PlaybackObservation& value) override { s->observed=value; ++s->observations; }
    ::stream::telemetry::Snapshot captureTelemetry(::stream::telemetry::Capture) const noexcept override { return s->telemetry; }
    ::stream::telemetry::network::NetworkSnapshot captureNetworkTelemetry()const noexcept override{return s->network;}
    ::stream::telemetry::protocol::ProtocolSnapshot captureProtocolTelemetry()const noexcept override{return s->protocol;}
    void enableNetworkEvents(bool value)noexcept override{s->numericEvents=value;}
    bool peekNetworkEvent(::stream::telemetry::network::NetworkEvent& value)const noexcept override {
        if(s->networkEvents.empty())return false;
        value=s->networkEvents.front();return true;
    }
    std::size_t peekNetworkEvents(::stream::telemetry::network::NetworkEvent* out,std::size_t capacity)const noexcept override {
        const auto count=std::min(capacity,s->networkEvents.size());std::copy_n(s->networkEvents.begin(),count,out);return count;
    }
    bool retireNetworkEvent(std::uint64_t sequence,bool admitted)noexcept override{
        if(s->networkEvents.empty()||s->networkEvents.front().journalSequence!=sequence)return false;
        s->networkEvents.pop_front();s->network.eventCursor=sequence;if(admitted)++s->eventAdmitted;else {++s->eventUncertain;++s->network.eventMissed;}return true;
    }
    unsigned networkEventFinalDiscardBudget()const noexcept override{return s->finalEventDiscardBudget;}
    bool supportsControlDiscoveryTelemetry()const noexcept override{return s->discoveryEnabled;}
    std::size_t peekControlDiscoveryEvents(::stream::telemetry::network::ControlDiscoveryEvent* out,std::size_t capacity,bool)const noexcept override {
        const auto count=std::min(capacity,s->discoveryEvents.size());std::copy_n(s->discoveryEvents.begin(),count,out);return count;
    }
    bool retireControlDiscoveryEvent(std::uint64_t sequence,bool admitted)noexcept override {
        if(s->discoveryEvents.empty()||s->discoveryEvents.front().sequence!=sequence)return false;
        s->discoveryEvents.pop_front();if(admitted)++s->discoveryAdmitted;else ++s->discoveryUncertain;return true;
    }
    bool pollControlDiagnostic(std::string&,bool)override{++s->legacyControlPolls;return false;}
    bool waitingForVideoKeyframe() const override { return s->lossWaiting || s->decoderWaiting; }
    bool requestVideoKeyframe() override { ++s->plis; return true; }
    void setDecoderWaitingForKeyframe(bool value) override { s->decoderWaiting=value;if(!value)++s->decoderClears; }
    bool pollVideoDiagnostic(std::string&) override { ++s->legacyEventPolls;return false; }
    std::string qualityStatus() const override { return {}; }
    std::string diagnostics() const override { ++s->legacyNetworkReads;return {}; }
    std::string transportTimings() const override { return {}; }
    std::string videoDiagnostics() const override { ++s->legacyNetworkReads;return {}; }
    std::string gapDiagnostics() const override { ++s->legacyNetworkReads;return {}; }
    std::string queueDiagnostics() const override { ++s->legacyNetworkReads;return {}; }
    std::string mutexDiagnostics() const override { ++s->legacyNetworkReads;return {}; }
    std::string dtlsDiagnostics() const override { ++s->legacyNetworkReads;return {}; }
    std::string rtcpDiagnostics() const override { ++s->legacyNetworkReads;return {}; }
    std::string inputDiagnostics() const override { ++s->legacyNetworkReads;return {}; }
    std::shared_ptr<ClientState> s;
};
class TransportFactory final : public fog::stream::PlayerTransportFactory {
public:
    explicit TransportFactory(Trace& t) : trace(t) {}
    std::unique_ptr<::stream::StreamingClient> create(fog::ProviderId provider) override {
        ++calls;providers.push_back(provider);
        if(fail)throw std::runtime_error("fixture transport construction");
        auto state=next.empty()?std::make_shared<ClientState>():next.front();
        if(!next.empty())next.pop_front();
        state->trace=&trace;created.push_back(state);
        return std::make_unique<Client>(state);
    }
    Trace& trace;
    unsigned calls=0;
    bool fail=false;
    std::deque<std::shared_ptr<ClientState>> next;
    std::vector<std::shared_ptr<ClientState>> created;
    std::vector<fog::ProviderId> providers;
};
struct MediaState {
    Trace* trace=nullptr;
    Player::Time slot{};
    Media::Progress progress;
    fog::media::telemetry::Snapshot telemetry;
    mutable unsigned legacyTelemetryReads=0;
    Media::WorkerPolicy policy=Media::WorkerPolicy::FogHub;
    int fps=0;
    unsigned resets=0,feeds=0,audioFeeds=0,prepares=0,draws=0,presents=0,destroyed=0;
    std::uint64_t workerGeneration=0,drawn=0;
    bool pending=false,prepareAllowed=true,videoRecoverable=true,audioFailure=false,checkFailure=false;
    unsigned videoFailures=0,prepareFailures=0;
    std::exception_ptr workerFailure;
    std::function<void()> videoAction,prepareAction;
};
class FakeMedia final : public Media {
public:
    explicit FakeMedia(std::shared_ptr<MediaState> value) : s(std::move(value)) {}
    ~FakeMedia() override { assert(!s->progress.workers); ++s->destroyed; s->trace->add("media-destroy"); }
    void setVideoCodec(::stream::VideoCodec) override {}
    void setPresentationFrameRate(int fps) override { s->fps=fps; }
    void startWorkers(::stream::CompressedMediaSource& source,int fps,WorkerPolicy policy) override {
        s->progress.workers=true;s->policy=policy;s->fps=fps;source_=&source;s->trace->add("workers-start");
    }
    void stopWorkers() noexcept override { s->progress.workers=false;source_=nullptr;s->trace->add("media-worker-join"); }
    bool workersRunning() const override { return s->progress.workers; }
    bool takeWorkerError(WorkerError& error) override {
        if(!s->workerFailure)return false;
        error.error=s->workerFailure;s->workerFailure={};return true;
    }
    std::uint64_t workerVideoGeneration() const override { return s->workerGeneration; }
    bool waitingForVideoKeyframe() const override { return s->progress.waiting; }
    std::uint64_t presentedFrames() const override { return s->progress.presented; }
    void resetVideoDecoder() override { ++s->resets;s->pending=false;s->progress.waiting=true;s->trace->add("decoder-reset"); }
    void video(const ::stream::Packet&) override {
        ++s->feeds;if(s->videoAction)s->videoAction();
        if(s->videoFailures){--s->videoFailures;throw fog::stream::PlayerVideoError(-1,s->videoRecoverable,"fixture decode");}
        ++s->progress.decoded;s->pending=true;s->progress.waiting=false;
    }
    void audio(const ::stream::Packet&) override {
        ++s->audioFeeds;if(s->audioFailure)throw std::runtime_error("fixture audio exhausted");++s->progress.audio;
    }
    void checkAudioRecovery() const override { if(s->checkFailure)throw std::runtime_error("fixture audio deadline"); }
    bool prepare(Player::Time slot) override {
        ++s->prepares;s->slot=slot;if(s->prepareAction)s->prepareAction();
        if(s->prepareFailures){--s->prepareFailures;throw fog::stream::PlayerVideoError(-2,true,"fixture prepare");}
        if(!s->prepareAllowed||!s->pending||(source_&&!source_->mediaAdmissionAllowed()))return false;
        s->pending=false;++s->progress.uploaded;return true;
    }
    Progress progress() const override { return s->progress; }
    fog::media::telemetry::Snapshot captureTelemetry()const noexcept override{return s->telemetry;}
    bool draw() override { ++s->draws;s->drawn=s->progress.uploaded;return s->drawn!=0; }
    void present() override { ++s->presents;if(s->drawn>s->progress.presented)s->progress.presented=s->drawn; }
    std::string videoDiagnostics() const override { ++s->legacyTelemetryReads;return {}; }
    std::string audioDiagnostics() const override { ++s->legacyTelemetryReads;return {}; }
    std::string imageDiagnostics() const override { ++s->legacyTelemetryReads;return {}; }
    std::string latencyDiagnostics() const override { ++s->legacyTelemetryReads;return {}; }
    std::shared_ptr<MediaState> s;
    ::stream::CompressedMediaSource* source_=nullptr;
};
class MediaFactory final : public fog::stream::PlayerMediaFactory {
public:
    explicit MediaFactory(Trace& t):trace(t){}
    std::unique_ptr<Media> create() override {
        if(fail)throw std::runtime_error("fixture media construction");
        auto state=next.empty()?std::make_shared<MediaState>():next.front();
        if(!next.empty())next.pop_front();
        state->trace=&trace;created.push_back(state);return std::make_unique<FakeMedia>(state);
    }
    bool supportsH265() const override{return hevc;}
    Trace& trace;
    bool fail=false,hevc=true;
    std::deque<std::shared_ptr<MediaState>> next;
    std::vector<std::shared_ptr<MediaState>> created;
};
class Input final : public fog::stream::PlayerInputEffects {
public:
    explicit Input(Trace& value):trace(value){}
    void transportStarting(::stream::ControllerProfile,::stream::StreamInputSink&) override { trace.add("input-start"); }
    void transportPumped(::stream::StreamInputSink&) override { trace.add("input-release-retry"); }
    void videoSuspended(fog::stream::VideoSuspension reason) override { suspensions.push_back(reason);trace.add("input-suspend"); }
    void videoPrepared(bool value) override { waiting=value;trace.add("input-prepared"); }
    void beforeTransportClose(::stream::StreamInputSink*) noexcept override { trace.add("input-release"); }
    void transportClosed() noexcept override { trace.add("input-closed"); }
    Trace& trace;
    bool waiting=true;
    std::vector<fog::stream::VideoSuspension> suspensions;
};
struct Fixture {
    Player::Time now=Player::Time{}+1s;
    Trace trace;
    TransportFactory transports{trace};
    MediaFactory medias{trace};
    Input input{trace};
    Player owner;
    explicit Fixture(bool workers=false) : owner(transports,medias,input,{workers,true,true,true},[this]{return now;}) {}
    Player::Start request(fog::ProviderId provider=fog::ProviderId::Mts) const {
        Player::Start value;value.rental={provider,"fixture-rental"};return value;
    }
    auto client() const { return transports.created.back(); }
    auto media() const { return medias.created.back(); }
    Player::BeforeMedia before(Player::Admission admission={}) { owner.observe(now);return owner.beforeMedia(admission,now); }
    Player::MediaTurn turn(Player::Admission admission={}) { before(admission);return owner.media(now); }
    void frame(std::uint64_t generation=0) { ::stream::Packet packet;packet.videoGeneration=generation;client()->packets.push_back(std::move(packet)); }
    void joined() {
        const auto limit=std::chrono::steady_clock::now()+2s;
        while(owner.snapshot().teardown&&std::chrono::steady_clock::now()<limit){owner.observe(now);std::this_thread::yield();}
        assert(!owner.snapshot().teardown);
    }
};
}
