#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace fog::media::telemetry {
enum class Availability : std::uint8_t { NotPublished, Available, FinalRetained };
enum class ClockDomain : std::uint8_t { SteadyMicroseconds };
enum class Scope : std::uint8_t { MediaLifetime, Rolling5s };
enum class Codec : std::uint8_t { None, H264, Hevc };
enum class ImageLayout : std::uint8_t { Iyuv, Nv12 };
enum class Capture : unsigned { Video=1, Audio=2, Image=4, Latency=8, All=15 };
constexpr Capture operator|(Capture a,Capture b) { return Capture(unsigned(a)|unsigned(b)); }
constexpr bool includes(Capture mask,Capture group) { return (unsigned(mask)&unsigned(group))!=0; }
struct Source {
    // Latest observed producer operation, not proof of a decoded/uploaded
    // frame. Its epoch/run may differ from the current admission while blocked.
    static constexpr std::uint64_t StaleAfterUs=2000000;
    std::uint64_t revision=0,publishedAtUs=0,epoch=0,sourceGeneration=0,loadGeneration=0,workerRun=0;
    std::uint64_t capturedAtUs=0,ageUs=0;
    Availability availability=Availability::NotPublished;
    Scope scope=Scope::MediaLifetime;
    bool sourceGenerationKnown=false,loadGenerationKnown=false,stale=false,clockValid=false,epochKnown=false;
};
inline void sourceAge(Source& source,std::uint64_t capturedAtUs) {
    source.clockValid=source.availability!=Availability::NotPublished && source.capturedAtUs<=source.publishedAtUs &&
        source.publishedAtUs<=capturedAtUs;
    source.ageUs=source.clockValid?capturedAtUs-source.capturedAtUs:0;
    source.stale=source.availability==Availability::Available&&source.clockValid&&source.ageUs>=Source::StaleAfterUs;
}
enum class StallKind : std::uint8_t { Stall, ObserverUnavailable };
enum class StallLane : std::uint8_t { Decoder, Presentation };
enum class StallStage : std::uint8_t { Idle, DecoderInit, DecodeSend, DecodeReceive, FrameRelease, DecoderRelease,
    Transfer, Convert, TextureUpload, TextureRelease, Draw, Present };
