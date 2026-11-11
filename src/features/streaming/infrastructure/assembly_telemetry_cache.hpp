#pragma once
#include "features/streaming/domain/h264_rtp.hpp"
#include "features/streaming/domain/network_snapshot.hpp"
#include "features/streaming/domain/rtp_repair.hpp"
#include "features/streaming/infrastructure/network_snapshot_slot.hpp"
namespace stream::telemetry::network {
static_assert(std::is_trivially_copyable_v<AssemblySnapshot> && sizeof(AssemblySnapshot)<=8192);
// The assembly owner calls reset/publishIfDue while it already serializes both
// protocol owners. UI reads only the separate cache mutex and immutable values.
// No owner reference is retained. Integration into stream.cpp is a later checkpoint.
class AssemblyTelemetryCache {
public:
    static constexpr std::uint64_t IntervalUs=1000000;
    using Read=SnapshotSlot<AssemblySnapshot>::Read;
    void reset(std::uint64_t epoch,std::uint64_t generation) {
        epoch_=epoch;generation_=generation;nextAtUs_=0;slot_.reset(epoch,generation);
    }
    bool publishIfDue(std::uint64_t epoch,std::uint64_t generation,const H264RtpAssembler& rtp,const RtpRepair& repair,
        std::uint64_t nowUs,bool final=false,const QueueSnapshot& queue={}) {
        if(epoch!=epoch_||generation!=generation_||(!final&&nowUs<nextAtUs_))return false;
        const AssemblySnapshot value{rtp.numericSnapshot(),repair.numericSnapshot(),queue};
        if(!slot_.publish(epoch,generation,value,nowUs,final))return false;
        nextAtUs_=nowUs+IntervalUs;return true;
    }
    Read read(std::uint64_t nowUs)const{return slot_.read(nowUs,IntervalUs*2);}
    void closeWithoutFinal(std::uint64_t epoch,std::uint64_t generation){slot_.closeWithoutFinal(epoch,generation);}
private:
    SnapshotSlot<AssemblySnapshot> slot_;
    std::uint64_t epoch_=0,generation_=0,nextAtUs_=0; // assembly owner only
};
}
