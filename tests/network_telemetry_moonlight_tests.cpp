#define FOG_MOONLIGHT_LOG_INTEGRATION_TEST
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#define main moonlightFixtureMain
#define LiGetMicroseconds OriginalFixtureClock
#include "../tools/moonlight/tests/session_test.cpp"
#undef main
#undef LiGetMicroseconds
extern "C" uint64_t LiGetMicroseconds(void){return 1000000;}
#pragma GCC diagnostic pop
#include "features/streaming/application/network_telemetry_logging.hpp"
extern "C" {
#include "FogMoonlightDiagnosticsInternal.h"
}
#include <fstream>
#include "features/streaming/application/protocol_diagnostics_formatter.hpp"
namespace net=stream::telemetry::network;
namespace proto=stream::telemetry::protocol;
namespace{std::atomic<unsigned> legacyEvents{0};void observeLegacy(const std::string& s){if(s.find("Moonlight IDR ")==0||s.find("Moonlight thread ")==0)++legacyEvents;}}
void retainedClientCannotConsumeNewConnection(const char* directory){
 moonlight::MoonlightSession session(std::string(directory)+"/lease-handoff");
 pair(session);auto old=session.create();assert(old);old->enableNetworkEvents(true);
 FogDiagReset();assert(old->start({rental()}));eventually([&]{return old->ready();});
 FogDiagSetWaitingForIdr(1,9,70000);FogDiagIdrRequested(70001);
 net::NetworkEvent event;assert(old->peekNetworkEvent(event));const auto oldConnection=event.core.connection;
 while(old->peekNetworkEvent(event))assert(old->retireNetworkEvent(event.journalSequence,true));
 old->close();const auto oldFinal=old->captureNetworkTelemetry();assert(!old->peekNetworkEvent(event));
 // Public API permits re-pairing after close while the old Client remains alive.
 pair(session);auto next=session.create();assert(next);next->enableNetworkEvents(true);
 FogDiagReset();assert(next->start({rental()}));eventually([&]{return next->ready();});
 FogDiagSetWaitingForIdr(1,9,80000);FogDiagIdrRequested(80001);
 assert(!old->peekNetworkEvent(event));
 assert(old->captureNetworkTelemetry().eventCursor==oldFinal.eventCursor);
 unsigned received=0;
 while(next->peekNetworkEvent(event)){
  assert(event.kind==net::EventKind::Core&&event.core.connection==oldConnection+1);
  assert(event.streamEpoch!=oldFinal.streamEpoch);
  assert(next->retireNetworkEvent(event.journalSequence,true));++received;
 }
 assert(received==2);next->close();
}
int main(){
 char path[]="/tmp/fog-network-fh-XXXXXX";assert(mkdtemp(path));
 {
  moonlight::MoonlightSession session(path);pair(session);auto client=session.create();assert(client);
  assert(!client->captureNetworkTelemetry().core.hasValue);client->enableNetworkEvents(true);moonlight_diagnostics::sink=&observeLegacy;
  FogDiagReset();FogDiagAdd(FOG_DIAG_videoPacketsReceived,7);FogDiagAdd(FOG_DIAG_videoBytesReceived,777);
  FogDiagSetWaitingForIdr(1,5,1000);FogDiagIdrRequested(1100);FogDiagIdrDispatched(1200);FogDiagIdrDispatchResult(1,1300);
  FogDiagThreadStarted("VideoRecv",2,-2,7,59,0,1400);FogDiagThreadStarted("SECRET-ROLE-CANARY",0,0,0,0,0,1500);
  initialVideoFrames=0;assert(client->start({rental()}));eventually([&]{return client->ready();});
  net::NetworkEvent event;eventually([&]{return client->peekNetworkEvent(event);});
  const auto live=client->captureNetworkTelemetry();assert(live.provider==net::Provider::FogHub&&live.core.hasValue&&live.core.state==net::State::Live);
  eventually([&]{return client->captureProtocolTelemetry().fh.encryptionKnown;});
  const auto protocolLive=client->captureProtocolTelemetry();assert(protocolLive.state==proto::State::Live&&protocolLive.transportEpoch==live.transportEpoch);
  assert(proto::protocolText(protocolLive).rfind(client->diagnostics(),0)==0);assert(proto::mutexText(protocolLive).rfind(client->mutexDiagnostics(),0)==0);assert(proto::inputText(protocolLive).rfind(client->inputDiagnostics(),0)==0);
  assert(live.fh.core.videoPacketsReceived==7&&live.fh.core.videoBytesReceived==777&&live.core.ageKnown);
  assert(net::fhGapText(live.fh)==client->gapDiagnostics());assert(net::fhQueueText(live.fh,true)==client->queueDiagnostics());
  assert(legacyEvents==0&&!event.transportBindingKnown&&event.core.connection==1);
  fog::RunLog log;assert(log.start(path));
  while(client->peekNetworkEvent(event)){const auto r=net::drainNetworkEvents(*client,log,200,1);assert(r.accepted==1&&log.flush());}
  // Reproduce the actual worker-to-32-slot adapter overwrite: leave a complete
  // burst across its old one-second worker deadline, then consume production
  // immutable batches. Demand transfer must retain every core event, without
  // moving this sub-capacity burst's loss into the 256-entry core journal.
  for(const unsigned burst:{84u,unsigned(FOG_MOONLIGHT_EVENT_CAPACITY)}){
   const auto before=client->captureNetworkTelemetry();
   for(unsigned i=0;i<burst;++i)FogDiagIdrRequested(10000+burst*1000+i);
   std::this_thread::sleep_for(std::chrono::milliseconds(1100));
   assert(client->captureNetworkTelemetry().eventMissed==before.eventMissed);
   std::array<net::NetworkEvent,32> batch;unsigned delivered=0,batches=0;std::uint64_t lastSequence=0;
   for(unsigned turn=0;turn<10;++turn){
    const auto count=client->peekNetworkEvents(batch.data(),batch.size());if(!count)break;
    assert(count<=32);++batches;
    std::array<net::NetworkEvent,32> frozen;assert(client->peekNetworkEvents(frozen.data(),frozen.size())==count);
    for(std::size_t i=0;i<count;++i){
     assert(batch[i].kind==net::EventKind::Core&&batch[i].core.sequence>lastSequence);
     assert(batch[i].core.timeUs==10000+burst*1000+delivered);
     assert(batch[i].journalSequence==frozen[i].journalSequence&&batch[i].journalMissed==before.eventMissed);
     lastSequence=batch[i].core.sequence;++delivered;assert(client->retireNetworkEvent(batch[i].journalSequence,true));
    }
   }
   const auto after=client->captureNetworkTelemetry();
   assert(delivered==burst&&batches==(burst+30)/31&&!client->peekNetworkEvent(event));
   assert(after.eventMissed==before.eventMissed&&after.coreSourceMissed==before.coreSourceMissed);
  }
  // A true core-capacity overflow remains explicit and distinct. Mixed event
  // and CoreMissed transfer must reserve its loss-record slot, never self-drop.
  for(unsigned i=0;i<400;++i)FogDiagIdrRequested(2000+i);
  assert(client->peekNetworkEvent(event));
  assert(client->captureNetworkTelemetry().coreSourceMissed==400-FOG_MOONLIGHT_EVENT_CAPACITY);
  assert(client->captureNetworkTelemetry().eventMissed==0);
  while(client->peekNetworkEvent(event)){const auto r=net::drainNetworkEvents(*client,log,201,1);assert(r.accepted==1&&log.flush());}
  // The joined source can still retain the whole core ring. Exercise the same
  // bounded final retirement loop used by Player, with an uncertain first batch
  // followed by explicit discards; no retried admission and no hidden leftovers.
  for(unsigned i=0;i<400;++i)FogDiagIdrRequested(30000+i);
  client->close();
  const auto beforeDiscard=client->captureNetworkTelemetry();
  std::array<net::NetworkEvent,32> finalBatch;const auto finalCount=client->peekNetworkEvents(finalBatch.data(),finalBatch.size());
  assert(finalCount==32);
  for(std::size_t i=0;i<finalCount;++i)assert(client->retireNetworkEvent(finalBatch[i].journalSequence,false));
  unsigned discarded=0;
  for(unsigned attempt=0;attempt<client->networkEventFinalDiscardBudget()&&client->peekNetworkEvent(event);++attempt){
   assert(client->retireNetworkEvent(event.journalSequence,false));++discarded;
  }
  assert(discarded+finalCount==FOG_MOONLIGHT_EVENT_CAPACITY+1&&!client->peekNetworkEvent(event));
  const auto final=client->captureNetworkTelemetry();
  assert(final.coreSourceMissed==beforeDiscard.coreSourceMissed+400-FOG_MOONLIGHT_EVENT_CAPACITY);
  assert(final.eventMissed==beforeDiscard.eventMissed+discarded+finalCount);
  const auto protocolFinal=client->captureProtocolTelemetry();assert(protocolFinal.state==proto::State::FinalRetained&&protocolFinal.fh.encryptionKnown);assert(proto::protocolText(protocolFinal).rfind(client->diagnostics(),0)==0);assert(proto::inputText(protocolFinal).rfind(client->inputDiagnostics(),0)==0);
  assert(final.core.hasValue&&final.core.state==net::State::FinalRetained&&final.fh.core.videoPacketsReceived==7&&final.transportEpoch==live.transportEpoch);
  assert(net::enqueueNetwork(log,202,net::Line::GapFinal,final));assert(log.flush());
  FogDiagReset();connectionResult=-77;assert(client->start({rental()}));eventually([&]{return !client->error().empty();});client->close();
  const auto protocolFailed=client->captureProtocolTelemetry();assert(protocolFailed.transportEpoch==protocolFinal.transportEpoch+1&&protocolFailed.fh.encryption.encryptionSelected==0);
  const auto failed=client->captureNetworkTelemetry();assert(failed.transportEpoch==live.transportEpoch+1&&!failed.core.hasValue);
  assert(net::networkText(failed,net::Line::Gap).find("udp_video_packets=")==std::string::npos);connectionResult=0;
  moonlight_diagnostics::sink=nullptr;log.close();std::ifstream file(std::string(path)+"/last-run.log");const std::string text{std::istreambuf_iterator<char>(file),{}};
  assert(text.find("Moonlight IDR event=request seq=")!=std::string::npos&&text.find("core_us=1100")!=std::string::npos);
  assert(text.find("Moonlight thread role=VideoRecv connection=1 core=2")!=std::string::npos);
  assert(text.find("Moonlight IDR journal missed=")!=std::string::npos&&text.find("journalMissed=0")!=std::string::npos);
  assert(text.find("SECRET-ROLE-CANARY")==std::string::npos&&legacyEvents==0);
 }
 retainedClientCannotConsumeNewConnection(path);
 std::filesystem::remove_all(path);std::cout<<"Actual FH numeric core mapping and real producer journals -> typed writer, loss/final/early failure passed\n";
}
