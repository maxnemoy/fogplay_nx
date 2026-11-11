#pragma once
#include "features/streaming/application/player_media.hpp"
#include "features/streaming/application/input_gate_diagnostics.hpp"
#include "features/streaming/domain/control_discovery.hpp"
#include "features/streaming/application/playback_telemetry.hpp"
#include "features/streaming/domain/streaming_client.hpp"
#include "features/settings/domain/connection_settings.hpp"
#include <functional>
#include <optional>
#include <variant>

namespace fog::stream {
class PlayerTransportFactory {
public:
    virtual ~PlayerTransportFactory() = default;
    virtual std::unique_ptr<::stream::StreamingClient> create(ProviderId) = 0;
};
enum class VideoSuspension { Loss, Decoder, Generation, InputFreeze, Transport };
// Synchronous owner-turn hooks. Implementations handle only platform/input
// effects; none may reenter Player or run a media/recovery algorithm.
class PlayerInputEffects {
public:
    virtual ~PlayerInputEffects() = default;
    virtual void transportStarting(::stream::ControllerProfile, ::stream::StreamInputSink&) = 0;
    virtual void transportPumped(::stream::StreamInputSink&) = 0;
    virtual void videoSuspended(VideoSuspension) = 0;
    virtual void videoPrepared(bool waiting) = 0;
    virtual void beforeTransportClose(::stream::StreamInputSink*) noexcept = 0;
    virtual void transportClosed() noexcept = 0;
};
enum class PlayerDiagnosticPhase { Control, Protocol, Final, Closed, Periodic, Metrics, SlowCycle, DecodeError };
struct PlayerDiagnosticSnapshot {
    ::stream::telemetry::Snapshot transport;
    ::stream::telemetry::Playback playback;
    ::stream::telemetry::network::NetworkSnapshot network;
    ::stream::telemetry::protocol::ProtocolSnapshot protocol;
    ::fog::media::telemetry::Snapshot mediaTelemetry;
    PlayerMedia::Progress media;
    InputGateSnapshot inputGate;
    std::string status,error;
    std::vector<std::string> controls,videoEvents;
    std::uint64_t networkRetirementMissed=0,controlDiscoveryRetirementMissed=0;
    std::uint64_t videoWorkUs=0,pumpGapUs=0,previousPumpUs=0,pumpUs=0,transportUs=0,mediaBatchUs=0,prepareUs=0,resetUs=0;
    std::uint64_t videoBefore=0,videoAfter=0,drained=0,videoDrained=0;
    std::uint32_t rtpTimestamp=0;
    std::int64_t firstSequence=0,lastSequence=0;
    bool periodicDetails=false;
    bool stateChanged=false,recovering=false,controlReady=false,hasPacket=false,finalFlush=false,reaperJoined=false,retry=false,allowed=false;
};
class PlayerDiagnosticsObserver {
public:
    virtual ~PlayerDiagnosticsObserver() = default;
    // Immutable values captured by Player on its owner turn; never background
    // access to Player, Media, UI or a provider SDK.
    virtual void report(PlayerDiagnosticPhase,const PlayerDiagnosticSnapshot&) noexcept = 0;
    virtual void captureFailed(PlayerDiagnosticPhase) noexcept {}
    // At most 32 value-owned events per batch. False is uncertain admission,
    // never a request to retry a potentially accepted record.
    virtual bool networkEvents(const ::stream::telemetry::network::NetworkEvent*,std::size_t,bool) noexcept { return false; }
    virtual void networkEventsDiscarded(unsigned) noexcept {}
    virtual bool controlDiscoveryEvents(const ::stream::telemetry::network::ControlDiscoveryEvent*,std::size_t) noexcept {return false;}
    virtual void controlDiscoveryDiscarded(unsigned) noexcept {}
};
class Player {
public:
    using Time = std::chrono::steady_clock::time_point;
    using Clock = std::function<Time()>;
    struct Options {
        bool mtsMediaWorkers=true,inputFreezeGate=true,mtsCatchup=true,mtsCatchupAgeGuard=true;
    };
    struct Start {
        EntityId rental;
        video::Settings video;
        connection::Settings connection;
        ::stream::ControllerProfile controller=::stream::ControllerProfile::Xbox;
    };
    struct Admission {
        bool active=true,stopRequested=false,shuttingDown=false,serviceBusy=false,rentalPending=true,pairedPending=true;
    };
    enum class Purpose { Transport, Quality };
    enum class Verdict { Ready, Retry, Refused };
    struct ResumeVerdict { Purpose purpose; Verdict verdict; EntityId expected; };
    enum class StopReason { Admission, InitialStart, FatalTransport, Exhausted, RetryTerminal, Deadline,
        PreflightUnavailable, PreflightExpired, PreflightRefused, StartFatal, InspectionDispatch,
        QualityUnavailable, QualityRefused, QualityDisconnect, QualityCloseDeadline, QualityResume, MediaFailure };
    struct StopRequired { StopReason reason; };
    struct InspectionNeeded { Purpose purpose; EntityId expected; };
    using Effect = std::variant<StopRequired,InspectionNeeded>;
    enum class EventKind { TransportStarted, TransportClosed, TeardownScheduled, TeardownJoined, TransportBreak, RecoveryScheduled, RecoverySucceeded,
        FirstPrepared, Prepared, DecoderReset, Generation, Loss, InputFreeze, QualityResume };
    struct Event { EventKind kind; std::uint64_t value=0; bool flag=false; };
    enum class StartResult { Started, Recovering, Refused, StopRequired };
    enum class QualityResult { Saved, Applied, Inspection, Coalesced, Busy, Connecting, StopRequired };
    struct Snapshot {
        ProviderId provider=ProviderId::Mts;
        bool ownsRental=false,transport=false,media=false,teardown=false,retained=false,cancelled=false,shutdown=false;
        bool ready=false,disconnected=false,connected=false,recovering=false,connecting=false,videoRecovering=false;
        bool firstPrepared=false,lossWaiting=false,inputFreeze=false,inputAllowed=false,inspection=false,qualityClosing=false,relay=false,mtsMediaWorkers=false;
        unsigned attempts=0,decoderResets=0;
        std::uint64_t observedGeneration=0,lastVideoAgeMs=0,transportAgeMs=0,videoWorkUs=0,decodeErrors=0;
        int framesPerSecond=60;
        ::stream::ControllerProfile controller=::stream::ControllerProfile::Xbox;
        video::Settings activeVideo;
        ::stream::InputSuspension suspension;
        InputGateSnapshot inputGate;
        PlayerMedia::Progress progress;
    };
    struct BeforeMedia { bool media=false,inputAllowed=false; };
    struct MediaTurn { bool prepared=false,firstPrepared=false; };

