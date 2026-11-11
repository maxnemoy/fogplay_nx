#pragma once
#include "features/streaming/domain/network_snapshot.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
namespace stream::telemetry {
enum class Provider : std::uint8_t { Unknown, Mts, FogHub };
enum class Availability : std::uint8_t { NotCollected, NotStarted, Available, FinalRetained, TemporarilyUnavailable, NotSupported };
enum class Reason : std::uint8_t { NotMigrated, None, NotYetPublished, ClosedWithoutFinal, UnsupportedProvider, CaptureFailed };
enum class Scope : std::uint8_t { TransportLifetime, TransportRolling5s, ClientRolling5s };
enum class ClockDomain : std::uint8_t { SteadyMicroseconds };
enum class Capture : unsigned { Transport = 1, Input = 2, Control = 4, All = 7 };
inline bool includes(Capture mask, Capture group) { return (unsigned(mask) & unsigned(group)) != 0; }
struct Status {
    Availability availability = Availability::NotCollected;
    Reason reason = Reason::NotMigrated;
    Scope scope = Scope::TransportLifetime;
};
inline bool available(Status value) { return value.availability == Availability::Available || value.availability == Availability::FinalRetained; }
struct Timing { std::uint64_t calls=0, totalUs=0, maxUs=0, over50ms=0; };
struct Percentiles { std::uint64_t samples=0, p50Us=0, p95Us=0, p99Us=0, overflowLifetime=0; };
struct MtsTransport {
    // pump,nack,icePublish,iceDiag,icePair,iceSchedule,iceWorkerIdle,iceWork,
    // receive,events,quality,controlFlush,pli,heartbeat. Durations are nested.
    std::array<Timing,14> stages{};
    bool iceWorkerPresent = false;
};
struct MtsControl {
    std::uint64_t pending=0,bytes=0,rejected=0,sent=0,pongCoalesced=0,ageMs=0,ageMaxMs=0;
    std::uint64_t operationMs=0,queueResets=0,inFlightMs=0;
    bool backpressure=false,operationStalled=false,transportActive=false,inFlight=false;
    Timing send{}, idle{};
};
struct FhTransport {
    std::array<Timing,4> adapter{}; // worker_gap,drain,copy,complete; approximate atomic copies
    std::array<Timing,6> receive{}; // cycle,wait,work,loop_gap,alloc,decrypt
    std::uint64_t emptyPolls=0,invalidTimes=0;
    std::uint64_t bufferRequestedBytes=0,bufferSelectedBytes=0,bufferActualBytes=0;
    std::uint64_t bufferSetFailures=0,bufferSetLastError=0,bufferQuerySucceeded=0,bufferQueryError=0;
    Status core{};
};
struct FhControl {
    Status command{}; // Command numeric sentinels are only meaningful when published.
    std::uint64_t accepted=0,rejected=0,ignored=0,attempt=0,failures=0,closedToResumeMs=0;
    // Sanitized command enum (0 none,1 launch,2 resume), never URLs/identities.
    int operation=0,curl=-1,xmlStatus=0,xmlResult=-1,ack=-1,result=-1;
    std::int64_t http=0;
    std::uint64_t elapsedMs=0,httpElapsedMs=0;
    bool dispatched=false,cancelled=false;
};
struct Snapshot {
    std::uint32_t schemaVersion=1;
    Provider provider=Provider::Unknown;
    ClockDomain clock=ClockDomain::SteadyMicroseconds;
    // transportEpoch is owner-generated, not an account/rental identifier.
    std::uint64_t streamEpoch=0,transportEpoch=0,videoGeneration=0,sequence=0,capturedAtSteadyUs=0,captureCompletedAtSteadyUs=0;
    Status transport{},input{},control{};
    // Network/repair/Media/loop groups remain on their legacy path in T1.
    Status unmigrated{};
    MtsTransport mts{}; MtsControl mtsControl{};
    FhTransport fh{}; FhControl fhControl{};
    Percentiles inputLatency{};
};
static_assert(std::is_trivially_copyable_v<Snapshot>);
static_assert(sizeof(Snapshot) <= 4096);
}
namespace stream {
class StreamTelemetrySource : public StreamNetworkTelemetrySource {
public:
    virtual ~StreamTelemetrySource() = default;
    // Application-owner capture. Copies only; no pump, SDK blocking queries,
    // policy mutation or formatting. Compatibility fakes are explicitly unmigrated.
    virtual telemetry::Snapshot captureTelemetry(telemetry::Capture) const noexcept { return {}; }
};
}
