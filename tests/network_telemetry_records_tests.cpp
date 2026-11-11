#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#define main repairFixtureMain
#include "rtp_repair_tests.cpp"
#undef main
#pragma GCC diagnostic pop
#include "features/streaming/application/network_telemetry_logging.hpp"
#include "features/streaming/infrastructure/bounded_network_events.hpp"
#include "features/streaming/infrastructure/server_performance.hpp"
#include "features/streaming/infrastructure/transport_history.hpp"
#include "features/streaming/infrastructure/incoming_control.hpp"
#include "features/streaming/infrastructure/stream_queue.hpp"
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <unistd.h>
namespace net=stream::telemetry::network;
namespace {
struct Directory{std::string path;Directory(){char p[]="/tmp/fog-network-record-XXXXXX";auto v=mkdtemp(p);assert(v);path=v;}~Directory(){std::filesystem::remove_all(path);}};
std::string read(const std::string& name){std::ifstream f(name);return {std::istreambuf_iterator<char>(f),{}};}
std::mutex gateMutex;std::condition_variable gateChanged;bool entered=false,released=false;
class Gate final:public fog::RunLog::DeferredRecord{
public:std::size_t rawBytes()const noexcept override{return sizeof(*this);}std::size_t maxOutputBytes()const noexcept override{return 8;}
void format(fog::RunLog::DeferredSink& sink)const override{std::unique_lock lock(gateMutex);entered=true;gateChanged.notify_all();gateChanged.wait(lock,[]{return released;});sink.append("gate");}};
void block(fog::RunLog& log){{std::lock_guard lock(gateMutex);entered=released=false;}assert(log.appendDeferred(std::make_unique<Gate>()));log.requestFlush();std::unique_lock lock(gateMutex);assert(gateChanged.wait_for(lock,std::chrono::seconds(2),[]{return entered;}));}
void release(){std::lock_guard lock(gateMutex);released=true;gateChanged.notify_all();}
class Source final:public stream::StreamNetworkTelemetrySource{
public:net::EventJournal<net::NetworkEvent> journal;
bool peekNetworkEvent(net::NetworkEvent& e)const noexcept override{net::EventJournal<net::NetworkEvent>::Entry entry;if(!journal.peek(entry))return false;e=entry.value;e.journalSequence=entry.sequence;e.journalMissed=entry.missed;return true;}
bool retireNetworkEvent(std::uint64_t seq,bool admitted)noexcept override{return journal.retire(seq,admitted);}
};
void audioCadence(){
 stream::AudioRtpCadence cadence;
 assert(cadence.snapshot().packets==0&&cadence.snapshot().segments==0);
 // One RTP wrap remains in the same monotonic segment; source media advances
 // 20 ms over 20 ms locally, even when the first receipt is at local time zero.
 cadence.received(0xffffff00u,3,0);cadence.received(224,4,10);cadence.received(704,5,20);
 const auto wrapped=cadence.snapshot();
 assert(wrapped.packets==3&&wrapped.bytes==12&&wrapped.firstArrivalMs==0&&wrapped.lastArrivalMs==20);
 assert(wrapped.segments==1&&wrapped.wraps==1&&wrapped.segmentTicks==960&&wrapped.maximumGapMs==10&&wrapped.segmentPackets==3);
 cadence.received(704,3,20);assert(cadence.snapshot().repeatedTimestamps==1&&cadence.snapshot().burstIntervals==1&&cadence.snapshot().segmentPackets==4);
 // Reordered/reset timestamps terminate the segment rather than inflating a
 // purported cumulative lifetime duration from forward-only differences.
 cadence.received(224,3,21);
 auto changed=cadence.snapshot();assert(changed.backsteps==1&&changed.segments==2&&changed.segmentTicks==0&&changed.segmentPackets==1);
 cadence.received(1184,3,30);changed=cadence.snapshot();
 assert(changed.segmentTicks==960&&changed.segmentFirstArrivalMs==21&&changed.segmentLastArrivalMs==30&&changed.segmentPackets==2);
 cadence.received(1184+stream::AudioRtpCadence::MaximumStepTicks+1,3,40);
 changed=cadence.snapshot();assert(changed.jumps==1&&changed.segments==3&&changed.segmentTicks==0);
 cadence.received(0,3,50);changed=cadence.snapshot();assert(changed.backsteps==2&&changed.segments==4&&changed.segmentTicks==0);
 cadence.received(480,3,49);changed=cadence.snapshot();assert(changed.clockRegressions==1&&changed.segments==5&&changed.segmentTicks==0);
 cadence.received(960,3,59);changed=cadence.snapshot();assert(changed.segmentTicks==480&&changed.segmentLastArrivalMs==59);
 // Boundaries are inclusive and counters overlap. Clock regressions are not
 // interpreted as a gap; repeated RTP timestamps still count as received packets.
 stream::AudioRtpCadence gaps;
 gaps.received(0,1,1000);gaps.received(480,1,1039);
 assert(gaps.snapshot().gapsGe40Ms==0);
 gaps.received(960,1,1079);gaps.received(1440,1,1178);
 assert(gaps.snapshot().gapsGe40Ms==2&&gaps.snapshot().gapsGe100Ms==0);
 gaps.received(1920,1,1278);gaps.received(2400,1,1527);gaps.received(2880,1,1777);
 auto threshold=gaps.snapshot();
 assert(threshold.gapsGe40Ms==5&&threshold.gapsGe100Ms==3&&threshold.gapsGe250Ms==1&&threshold.segmentPackets==7);
 gaps.received(2880,1,1777);gaps.received(3360,1,1776);threshold=gaps.snapshot();
 assert(threshold.gapsGe40Ms==5&&threshold.gapsGe100Ms==3&&threshold.gapsGe250Ms==1);
 assert(threshold.clockRegressions==1&&threshold.segmentPackets==1&&threshold.repeatedTimestamps==1);
 net::NetworkSnapshot thresholdSnapshot;thresholdSnapshot.provider=net::Provider::Mts;thresholdSnapshot.assembly.hasValue=true;
 thresholdSnapshot.mts.queue.audioCadence=threshold;
 const auto thresholdText=net::networkText(thresholdSnapshot,net::Line::Queue);
 assert(thresholdText.find("audioRxGapsGe40Ms=5")!=thresholdText.npos&&thresholdText.find("audioRxGapsGe100Ms=3")!=thresholdText.npos);
 assert(thresholdText.find("audioRxGapsGe250Ms=1")!=thresholdText.npos&&thresholdText.find("audioRtpSegmentPackets=1")!=thresholdText.npos);
 net::NetworkSnapshot snapshot;snapshot.provider=net::Provider::Mts;snapshot.assembly.hasValue=true;
 snapshot.mts.queue.audioCadence=changed;
 net::NetworkSnapshot fh;fh.provider=net::Provider::FogHub;
 assert(net::networkText(fh,net::Line::Queue).find("audioCadenceAvailable=0 audioCadenceReason=unsupported-provider")!=std::string::npos);
 const auto text=net::networkText(snapshot,net::Line::Queue);
 assert(text.find("audioCadenceBoundary=depacketized-before-queue")!=text.npos&&text.find("audioCadenceAbsoluteAge=unavailable")!=text.npos);
 assert(text.find("audioRxPackets=10")!=text.npos&&text.find("audioRtpSegmentTicks=480")!=text.npos);
 // The longest cadence numeric values fit the existing deferred Queue line cap.
 const auto maximum=~std::uint64_t{0};auto& a=snapshot.mts.queue.audioCadence;
 a={maximum,maximum,maximum,maximum,maximum,maximum,maximum,maximum,maximum,maximum,maximum,maximum,maximum,maximum,maximum,maximum,maximum,maximum,maximum};
 assert(net::networkText(snapshot,net::Line::Queue).size()+net::observationText(snapshot.assembly).size()<net::lineLimit(net::Line::Queue));
}
void golden(){
 net::NetworkEvent packetGap;packetGap.provider=net::Provider::FogHub;packetGap.kind=net::EventKind::Core;
 packetGap.core.kind=12;packetGap.core.sequence=5;packetGap.core.connection=2;packetGap.core.wait=7;
 packetGap.core.timeUs=900000;packetGap.core.value=600000;
 assert(net::coreEventText(packetGap)=="Moonlight receive event=packet-gap seq=5 connection=2 wait=7 core_us=900000 gapUs=600000 thresholdUs=50000 previousCoreUs=300000");
 packetGap.core.timeUs=1;
 assert(net::coreEventText(packetGap).find("previousCoreUs=unknown")!=std::string::npos);
 A a;fragmentGap(a);assert(net::rtpText(a.numericSnapshot())==a.diagnostics());assert(net::gapText(a.numericSnapshot())==a.gapDiagnostics());
 a.takeNacks(113);a.tick(190);assert(net::gapText(a.numericSnapshot())==a.gapDiagnostics());
 A copy=a;net::RtpEvent event;std::string legacy;while(a.pollNumericEvent(event)){assert(copy.pollEvent(legacy));assert(net::rtpEventText(event)==legacy);}
 stream::RtpRepairConfig config;config.media[98]=true;config.media[100]=false;config.rtx[99]=98;config.mediaSsrc=1;config.rtxSsrc=2;
 stream::RtpRepair repair(config);assert(net::repairText(repair.numericSnapshot())==repair.diagnostics());
 stream::ServerPerformance server;server.receive(nlohmann::json{{"cpu_usage",0},{"encode","4.5"}},0);
 assert(net::performanceText(server.numericSnapshot(5001))==server.diagnostics(5001));
 stream::TransportHistory history;history.record(stream::TransportHistory::Kind::Failure,7,123,true,-1,0,8);
 assert(net::historyText(history.numericSnapshot())==history.diagnostics());
 stream::quality::Controller quality;stream::quality::Telemetry telemetry;stream::quality::Sample sample;
 assert(net::autoQualityText(quality.numericSnapshot())==quality.diagnostics());quality.enable(true,0);
 for(std::uint64_t i=0;i<100;++i){telemetry.profile.set(stream::quality::Profile::Middle,i*500);telemetry.fps.set(30,i*500);sample.frames+=15;sample.decodedFrames+=15;
  sample.workUs+=5000;sample.decoderWorkUs+=20000;sample.videoGeneration=i/10;sample.recovering=i%8==0;sample.workerVideo=i>20;
  quality.observe(i*500,sample,telemetry);assert(net::autoQualityText(quality.numericSnapshot())==quality.diagnostics());}
 stream::MediaQueue queue;net::QueueSnapshot q;assert(net::pressureText(q)==queue.pressureText());
 net::NetworkSnapshot snapshot;snapshot.provider=net::Provider::Mts;snapshot.assembly.hasValue=true;snapshot.mts.rtp=a.numericSnapshot();snapshot.mts.repair=repair.numericSnapshot();
 net::NetworkRecord record(42,net::Line::Video,snapshot);fog::RunLog::DeferredSink sink(record.maxOutputBytes());record.format(sink);
 assert(sink.text().find("[42 ms] Video RTP: nackEnabled=")==0);
 assert(sizeof(net::NetworkRecord)<=9216&&sizeof(net::EventRecord)<=1024);
 // A maximum valid PT list is clamped to the original group's output limit.
 for(auto& media:snapshot.mts.repair.media)media={true,true};
 for(auto& rtx:snapshot.mts.repair.rtx)rtx={true,127,true};
 snapshot.mts.repair.mediaCount=snapshot.mts.repair.rtxCount=128;
 net::NetworkRecord large(42,net::Line::Video,snapshot);fog::RunLog::DeferredSink bounded(large.maxOutputBytes());large.format(bounded);
 assert(bounded.text().size()<=4096+64);
}
void chargeAndLifetime(){
 Directory dir;fog::RunLog log;assert(log.start(dir.path));block(log);const auto initial=log.stats().residentBytes;
 net::NetworkSnapshot snapshot;snapshot.provider=net::Provider::Mts;snapshot.assembly.hasValue=true;snapshot.mts.rtp.counters.received=123;
 net::NetworkRecord layout(77,net::Line::Video,snapshot);const auto charge=layout.rawBytes()+layout.maxOutputBytes()+1;
 for(int i=0;i<3;++i)assert(net::enqueueNetwork(log,77,net::Line::Video,snapshot));
 assert(log.stats().deferredRecords==4&&log.stats().residentBytes==initial+3*charge&&log.stats().residentBytes<=fog::RunLog::maxQueuedBytes);
 assert(!net::enqueueNetwork(log,77,net::Line::Video,snapshot));snapshot.mts.rtp.counters.received=999;
 release();assert(!log.flush());log.close();const auto text=read(dir.path+"/last-run.log");
 assert(text.find(" rtp=123 ")!=std::string::npos&&text.find(" rtp=999 ")==std::string::npos);
 assert(log.stats().residentBytes==0&&log.stats().deferredRecords==0);
 Directory next;fog::RunLog owned;assert(owned.start(next.path));block(owned);
 {Source source;net::NetworkEvent event;event.provider=net::Provider::Mts;event.rtp.reason=6;event.rtp.ms=1234;event.rtp.sequence=99;source.journal.push(event);
  const auto done=net::drainNetworkEvents(source,owned,88,1);assert(done.accepted==1&&source.journal.size()==0);}
 release();assert(owned.flush());owned.close();assert(read(next.path+"/last-run.log").find("[88 ms] Video RTP event: reason=6 ms=1234 seq=99")!=std::string::npos);
}
void journalAndAdmission(){
 Source source;net::NetworkEvent e;e.provider=net::Provider::Mts;e.rtp.reason=1;
 for(unsigned i=0;i<40;++i){e.rtp.sequence=i;source.journal.push(e);}assert(source.journal.size()==32&&source.journal.lost()==8);
 net::NetworkEvent first,again;assert(source.peekNetworkEvent(first)&&source.peekNetworkEvent(again)&&first.journalSequence==again.journalSequence&&first.rtp.sequence==8);
 Directory dir;fog::RunLog log;assert(log.start(dir.path));block(log);
 for(int i=0;i<3;++i)assert(log.appendDeferred(std::make_unique<net::EventRecord>(1,e)));
 const auto rejected=net::drainNetworkEvents(source,log,2,4);assert(rejected.accepted==0&&rejected.uncertain==1&&source.journal.size()==31&&source.journal.lost()==9);
 assert(source.peekNetworkEvent(again)&&again.journalSequence==first.journalSequence+1&&again.journalMissed==9);
 release();assert(!log.flush());const auto accepted=net::drainNetworkEvents(source,log,3,1);assert(accepted.accepted==1&&accepted.uncertain==0);
 assert(!log.flush());
 assert(net::discardNetworkEvents(source,log,999)==30&&source.journal.size()==0&&source.journal.lost()==39&&source.journal.cursor()==40);
 assert(!log.flush());assert(log.stats().droppedDeferred==31);log.close();const auto text=read(dir.path+"/last-run.log");
 assert(text.find("journalSequence="+std::to_string(again.journalSequence)+" journalMissed=9")!=std::string::npos);
}
void b1Fixture(){
 const char* path=std::getenv("FOG_NETWORK_FIXTURE");if(!path)return;
 A owner;fragmentGap(owner);owner.takeNacks(113);owner.tick(190);
 net::NetworkSnapshot snapshot;snapshot.provider=net::Provider::Mts;snapshot.assembly.hasValue=true;snapshot.assembly.state=net::State::FinalRetained;
 snapshot.assembly.capturedAtUs=190000;snapshot.assembly.ageKnown=true;snapshot.mts.rtp=owner.numericSnapshot();
 std::ofstream out(path);out<<"[0 ms] FogPlay NX 0.6.30 startup.\n[0 ms] Diagnostic clock: schema=1 clock=steady startupEpochMs=0 resolutionMs=1\n[1 ms] Controller profile: xbox; provider=mts\n";
 net::RtpEvent event;std::uint64_t serial=0;
 while(owner.pollNumericEvent(event)){net::NetworkEvent value;value.provider=net::Provider::Mts;value.rtp=event;value.journalSequence=++serial;
  net::EventRecord record(250,value);fog::RunLog::DeferredSink sink(record.maxOutputBytes());record.format(sink);out<<sink.text()<<'\n';}
 net::NetworkRecord record(300,net::Line::VideoFinal,snapshot);fog::RunLog::DeferredSink sink(record.maxOutputBytes());record.format(sink);out<<sink.text()<<'\n';
 A fresh;snapshot.mts.rtp=fresh.numericSnapshot();snapshot.assembly.state=net::State::Live;snapshot.assembly.generation=2;
 net::NetworkRecord reset(400,net::Line::Video,snapshot);fog::RunLog::DeferredSink changed(reset.maxOutputBytes());reset.format(changed);out<<changed.text()<<'\n';
}

}
int main(){audioCadence();golden();chargeAndLifetime();journalAndAdmission();b1Fixture();std::cout<<"Network formatter parity, bounds/actual shared charge, owned records, journal/admission uncertainty passed\n";}
