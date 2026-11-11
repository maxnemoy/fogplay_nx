#pragma once
#include "features/streaming/domain/network_snapshot.hpp"
#include "features/streaming/infrastructure/network_snapshot_slot.hpp"
namespace stream::telemetry::network {
static_assert(static_cast<unsigned>(State::NotPublished)==static_cast<unsigned>(Publication::NotPublished));
static_assert(static_cast<unsigned>(State::Live)==static_cast<unsigned>(Publication::Live));
static_assert(static_cast<unsigned>(State::FinalRetained)==static_cast<unsigned>(Publication::FinalRetained));
static_assert(static_cast<unsigned>(State::CaptureFailed)==static_cast<unsigned>(Publication::CaptureFailed));
static_assert(static_cast<unsigned>(State::ClosedWithoutFinal)==static_cast<unsigned>(Publication::ClosedWithoutFinal));
template<class Read>Observation observation(const Read& r){
    return {static_cast<State>(r.publication),r.hasValue,r.ageKnown,r.stale,r.epoch,r.generation,r.revision,r.capturedAtUs,r.ageUs};
}
template<class Core>CoreCounters captureCore(const Core& value){CoreCounters result;
#define FOG_FH_COPY(name) result.name=value.name;
    FOG_NETWORK_FH_FIELDS(FOG_FH_COPY)
#undef FOG_FH_COPY
    return result;
}
inline void ageObservation(Observation& s,std::uint64_t nowUs,std::uint64_t staleAfterUs){
    s.ageKnown=s.hasValue&&nowUs>=s.capturedAtUs;s.ageUs=s.ageKnown?nowUs-s.capturedAtUs:0;
    s.stale=!s.ageKnown||s.ageUs>staleAfterUs||s.state==State::CaptureFailed;
}
inline void agePerformance(PerformanceSnapshot& s,std::uint64_t nowMs){
    s.capturedAtMs=nowMs;for(auto& f:s.fields){f.ageKnown=f.known&&nowMs>=f.sampledAtMs;f.ageMs=f.ageKnown?nowMs-f.sampledAtMs:0;f.stale=!f.ageKnown||f.ageMs>s.staleAfterMs;}
}
}

namespace stream::telemetry::network {
template<class Source> DtlsCounters captureDtls(const Source& source){
    DtlsCounters out;out.available=source.available;
#define FOG_DTLS_COPY(name) out.name=source.name;
    FOG_NETWORK_DTLS_FIELDS(FOG_DTLS_COPY)
#undef FOG_DTLS_COPY
    return out;
}
}