    Player(PlayerTransportFactory&,PlayerMediaFactory&,PlayerInputEffects&,Clock = std::chrono::steady_clock::now);
    Player(PlayerTransportFactory&,PlayerMediaFactory&,PlayerInputEffects&,Options,Clock = std::chrono::steady_clock::now);
    ~Player();
    Player(const Player&)=delete;
    Player& operator=(const Player&)=delete;
    StartResult start(Start,Admission);
    // Pump terminal events and consume the close mailbox. Never restart here.
    void observe(Time refreshSlot);
    void pollTeardown();
    // Joined inspection is store-only; fresh post-overlay admission consumes it.
    void resumeVerdict(ResumeVerdict);
    void inspectionDispatchFailed();
    BeforeMedia beforeMedia(Admission,Time);
    MediaTurn media(Time refreshSlot);
    void presentationCompleted(Time);
    QualityResult changeVideo(video::Settings,Admission,Time);
    // Intent never joins/destroys a transport. Shell first dispatches rental
    // stop via its existing service slot, then authorizes blocking cleanup.
    void cancel() noexcept;
    void closeAfterStopDispatch() noexcept;
    void closeResolvedRental() noexcept;
    void shutdownAfterCancellation() noexcept;
    Snapshot snapshot() const;
    ::stream::telemetry::Playback timings(bool resetWindow=false);
    ::stream::telemetry::Playback timingsSnapshot() const;
    std::optional<Effect> takeEffect();
    std::optional<Event> takeEvent();
    std::exception_ptr takeFailure();
    void setDiagnosticsObserver(PlayerDiagnosticsObserver*) noexcept;
    std::string qualityStatus() const;
    std::string inputDiagnostics() const;
    // Frozen during observe; remains valid through this turn's late input.
    bool periodicDiagnosticsHaveRoom() const noexcept;
    bool requestVideoKeyframe();
    // Borrow only for the current owner turn. No view survives close/start.
    ::stream::StreamInputSink* inputSink() noexcept;
    const ::stream::StreamTelemetrySource* telemetrySource() const noexcept;
    MediaPresentation* presentation() noexcept;
    const PlayerMedia* mediaView() const noexcept;
    static const char* stopReasonName(StopReason) noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
