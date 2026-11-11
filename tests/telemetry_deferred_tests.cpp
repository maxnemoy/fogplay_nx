#include "app/stream_telemetry_logging.hpp"
#include "features/streaming/infrastructure/async_control_sender.hpp"
#include "features/streaming/infrastructure/telemetry_capture.hpp"
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <mutex>
#include <unistd.h>
using namespace stream::telemetry;
namespace {
std::mutex gateMutex;std::condition_variable changed;bool entered=false,released=false;
class Block final:public fog::RunLog::DeferredRecord{
public:
 std::size_t rawBytes()const noexcept override{return sizeof(*this);}
 std::size_t maxOutputBytes()const noexcept override{return 100;}
 void format(fog::RunLog::DeferredSink& s)const override{
  std::unique_lock<std::mutex> l(gateMutex);entered=true;changed.notify_all();changed.wait(l,[]{return released;});s.append("blocked");
 }
};
class Fault final:public fog::RunLog::DeferredRecord{
public:
 explicit Fault(int mode):mode_(mode){}
 std::size_t rawBytes()const noexcept override{return mode_==3?16385:sizeof(*this);}
 std::size_t maxOutputBytes()const noexcept override{return 8;}
 void format(fog::RunLog::DeferredSink& s)const override{
  if(mode_==0)throw std::runtime_error("SECRET-CANARY");
  if(mode_==4)throw std::bad_alloc();
  s.append(mode_==1?"123456789":"a\nb");
 }
private:int mode_;
};
struct Fixture{
 std::string path;
 Fixture(){char n[]="/tmp/fog-telemetry-XXXXXX";const auto v=mkdtemp(n);assert(v);path=v;}
 ~Fixture(){std::filesystem::remove_all(path);}
};
std::string read(const std::string& path){std::ifstream f(path);return {std::istreambuf_iterator<char>(f),{}};}
void release(){std::lock_guard<std::mutex> l(gateMutex);released=true;changed.notify_all();}
void block(fog::RunLog& log){
 {std::lock_guard<std::mutex> l(gateMutex);entered=false;released=false;}
 assert(log.appendDeferred(std::make_unique<Block>()));log.requestFlush();
 std::unique_lock<std::mutex> l(gateMutex);assert(changed.wait_for(l,std::chrono::seconds(2),[]{return entered;}));
}
void golden(){
 Snapshot s;s.provider=Provider::Mts;s.transport={Availability::Available,Reason::None,Scope::TransportLifetime};
 s.input={Availability::Available,Reason::None,Scope::TransportRolling5s};s.inputLatency={2,10,20,20,3};
 assert(inputText(s)=="provider=MTS scope=rolling-5s boundary=pad-to-send-return accepted-only=1 samples=2 p50Us=10 p95Us=20 p99Us=20 overflowLifetime=3");
 s.mts.stages[0]={2,12,9,0};s.mts.stages[1]={1,2,2,0};
 const std::string expected="scope=transport-lifetime nested=1 iceThread=worker pumpCalls=2 pumpAvgUs=6 pumpMaxUs=9 nackCalls=1 nackAvgUs=2 nackMaxUs=2 icePublishCalls=0 icePublishAvgUs=0 icePublishMaxUs=0 receiveCalls=0 receiveAvgUs=0 receiveMaxUs=0 eventsCalls=0 eventsAvgUs=0 eventsMaxUs=0 qualityCalls=0 qualityAvgUs=0 qualityMaxUs=0 controlFlushCalls=0 controlFlushAvgUs=0 controlFlushMaxUs=0 pliCalls=0 pliAvgUs=0 pliMaxUs=0 heartbeatCalls=0 heartbeatAvgUs=0 heartbeatMaxUs=0";
 assert(transportText(s)==expected);
 Record r(42,Line::TransportFinal,s);fog::RunLog::DeferredSink sink(r.maxOutputBytes());r.format(sink);
 assert(sink.text()=="[42 ms] Stream transport final: "+expected);
 s.input={Availability::TemporarilyUnavailable,Reason::ClosedWithoutFinal,Scope::TransportRolling5s};
 assert(inputText(s)=="provider=MTS unavailable=closed");
 s.provider=Provider::FogHub;s.fh.core={Availability::NotStarted,Reason::NotYetPublished,Scope::TransportLifetime};
 assert(transportText(s).find("rx_cycle")==std::string::npos);
 assert(transportText(s).find("core_unavailable=not-started")!=std::string::npos);
 // Maximal numeric values must fit declared output limits without truncating fields.
 const auto n=std::numeric_limits<std::uint64_t>::max();
 s.transport={Availability::Available,Reason::None,Scope::TransportLifetime};s.fh.core=s.transport;
 for(auto& t:s.fh.adapter)t={n,n,n,n};
 for(auto& t:s.fh.receive)t={n,n,n,n};
 s.fh.emptyPolls=s.fh.invalidTimes=s.fh.bufferRequestedBytes=s.fh.bufferSelectedBytes=s.fh.bufferActualBytes=n;
 s.fh.bufferSetFailures=s.fh.bufferSetLastError=s.fh.bufferQuerySucceeded=s.fh.bufferQueryError=n;
 Record max( n,Line::Transport,s);fog::RunLog::DeferredSink maxSink(max.maxOutputBytes());max.format(maxSink);
 Playback playback;for(auto& t:playback.stages)t={n,n,n,n};
 playback.pumpGapMaxUs=playback.outsideMeasuredPumpMaxUs=playback.videoQueueWaitMaxMs=playback.audioQueueWaitMaxMs=n;
 playback.slowCycles=playback.batchVideoTotal=playback.batchVideoMax=playback.frameBudgetYields=playback.budgetYields=playback.catchupWindows=n;
 playback.logQueued=playback.logDropped=playback.logWritten=playback.logBatches=playback.logBatchLastUs=playback.logBatchMaxUs=n;
 Record maxPlayback(n,playback);fog::RunLog::DeferredSink ps(maxPlayback.maxOutputBytes());maxPlayback.format(ps);
 assert(ps.text().size()<2048);
 assert(sizeof(Snapshot)<=4096&&sizeof(Record)<=4096);
}
void ownedAndBarriers(){
 Fixture f;fog::RunLog log;assert(log.start(f.path));block(log);
 {
  auto owner=std::make_unique<Snapshot>();owner->provider=Provider::Mts;owner->transportEpoch=13;
  owner->input={Availability::Available,Reason::None,Scope::TransportRolling5s};owner->inputLatency={1,123,123,123,0};
  assert(log.appendDeferred(std::make_unique<Record>(17,Line::Input,*owner)));
  owner->inputLatency.p50Us=999; // subsequent mutation/destruction cannot affect writer
 }
 auto flushed=std::async(std::launch::async,[&]{return log.flush();});
 assert(flushed.wait_for(std::chrono::milliseconds(30))==std::future_status::timeout);
 assert(log.append("after-owner"));
 release();assert(flushed.get());assert(log.flush());log.close();
 const auto data=read(f.path+"/last-run.log");
 assert(data.find("blocked\n[17 ms] Input latency: provider=MTS")!=std::string::npos);
 assert(data.find("p50Us=123")!=std::string::npos&&data.find("999")==std::string::npos);
 assert(data.find("after-owner")>data.find("p50Us=123"));
 assert(log.stats().deferredRecords==0&&log.stats().residentBytes==0);
}
void limits(){
 Fixture f;fog::RunLog log;assert(log.start(f.path));block(log);
 Snapshot snapshot;for(int i=0;i<3;++i)assert(log.appendDeferred(std::make_unique<Record>(i,Line::Input,snapshot)));
 assert(!log.appendDeferred(std::make_unique<Record>(4,Line::Input,snapshot)));
 assert(log.stats().deferredRecords==4&&log.stats().droppedDeferred==1);
 std::string large(8192,'x');while(log.append(large)){}
 assert(log.stats().residentBytes<=fog::RunLog::maxQueuedBytes);
 release();assert(!log.flush());log.close();assert(log.stats().residentBytes==0);
 assert(read(f.path+"/last-run.log").find("enqueue rejection")!=std::string::npos);
}
void failures(){
 Fixture f;fog::RunLog log;assert(log.start(f.path));
 assert(!log.appendDeferred(std::make_unique<Fault>(3))); // raw size fails before format
 for(const int i:{0,1,2,4}){assert(log.appendDeferred(std::make_unique<Fault>(i)));log.requestFlush();
  for(int n=0;n<1000&&log.stats().deferredRecords;++n)std::this_thread::sleep_for(std::chrono::milliseconds(1));}
 assert(log.append("survives"));assert(!log.flush());log.close();
 auto stats=log.stats();assert(stats.formatFailures==4&&stats.droppedDeferred==5&&stats.droppedLines==5);
 const auto data=read(f.path+"/last-run.log");assert(data.find("SECRET-CANARY")==std::string::npos);
 assert(data.find("deferred-format failure")!=std::string::npos&&data.find("survives")!=std::string::npos);
}
void actualAppEnqueue(){
 struct Source final:stream::StreamTelemetrySource{
  mutable unsigned captures=0;
  Snapshot captureTelemetry(Capture mask)const noexcept override{
   ++captures;Snapshot s;s.provider=Provider::Mts;s.transportEpoch=9;
   if(includes(mask,Capture::Transport)){s.transport={Availability::Available,Reason::None,Scope::TransportLifetime};s.mts.stages[0]={2,12,9,0};}
   if(includes(mask,Capture::Input)){s.input={Availability::Available,Reason::None,Scope::TransportRolling5s};s.inputLatency={1,456,456,456,0};}
   return s;
  }
 };
 Fixture fixture;auto& log=fogplay::diagnostics::runLog;assert(log.start(fixture.path));block(log);
 {
  Source source;fogplay::diagnostics::diagnosticTransport(source,Line::TransportClosed);
  fogplay::diagnostics::diagnosticInput(source);assert(source.captures==2);
  fogplay::diagnostics::PlaybackTiming t{2,12,9};fog::RunLog::Stats stats;
  stats.queuedLines=11;stats.droppedLines=12;stats.writtenLines=13;stats.batches=14;stats.lastBatchUs=15;stats.maxBatchUs=16;
  const auto value=capturePlayback(t,t,t,t,t,t,t,t,stats,1,2,3,4,5,6,7,8,9,10);
  const std::string expected="Playback timing: drainCalls=2 drainAvgUs=6 drainMaxUs=9 presentCalls=2 presentAvgUs=6 presentMaxUs=9 transportPumpCalls=2 transportPumpAvgUs=6 transportPumpMaxUs=9 videoFeedCalls=2 videoFeedAvgUs=6 videoFeedMaxUs=9 decoderResetCalls=2 decoderResetAvgUs=6 decoderResetMaxUs=9 audioFeedCalls=2 audioFeedAvgUs=6 audioFeedMaxUs=9 prepareCalls=2 prepareAvgUs=6 prepareMaxUs=9 logEnqueueCalls=2 logEnqueueAvgUs=6 logEnqueueMaxUs=9 pumpGapMaxUs=1 outsideMeasuredPumpMaxUs=2 videoQueueWaitMaxMs=3 audioQueueWaitMaxMs=4 slowCycles=5 batchVideoTotal=6 batchVideoMax=7 frameBudgetYields=8 budgetYields=9 catchupWindows=10 logQueued=11 logDropped=12 logWritten=13 logBatches=14 logBatchLastUs=15 logBatchMaxUs=16";
  assert(playbackText(value)==expected);fogplay::diagnostics::diagnosticPlayback(value);
 }
 assert(log.stats().acceptedDeferred==4);release();assert(log.flush());log.close();
 const auto data=read(fixture.path+"/last-run.log");
 const auto transport=data.find("Stream transport closed: scope=transport-lifetime");
 const auto input=data.find("Input latency: provider=MTS");const auto playback=data.find("Playback timing:");
 assert(transport<input&&input<playback&&data.find("p50Us=456")!=std::string::npos);
 assert(data.find("Playback timing:",playback+1)==std::string::npos);
}
void closeBarrier(){
 Fixture fixture;fog::RunLog log;assert(log.start(fixture.path));block(log);assert(log.append("final-before-close"));
 auto closing=std::async(std::launch::async,[&]{log.close();});
 assert(closing.wait_for(std::chrono::milliseconds(30))==std::future_status::timeout);
 release();closing.get();assert(read(fixture.path+"/last-run.log").find("blocked\nfinal-before-close\n")!=std::string::npos);
}
void actualSender(){
 std::mutex m;std::condition_variable cv;bool sending=false,done=false;
 stream::AsyncControlSender sender([]{return true;},[&](const std::string&){
  std::unique_lock<std::mutex> l(m);sending=true;cv.notify_all();cv.wait(l,[&]{return done;});
 });
 sender.allow(true);assert(sender.enqueue("synthetic",stream::ControlQueue::Mode::Ordered));
 {std::unique_lock<std::mutex> l(m);assert(cv.wait_for(l,std::chrono::seconds(2),[&]{return sending;}));}
 const auto began=std::chrono::steady_clock::now();const auto s=captureMtsControl(sender.snapshot());
 const auto input=capturePercentiles(sender.inputLatencySnapshot(fog::metrics::steadyNowUs()));
 assert(std::chrono::steady_clock::now()-began<std::chrono::milliseconds(100));
 assert(s.inFlight&&s.transportActive&&input.samples==0);
 {std::lock_guard<std::mutex> l(m);done=true;cv.notify_all();}
 sender.stop();
}
}
int main(){golden();ownedAndBarriers();limits();failures();actualAppEnqueue();closeBarrier();actualSender();std::cout<<"Typed telemetry golden, owned deferred, blocked formatter, budget, failure and sender tests passed\n";}
