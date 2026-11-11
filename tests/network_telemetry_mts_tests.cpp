#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#define main mtsFixtureMain
#include "stream_integration.cpp"
#undef main
#pragma GCC diagnostic pop
#include "features/streaming/application/network_telemetry_logging.hpp"
#include <filesystem>
#include "features/streaming/application/protocol_diagnostics_formatter.hpp"
#include "features/streaming/application/player_diagnostics_formatter.hpp"
#include <map>
#include <sstream>
#include <fstream>
namespace net=stream::telemetry::network;
namespace proto=stream::telemetry::protocol;
static std::map<std::string,std::string> fields(const std::string& text){std::map<std::string,std::string> out;std::istringstream words(text);std::string word;while(words>>word){const auto at=word.find('=');if(at!=std::string::npos)out[word.substr(0,at)]=word.substr(at+1);}return out;}
static void protocolParity(const stream::Client& client){const auto captured=client.captureProtocolTelemetry();const auto old=fields(client.diagnostics());const auto now=fields(proto::protocolText(captured));for(const auto& field:now){if(field.first.rfind("protocol",0)==0||field.first.find("AgeMs")!=std::string::npos||field.first.find("AvgUs")!=std::string::npos||field.first.find("MaxUs")!=std::string::npos||field.first.find("Calls")!=std::string::npos)continue;const auto found=old.find(field.first);assert(found!=old.end()&&found->second==field.second);}assert(proto::inputText(captured).rfind(client.inputDiagnostics(),0)==0);}