enum class StallError : std::uint8_t { None, ObserverThreadStart };
struct StallMetadata {
    int width = 0, height = 0, codec = 0, format = 0, stride = 0;
    std::uint64_t bytes = 0, nalTypes = 0, frame = 0;
};
struct StallEvent {
    StallKind kind = StallKind::Stall;
    StallLane lane = StallLane::Decoder;
    StallStage stage = StallStage::Idle;
    StallError error = StallError::None;
    std::uint64_t sequence = 0, monotonicMs = 0, elapsedMs = 0;
    StallMetadata metadata;
};
static_assert(std::is_trivially_copyable_v<StallEvent> && sizeof(StallEvent) <= 128);
enum class ErrorCategory : std::uint8_t { Unknown, VideoDecode, AudioPlayback, Logic, Allocation };
enum class StageAvailability : std::uint8_t { NotCollected, InstrumentationDisabled, Available };
struct StageDistributionValue {
    StageAvailability availability = StageAvailability::NotCollected;
    std::uint64_t capturedAtSteadyUs=0,samples=0,p50Us=0,p95Us=0,p99Us=0,overflowLifetime=0;
};
struct Timing { std::uint64_t calls=0,totalUs=0,maxUs=0; };
constexpr std::uint64_t average(Timing value) { return value.calls?value.totalUs/value.calls:0; }
struct Hardware {
    std::uint64_t events=0;
    std::uint32_t error=0,badBlocks=0,goodBlocks=0,cycles=0,headerError=0;
    std::int32_t waitCode=0;
    bool hasStatus=false,fresh=false;
};
// Captured only by the decoder owner. Numeric pixel IDs are resolved through
// the immutable process format-name table, never by querying a decoder later.
struct VideoOwner {
    Source source;
    Codec codec=Codec::None;
    std::int32_t codecId=0,pixelFormat=-1,framePixelFormat=-1,cpuPixelFormat=-1;
    std::int32_t colorspace=0,range=0,trc=0,primaries=0,threads=0,threadRequested=0,threadActive=0;
    std::array<Timing,4> stages{}; // send, receive, transfer, scale
    // Exact elapsed samples at SDK-call return, published by the decoder owner.
    std::array<StageDistributionValue,4> distributions{}; // send, receive, hardware transfer, pixel scale
    Hardware hardware;
    std::uint64_t pendingAgeUs=0,pendingSinceUs=0;
    bool pending=false,workerOwned=false,awaitingIdr=false;
};
enum class AudioClearReason : std::uint8_t { None, Latency, Recovery };
struct AudioOwner {
    // Existing SDL queued-byte observations exclude the hardware buffer. Empty
    // observations are not exact device underruns, A/V sync or playout delay.
    Source source;
    std::uint64_t packets=0,queuedBytes=0,maximumBytes=0,emptyObservations=0,prolongedEmptyObservations=0;
    std::uint64_t resets=0,discardedBytes=0,pcmDiscardedPackets=0,pcmDiscardedFrames=0,stalePackets=0;
    std::uint64_t overflowPackets=0,catchups=0,badPackets=0,decoderErrors=0,outputErrors=0;
    std::uint64_t recoveryAttempts=0,recoveryFailures=0,recoveryDiscardedPackets=0;
    std::uint64_t lastFloorBytes=0,maximumFloorBytes=0,floorWindows=0,elevatedFloorWindows=0;
    Timing queueWait;
    std::uint64_t queueObservedAtUs=0;
    // PCM sample frames (one frame contains all output channels), counted only
    // after successful decode / SDL submission. These are not device playback ACKs.
    std::uint64_t decodedPcmFrames=0,admittedPcmFrames=0,failedPcmFrames=0;
    std::uint64_t firstPcmAtUs=0,lastPcmAtUs=0,latencyClears=0,recoveryClears=0;
    std::uint64_t clearBeforeBytes=0,clearAfterBytes=0,clearAtUs=0;
    // SDL obtained stream format with allowed_changes=0; this is not the
    // physical output device's clock/rate or a measured consumption rate.
    std::uint32_t outputSampleRate=0,outputChannels=0,outputBytesPerFrame=0;
    AudioClearReason lastClearReason=AudioClearReason::None;
    bool recoveryPending=false,boundedSync=false,boundedWorker=false,queueKnown=false;
};
struct Queue {
    std::uint64_t depth=0,limit=1,readyEmpty=0,cadenceBlocked=0,rtpBlocked=0,capacityDrop=0,staleDrop=0;
    std::uint64_t lateCoalesce=0,taken=0,resets=0;
    std::int32_t framesPerSecond=30;
    bool displayPaced=false;
};
struct Presentation {
    std::uint64_t decoded=0,uploaded=0,presented=0,superseded=0;
    Timing upload,videoQueueWait,workerDecode,workerConvert;
    StageDistributionValue uploadDistribution; // UI owner, SDL update return
    Queue queue;
    // Existing Media atomics only; source generation is the worker's observed
    // packet generation, never synthesized as an uploaded-frame generation.
    std::uint64_t currentEpoch=0,observedSourceGeneration=0,currentLoadGeneration=0,workerRun=0;
    std::int32_t configuredFps=0;
    // Only a frozen stop handoff sets this origin tag. Workers can truthfully be
    // inactive while the retained configuration and counters describe that run.
    bool retainedWorkerHistory=false;
    bool workers=false,waitingForKeyframe=false,resetRequested=false,resetInProgress=false;
};
struct Image {
    // These are local texture/upload/draw/present counters, not source packet
    // generation identifiers or evidence of physical display scanout.
    std::uint64_t samples=0,blackFrames=0,blackRuns=0,blackStreak=0,blackStreakMax=0,unchangedStreak=0,unchangedStreakMax=0;
    std::uint64_t uploadedGeneration=0,drawnGeneration=0,presentedGeneration=0,draws=0,emptyDraws=0;
    std::uint64_t createErrors=0,uploadErrors=0,clearErrors=0,copyErrors=0,presentedBlackFrames=0,nv12Fallbacks=0;
    std::int64_t uploadAgeMs=-1,drawAgeMs=-1,newPresentAgeMs=-1;
    std::uint32_t points=0,yMin=255,yMax=0,yMean=0,darkPoints=0,chromaDeviation=0;
    std::int32_t width=0,height=0,textureCount=1,readyDepth=3;
    Timing sample;
    ImageLayout layout=ImageLayout::Iyuv;
    bool nearBlack=false,texture=false,presentedBlack=false,nv12=false,nv12Available=false,cpuPool=false;
};
struct Percentiles { std::uint64_t samples=0,p50Us=0,p95Us=0,p99Us=0,overflowLifetime=0; };
struct Latency {
    std::array<Percentiles,3> stages{}; // first packet to SDL return, assembly wait, completed-new-present gap
    std::uint64_t lateThresholdUs=0,lateCount=0,latePpm=0,unavailableAge=0,unavailableWait=0,originMisses=0;
    Scope scope=Scope::Rolling5s;
};
struct Snapshot {
    std::uint32_t schemaVersion=1;
    Capture captured=Capture::All;
    ClockDomain clock=ClockDomain::SteadyMicroseconds;
    std::uint64_t capturedAtUs=0,captureCompletedAtUs=0,mediaInstance=0;
    VideoOwner video;
    AudioOwner audio;
    Presentation presentation;
    Image image;
    Latency latency;
};
static_assert(std::is_trivially_copyable_v<Snapshot>);
static_assert(sizeof(Snapshot)<=4096,"Split media groups instead of growing the bounded handoff");
// The formatter receives an owned value. This record has no Media, SDL,
// decoder, packet, pixel-buffer, callback or worker lifetime reference.
enum class Line : std::uint8_t { Video,Audio,Image,Latency,Stages };
struct Record {
    static constexpr std::size_t MaximumRenderedBytes=8192;
    const Snapshot snapshot;
    const Line line;
    explicit Record(Snapshot value,Line requested=Line::Video):snapshot(value),line(requested){}
};
static_assert(std::is_trivially_copyable_v<Record> && sizeof(Record)<=4096);
inline constexpr std::size_t RecordQueueCharge=sizeof(Record)+Record::MaximumRenderedBytes;
}
