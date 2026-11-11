#ifndef RTC_ICE_DIAGNOSTICS_H
#define RTC_ICE_DIAGNOSTICS_H

#include <cstdint>
#include <array>
#include <type_traits>

namespace rtc {

// Fork extension. Fixed numeric copy; no endpoints, credentials or SDK handles.
// Counters are cumulative for one ICE transport, not reset by clearStats().
struct IceDiagnostics {
    enum class Availability : uint8_t {
        Available, NotStarted, NotSupported, ReadFailed, ClosedWithoutSample
    };
    enum class Provenance : uint8_t { None, Live, FinalRetained };
    enum class Backend : uint8_t { Unknown, LibJuice, LibNice };
    enum class FailureReason : uint8_t {
        None, UdpRecv, UdpPoll, Connection, ConsentExpired, StunTimeout,
        StunSend, StunResponse, ConnectivityTimeout, Unknown = 255
    };

    Availability availability = Availability::NotStarted;
    Provenance provenance = Provenance::None;
    Backend backend = Backend::Unknown;
    FailureReason failureReason = FailureReason::Unknown;
    // Numeric juice_state_t. Meaningful only when availability == Available.
    unsigned int state = 0;
    uint64_t udpBudgetYields = 0, udpDatagrams = 0;
    unsigned int udpMaxBatch = 0; // datagrams, maximum in one receive batch
    int recvErrno = 0; // signed backend error; preserve negative values
    unsigned int pollFlags = 0; // native poll bit mask
    int socketError = 0; // signed SO_ERROR; valid only if socketErrorAvailable
    bool socketErrorAvailable = false;
    uint64_t consentExpired = 0, stunTimeouts = 0, stunSendFailures = 0;
    uint64_t stunErrorResponses = 0;
    unsigned int stunErrorCode = 0; // STUN response code, not a duration
    int stunSendErrno = 0; // signed backend error; preserve negative values
    uint64_t connectivityTimeouts = 0;
};

static_assert(std::is_trivially_copyable<IceDiagnostics>::value);
static_assert(std::is_standard_layout<IceDiagnostics>::value);
static_assert(sizeof(IceDiagnostics) <= 128, "ICE snapshot must stay bounded");

// Cold-path poll evidence. Never place this history in a packet/frame record.
// Monotonic timestamps use the backend clock epoch, not the application epoch.
// Map sampleMonoUs to an application before/after query bracket. CPU time is
// unsupported: elapsed wall time does not establish CPU saturation.
struct IceUdpPollDiagnostics {
    struct Event {
        uint64_t sequence = 0, startMonoUs = 0, elapsedUs = 0;
        uint64_t bytes = 0, serviceGapUs = 0, readyDelayUs = 0;
        unsigned int datagrams = 0, flags = 0, exitReason = 0;
        uint64_t phaseRecvUs = 0, phaseCallbackUs = 0;
    };
    // flags: 1=large (>=256 datagrams), 2=slow (>=5000us), 4=budget yield,
    // 8=valid start+elapsed clock, 16=valid gap after a preceding budget yield,
    // 32=valid poll-ready-to-batch-start delay (excludes time sleeping in poll).
    // 64=phase sampled (every16th batch, starting withbatch0),
    // 128=all sampled phase clocks valid. Phase sections include preemption
    // and clock-boundary overhead; their sum need not equal batch elapsed.
    // Detail eligibility is >=50ms ORlarge/budget/error; flag2 remains>=5ms.
    // exitReason: 0=drained, 1=budget, 2=interrupted, 3=recv error, 4=callback error.
    IceDiagnostics ice;
    bool clockAvailable = false, sampleClockAvailable = false;
    bool lastClockAvailable = false, pendingYield = false;
    bool rcvbufChecked = false, rcvbufAvailable = false;
    int rcvbufBytes = 0, rcvbufErrno = 0;
    uint64_t sampleMonoUs = 0, clockInvalid = 0;
    uint64_t batches = 0, bytes = 0, timedBatches = 0;
    uint64_t elapsedTotalUs = 0, elapsedMaxUs = 0;
    uint64_t lastStartMonoUs = 0, lastEndMonoUs = 0;
    // Gap after a yield can include socket idle: packet 1001 is not proven.
    // pendingYield means only that the preceding batch hit the budget.
    uint64_t serviceGaps = 0, serviceGapMaxUs = 0;
    uint64_t readyDelays = 0, readyDelayMaxUs = 0;
    // Original >=5ms ORlarge/budget criterion, independent of filtered detail.
    uint64_t legacyEligibleBatches = 0;
    unsigned int phaseSampleEvery = 0; // 16 after the first poll batch
    uint64_t phaseSampledBatches = 0, phaseTimedBatches = 0, phaseInvalidBatches = 0;
    uint64_t phaseSampledDatagrams = 0, phaseTimedDatagrams = 0;
    // Receive calls include terminal EAGAIN/EINTR/error; sampled includes invalid.
    uint64_t phaseSampledReceiveCalls = 0, phaseTimedReceiveCalls = 0;
    // Totals/maxima cover complete valid sampled batches only. No extrapolation.
    uint64_t phaseRecvTotalUs = 0, phaseRecvBatchMaxUs = 0;
    uint64_t phaseCallbackTotalUs = 0, phaseCallbackBatchMaxUs = 0;
    uint64_t eventsTotal = 0, eventsOverwritten = 0; // filtered detail, not legacy criterion
    unsigned int eventCount = 0;
    std::array<Event, 16> events{}; // chronological, oldest first
};
static_assert(std::is_trivially_copyable<IceUdpPollDiagnostics>::value);
static_assert(sizeof(IceUdpPollDiagnostics) <= 2048, "cold UDP history must stay bounded");

} // namespace rtc
#endif
