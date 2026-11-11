#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#define main repairFixtureMain
#include "rtp_repair_tests.cpp"
#undef main
#pragma GCC diagnostic pop
#include "features/streaming/infrastructure/incoming_control.hpp"
#include "features/streaming/infrastructure/server_performance.hpp"
#include "features/streaming/infrastructure/transport_history.hpp"
#include "features/streaming/infrastructure/network_snapshot_slot.hpp"
#include "features/streaming/infrastructure/assembly_telemetry_cache.hpp"
#include <atomic>
#include <thread>
#include <type_traits>
using namespace stream::telemetry::network;
static void numericRtp(){
 A empty;const auto unknown=empty.numericSnapshot();assert(!unknown.initialized&&!unknown.lastTimestampKnown&&!unknown.lastExpiryKnown&&!unknown.roundTripKnown);
 A a;fragmentGap(a);auto gap=a.numericSnapshot();
 assert(gap.initialized&&gap.gapActive&&gap.nackEnabled&&gap.pendingPackets==a.pendingPackets());
 assert(gap.nackPackets==1&&gap.activeRequests==1&&gap.counters.received==4);
 assert(gap.assemblyBytes==a.assemblyBytes()&&gap.pendingBytes==a.pendingBytes());
 const auto frozen=gap;a.observeRoundTrip(100);feed(a,101,300,false,{0x7c,0x01,0x22},163);
 const auto repaired=a.numericSnapshot();assert(!repaired.gapActive&&repaired.repairedPackets==1&&repaired.arrivalSamples==1);
 assert(repaired.arrivalP50Ms==140&&repaired.arrivalP95Ms==140&&repaired.arrivalBucketMs==10&&repaired.arrivalHistogram[14]==1);
 assert(repaired.roundTripKnown&&repaired.roundTripMs==100&&repaired.arrivalAfterRtt==1);
 assert(frozen.arrivalSamples==0&&frozen.activeRequests==1); // Returned immutable value, no maps/frames retained.
 A expiry;fragmentGap(expiry);expiry.takeNacks(113);expiry.tick(190);
 const auto expired=expiry.numericSnapshot();assert(expired.lastExpiryKnown&&expired.expiredGaps==1&&expired.expiredNackAttempts==2);
 assert(expired.lastExpiry.first==101&&expired.lastExpiry.last==101&&expired.lastExpiry.deadlineMs==183&&expired.lastExpiry.overshootMs==7);
 assert(expired.lastExpiry.firstNackMs==23&&expired.lastExpiry.lastNackMs==113&&expired.lastExpiry.pendingDepth==1);
 feed(expiry,101,300,false,{0x7c,0x01,0x22},191);const auto late=expiry.numericSnapshot();
 assert(late.lastLateKnown&&late.matchedLatePackets==1&&late.lastLate.lateByMs==8&&late.lastLate.nackAttempts==2);
 const auto pending=late.pendingEvents;std::string event;assert(expiry.pollEvent(event));assert(expiry.numericSnapshot().pendingEvents==pending-1);
 assert(expired.lastExpiry.deadlineMs==183&&expired.matchedLatePackets==0);
 stream::RtpRepairConfig config;config.media[96]=true;config.rtx[97]=96;config.mediaSsrc=1;config.rtxSsrc=2;
 stream::RtpRepair repair(config);auto source=packet(10);assert(repair.normalize(source,[](auto){return false;}));
 assert(!repair.nack({11,12}).empty());auto bad=packet(11,98,9);assert(!repair.normalize(bad,[](auto){return false;}));
 const auto rs=repair.numericSnapshot();assert(rs.media[96].present&&rs.media[96].nack&&rs.rtx[97].originalPayload==96);
 assert(rs.fid&&rs.sourceKnown&&rs.nackEnabled&&rs.nackBatches==1&&rs.rejected==1&&rs.mediaCount==1&&rs.rtxCount==1);
 config.media[128]=false;config.rtx[200]=300;stream::RtpRepair invalid(config);const auto bounded=invalid.numericSnapshot();
 assert(bounded.mediaCount==2&&bounded.rtxCount==2&&bounded.mediaOutOfRange==1&&bounded.rtxOutOfRange==1);
}
static void controlAndPerformance(){
 stream::IncomingControl incoming;auto stats=stream::IncomingControl::classify(R"({"id":"stats","cpu_usage":12})");
 incoming.receive(stats,1);incoming.receive(stats,2);incoming.pongSent();incoming.pongFailed();
 const auto copy=incoming.numericSnapshot();assert(copy.counts[1]==2&&copy.statsPending&&copy.firstPendingKnown&&copy.firstPendingMs==1);
 assert(copy.burst==2&&copy.highBurst==2&&copy.statsCoalesced==1&&copy.pongsSent==1&&copy.pongsFailed==1);
 stream::quality::Telemetry value;assert(incoming.takeStats(value,6));const auto after=incoming.numericSnapshot();
 assert(!after.statsPending&&after.burst==0&&after.delayHighMs==5&&copy.statsPending);
 stream::IncomingControlDiagnostics schemas;
 const auto description=stream::IncomingControlDiagnostics::describe(nlohmann::json{{"id","secret_canary"},{"token","NEVER-COPY"}},32);
 schemas.receive(description,1);schemas.receive(description,2);const auto pending=schemas.numericSnapshot();
 assert(pending.received==2&&pending.schemas==1&&pending.coalesced==1&&pending.pendingEvents==2&&pending.pendingSchemas==1);
 std::string event;assert(schemas.poll(event,2));assert(schemas.numericSnapshot().pendingEvents==0);
 stream::ServerPerformance performance;auto unknown=performance.numericSnapshot(0);
 for(const auto& f:unknown.fields)assert(!f.known&&!f.ageKnown&&f.stale);
 performance.receive(nlohmann::json{{"cpu_usage",0},{"encode","4.5"}},0);
 const auto fresh=performance.numericSnapshot(5000);assert(fresh.fields[0].known&&fresh.fields[0].value==0&&fresh.fields[0].ageKnown&&!fresh.fields[0].stale);
 assert(fresh.fields[0].unit==PerformanceUnit::Percent&&fresh.fields[3].unit==PerformanceUnit::RawUnspecified);
 assert(fresh.fields[3].value==4.5&&fresh.fields[3].sampledAtMs==0);
 assert(performance.numericSnapshot(5001).fields[3].stale);
 performance.receive(nlohmann::json{{"encode","invalid-secret"}},6000);assert(performance.numericSnapshot(6000).fields[3].value==4.5);
 performance.receive(nlohmann::json{{"encode",0}},7000);const auto future=performance.numericSnapshot(6999);
 assert(future.fields[3].known&&!future.fields[3].ageKnown&&future.fields[3].stale);
 performance.reset();assert(!performance.numericSnapshot(8000).fields[3].known&&fresh.fields[3].known);
 stream::TransportHistory history;using Kind=stream::TransportHistory::Kind;
 for(int i=0;i<20;++i)history.record(Kind::Peer,i,i);
 history.record(Kind::Closed,1,20,true,-1,0,12);history.record(Kind::Failure,2,21,true,99,99,99);
 const auto h=history.numericSnapshot();assert(h.count==16&&h.entries[0].at==6&&h.entries[15].at==21);
 assert(h.hasTerminal&&h.terminal.kind==Kind::Closed&&h.terminal.at==20);
 assert(!h.terminalVideoAge.known&&h.terminalAudioAge.known&&h.terminalAudioAge.valueMs==0&&h.terminalControlAge.valueMs==12);
 static_assert(std::is_trivially_copyable_v<decltype(h)>);
}
static void slots(){
 SnapshotSlot<RtpSnapshot> cache;RtpSnapshot value;assert(!cache.publish(0,0,value,0));
 cache.reset(1,0);const auto none=cache.read(0,10);assert(!none.hasValue&&none.publication==Publication::NotPublished);
 value.counters.received=11;value.expectedSequence=11;assert(cache.publish(1,0,value,0));
 assert(cache.read(0,10).hasValue&&cache.read(0,10).ageKnown&&!cache.read(0,10).stale);
 assert(cache.read(10,10).ageUs==10&&!cache.read(10,10).stale&&cache.read(11,10).stale);
 assert(cache.publish(1,0,value,1,true));assert(!cache.publish(1,0,value,2));
 cache.reset(2,1);assert(!cache.publish(1,0,value,3));assert(!cache.read(4,10).hasValue);
 assert(cache.failed(2,1)&&cache.read(4,10).publication==Publication::CaptureFailed);
 assert(cache.publish(2,1,value,5));assert(!cache.publish(2,1,value,4));
 assert(!cache.read(4,10).ageKnown&&cache.read(4,10).stale);
 cache.closeWithoutFinal(2,1);assert(!cache.read(6,10).hasValue&&cache.read(6,10).publication==Publication::ClosedWithoutFinal);
 cache.reset(3,2);std::atomic<bool> done{false};
 std::thread owner([&]{for(std::uint64_t i=1;i<=10000;++i){RtpSnapshot v;v.counters.received=i;v.expectedSequence=std::int64_t(i);assert(cache.publish(3,2,v,i));}done=true;});
 while(!done){const auto read=cache.read(10000,20000);if(read.hasValue)assert(read.value.counters.received==std::uint64_t(read.value.expectedSequence));}
 owner.join();assert(cache.read(10000,20000).revision==10000);
}
static void assemblyCache(){
 AssemblyTelemetryCache cache;cache.reset(7,0);
 {
  A owner;stream::RtpRepair repair;fragmentGap(owner);
  assert(cache.publishIfDue(7,0,owner,repair,0));const auto first=cache.read(0);
  assert(first.hasValue&&first.value.rtp.nackPackets==1&&first.value.rtp.pendingPackets==1);
  feed(owner,101,300,false,{0x7c,0x01,0x22},163);
  assert(!cache.publishIfDue(7,0,owner,repair,100));assert(cache.read(100).value.rtp.arrivalSamples==0);
  assert(cache.publishIfDue(7,0,owner,repair,100,true));
 }
 const auto final=cache.read(101);assert(final.publication==Publication::FinalRetained&&final.value.rtp.arrivalSamples==1);
 cache.reset(7,1);A replacement;stream::RtpRepair repair;
 assert(!cache.publishIfDue(7,0,replacement,repair,102));assert(!cache.read(102).hasValue);
 assert(cache.publishIfDue(7,1,replacement,repair,102));assert(!cache.read(102).value.rtp.initialized);
 assert(cache.read(2000103).stale);cache.closeWithoutFinal(7,1);assert(cache.read(2000103).publication==Publication::ClosedWithoutFinal);
}
int main(){numericRtp();controlAndPerformance();slots();assemblyCache();std::cout<<"Numeric RTP repair/expiry, incoming/server/history and bounded generation-safe cache tests passed\n";}