int main(){
 {
  Fixture fixture;stream::Client client;
  assert(!client.captureNetworkTelemetry().assembly.hasValue);
  const std::string endpoint="ws://127.0.0.1:"+std::to_string(fixture.server->port())+"/ws/";
  assert(client.startForTesting("fixture","abcde",endpoint));
  auto pump=[&]{fixture.pump();client.pump();fixture.sendMedia();std::this_thread::sleep_for(std::chrono::milliseconds(2));};
  const auto began=Clock::now();
  while(Clock::now()-began<std::chrono::seconds(12)){
   pump();const auto s=client.captureNetworkTelemetry();if(s.assembly.hasValue&&s.mts.rtp.counters.received>2&&s.mts.queue.audioCadence.packets>=3&&fixture.controlReady)break;
  }
  auto live=client.captureNetworkTelemetry();assert(live.provider==net::Provider::Mts&&live.streamEpoch&&live.transportEpoch==1&&live.assembly.hasValue);
  assert(live.dtls.hasValue&&live.mtsDtls.available&&live.dtls.capturedAtUs&&live.dtls.state==net::State::Live);
  assert(live.detailedIceMigrated&&live.ice.hasValue&&live.ice.capturedAtUs&&live.ice.state==net::State::Live);
  const auto& audio=live.mts.queue.audioCadence;
  assert(audio.packets>=3&&audio.bytes==audio.packets*3&&audio.segments==1&&audio.segmentTicks>=960&&audio.segmentPackets==audio.packets);
  assert(audio.lastArrivalMs>=audio.firstArrivalMs&&audio.segmentLastArrivalMs>=audio.segmentFirstArrivalMs);
  assert(net::networkText(live,net::Line::Queue).find("audioCadenceBoundary=depacketized-before-queue")!=std::string::npos);
  protocolParity(client);const auto protocolLive=client.captureProtocolTelemetry();assert(protocolLive.state==proto::State::Live&&protocolLive.mts.videoMutexKnown&&protocolLive.transportEpoch==live.transportEpoch);
  assert(live.mtsIce.availability==net::IceCounters::Availability::Available);
  assert(live.mtsUdpPoll.hasValue&&live.mtsUdpPoll.provenance==1&&live.mtsUdpPoll.mappingKnown);
  assert(live.mtsUdpPoll.queryAfterUs>=live.mtsUdpPoll.queryBeforeUs);
  assert(live.mts.rtp.counters.received>0&&live.assembly.generation==live.videoGeneration&&live.history.hasValue&&live.control.hasValue);
  client.receiveControlForTesting(R"({"id":"stats","cpu_usage":0,"encode":"4.5","token":"SECRET-NETWORK-CANARY"})");client.pump();
  live=client.captureNetworkTelemetry();assert(live.mtsControl.performance.fields[0].known&&live.mtsControl.performance.fields[0].value==0);
  assert(live.mtsControl.performance.fields[3].value==4.5);
  // Freeze assembly owner and block the SDK ICE worker. UI capture still reads
  // the last bounded publication, without taking video or SDK diagnostic locks.
  client.pauseAssemblyForTesting(true);client.blockIceForTesting(true);std::this_thread::sleep_for(std::chrono::milliseconds(30));
  const auto paused=client.captureNetworkTelemetry();const auto at=Clock::now();
  for(int i=0;i<100;++i){const auto protocol=client.captureProtocolTelemetry();assert(protocol.transportEpoch==protocolLive.transportEpoch&&protocol.mts.videoMutexKnown);const auto s=client.captureNetworkTelemetry();assert(s.ice.revision==paused.ice.revision&&s.ice.capturedAtUs==paused.ice.capturedAtUs);assert(s.dtls.revision==paused.dtls.revision&&s.dtls.capturedAtUs==paused.dtls.capturedAtUs);assert(s.assembly.revision==paused.assembly.revision&&s.assembly.capturedAtUs==paused.assembly.capturedAtUs);}
  assert(Clock::now()-at<std::chrono::milliseconds(100));client.blockIceForTesting(false);client.pauseAssemblyForTesting(false);
  net::NetworkEvent first,again;assert(client.peekNetworkEvent(first)&&client.peekNetworkEvent(again));
  assert(first.journalSequence==again.journalSequence&&first.kind==net::EventKind::Rtp&&first.transportBindingKnown&&first.videoBindingKnown);
  assert(first.rtp.ms&&first.streamEpoch==live.streamEpoch&&first.transportEpoch==live.transportEpoch);
  char path[]="/tmp/fog-network-mts-XXXXXX";assert(mkdtemp(path));fog::RunLog log;assert(log.start(path));
  assert(client.supportsControlDiscoveryTelemetry());
  const std::string discoveryCanary=R"({"id":"InnocentControlId91723","OpaqueKey9152":"SECRET-DISCOVERY-CANARY","token":"TOKEN-DISCOVERY-CANARY"})";
  client.receiveControlForTesting(discoveryCanary);
  std::array<net::ControlDiscoveryEvent,4> discovery{};bool sawPrivate=false;
  for(unsigned attempt=0;attempt<64;++attempt){const auto count=client.peekControlDiscoveryEvents(discovery.data(),1,true);if(!count)break;
    for(std::size_t i=0;i<count;++i){const auto frozen=discovery[i];const auto text=net::controlDiscoveryText(frozen);assert(text.find("SECRET-DISCOVERY-CANARY")==text.npos&&text.find("OpaqueKey9152")==text.npos&&text.find("InnocentControlId91723")==text.npos);if(frozen.description.id==2){sawPrivate=true;client.receiveControlForTesting(discoveryCanary);}
      assert(log.appendDeferred(std::make_unique<fog::stream::diagnostics::ControlDiscoveryRecord>(99,&frozen,1))&&log.flush());assert(client.retireControlDiscoveryEvent(frozen.sequence,true));
      if(sawPrivate){net::ControlDiscoveryEvent newer;assert(client.peekControlDiscoveryEvents(&newer,1,true)==1&&newer.pending>=1);assert(client.retireControlDiscoveryEvent(newer.sequence,false));}
    }
    if(sawPrivate)break;
  }
  assert(sawPrivate);
  const auto drained=net::drainNetworkEvents(client,log,100,1);assert(drained.accepted==1&&log.flush());
  bool sawQuality=false;
  for(unsigned attempt=0;attempt<128;++attempt){net::NetworkEvent transition;if(!client.peekNetworkEvent(transition))break;if(transition.kind==net::EventKind::Quality){sawQuality=true;assert(transition.transportBindingKnown&&!transition.videoBindingKnown&&transition.videoGeneration==0);}else if(transition.kind==net::EventKind::Rtp||transition.kind==net::EventKind::Fixed||transition.kind==net::EventKind::RouteSelected||transition.kind==net::EventKind::RouteChanged)assert(transition.videoBindingKnown);assert(client.retireNetworkEvent(transition.journalSequence,false));}
  assert(sawQuality);
  assert(net::enqueueNetwork(log,101,net::Line::Gap,live));assert(log.flush());
  const auto beforeClose=client.captureNetworkTelemetry();
  client.close();const auto final=client.captureNetworkTelemetry();
  const auto protocolFinal=client.captureProtocolTelemetry();assert(protocolFinal.state==proto::State::FinalRetained&&protocolFinal.transportEpoch==protocolLive.transportEpoch&&protocolFinal.mts.videoMutexKnown);protocolParity(client);assert(proto::inputText(protocolFinal).rfind(client.inputDiagnostics(),0)==0);
  for(unsigned attempt=0;attempt<64;++attempt){net::ControlDiscoveryEvent pending;if(!client.peekControlDiscoveryEvents(&pending,1,true))break;assert(client.retireControlDiscoveryEvent(pending.sequence,false));}
  assert(final.assembly.state==net::State::FinalRetained&&final.assembly.hasValue&&final.control.state==net::State::FinalRetained&&final.history.state==net::State::FinalRetained);
  assert(final.dtls.hasValue&&final.dtls.state==net::State::FinalRetained);
  assert(final.ice.hasValue&&final.ice.state==net::State::FinalRetained);
  assert(final.mtsIce.provenance==net::IceCounters::Provenance::FinalRetained);
  assert(final.mtsUdpPoll.hasValue&&final.mtsUdpPoll.provenance==2);
  assert(final.mtsUdpPoll.queryAfterUs==beforeClose.mtsUdpPoll.queryAfterUs);
  assert(final.mtsUdpPoll.sampleMonoUs==beforeClose.mtsUdpPoll.sampleMonoUs);
  assert(net::networkText(final,net::Line::IceFinal).find("udpPollProvenance=2")!=std::string::npos);
  assert(final.mts.rtp.counters.received>=live.mts.rtp.counters.received&&final.transportEpoch==live.transportEpoch);
  assert(final.mts.queue.audioCadence.packets>=audio.packets);
  assert(net::enqueueNetwork(log,102,net::Line::VideoFinal,final));assert(log.flush());log.close();
  std::ifstream file(std::string(path)+"/last-run.log");const std::string text{std::istreambuf_iterator<char>(file),{}};
  assert(text.find("Video RTP event: reason=")!=std::string::npos&&text.find("Video RTP final: nackEnabled=")!=std::string::npos);
  assert(text.find("Stream control: provider=mts ")!=text.npos&&text.find("discoverySchema=2")!=text.npos&&text.find("OpaqueKey9152")==text.npos&&text.find("SECRET-DISCOVERY-CANARY")==text.npos);
  assert(text.find("SECRET-NETWORK-CANARY")==std::string::npos&&text.find("snapshotState=final")!=std::string::npos);
  std::filesystem::remove_all(path);
  assert(client.startForTesting("fixture","abcdf",endpoint));const auto next=client.captureNetworkTelemetry();
  const auto protocolNext=client.captureProtocolTelemetry();assert(protocolNext.transportEpoch==protocolFinal.transportEpoch+1&&protocolNext.mts.input.psFrames==0);
  assert(next.transportEpoch==final.transportEpoch+1&&next.streamEpoch==final.streamEpoch);
  assert(!next.dtls.hasValue);
  assert(!next.mtsUdpPoll.hasValue&&next.mtsUdpPoll.eventCursor==0&&next.mtsUdpPoll.sourceMissed==0);
  assert(!next.ice.hasValue&&next.mtsIce.availability==net::IceCounters::Availability::NotStarted);
  if(next.assembly.hasValue){assert(next.mts.rtp.counters.received==0);assert(next.mts.queue.audioCadence.packets==0);}
  client.close();
 }
 stream::Client::cleanupRuntime();std::cout<<"Actual MTS owner/cache publication, blocked ICE/assembly UI capture, journal/final/reconnect/typed writer passed\n";
}
