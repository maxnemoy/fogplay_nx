#include "features/streaming/application/protocol_diagnostics_formatter.hpp"
#include "features/streaming/application/network_diagnostics_formatter.hpp"
#include "features/streaming/domain/auto_quality.hpp"
#include "features/streaming/infrastructure/protocol_telemetry_capture.hpp"
#include "features/streaming/infrastructure/control_delivery.hpp"
#include <cassert>
#include <iostream>
#include <limits>
#include <new>
#include <atomic>
namespace p=stream::telemetry::protocol;
static std::atomic<bool> failAlloc{false};
void* operator new(std::size_t n){if(failAlloc)throw std::bad_alloc();if(auto* v=std::malloc(n?n:1))return v;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* v)noexcept{std::free(v);}void operator delete[](void* v)noexcept{std::free(v);}
void operator delete(void* v,std::size_t)noexcept{std::free(v);}void operator delete[](void* v,std::size_t)noexcept{std::free(v);}
int main(){
 p::ProtocolSnapshot s;s.provider=p::Provider::Mts;
 assert(p::protocolText(s)=="provider=MTS unavailable=not-published");assert(p::mutexText(s)==p::protocolText(s));assert(p::inputText(s)==p::protocolText(s));
 s.state=p::State::CaptureFailed;assert(p::controlDeliveryText(s)=="provider=MTS unavailable=capture-failed");
 s.state=p::State::Live;s.capturedAtSteadyUs=10000000;s.sourceCapturedAtSteadyUs=9000000;
 stream::TimedMutex::Snapshot stats{};stats[0]={2,7000,9000,5000,6000,1,2};const auto numeric=p::captureMutex(stats);
 assert(p::mutexGroup(numeric,"event",stream::TimedMutex::DiagnosticsEnabled)==stream::TimedMutex::diagnostics(stats,"event"));
 for(const auto n:{std::uint64_t{0},std::uint64_t{1},std::numeric_limits<std::uint64_t>::max()}){stats[0]={n,n,n,n,n,n,n};assert(p::mutexGroup(p::captureMutex(stats),"video",stream::TimedMutex::DiagnosticsEnabled)==stream::TimedMutex::diagnostics(stats,"video"));}
 stream::ControlDelivery delivery;delivery.sample(65537,1000);const auto raw=delivery.snapshot();auto& d=s.mts.delivery;d.known=raw.known;d.bytes=raw.currentBytes;d.maxBytes=raw.maxBytes;d.sampledAtMs=raw.sampledAt;d.blocked=raw.blocked;d.blockedSinceMs=raw.blockedSince;
 for(auto now:{std::uint64_t{500},std::uint64_t{1000},std::uint64_t{6000},std::uint64_t{6001}}){s.capturedAtSteadyUs=now*1000;assert(p::controlDeliveryText(s).rfind(delivery.diagnostics(now),0)==0);}
 s.mts.route.local=255;s.mts.route.remoteProtocol=255;s.mts.operation=static_cast<p::Operation>(255);const auto route=p::protocolText(s);assert(route.find("localType=unknown")!=route.npos&&route.find("operation=unknown")!=route.npos);
 auto captured=s;failAlloc=true;const auto copied=captured;const auto counters=p::captureMutex(stats);failAlloc=false;assert(copied.mts.delivery.bytes==65537&&counters[0].count==stats[0].count);
 s.mts.input.playStation=true;s.mts.input.psFrames=77;s.mts.input.nativeQueued[1]=9;const auto input=p::inputText(s);assert(input.find("frames=77")!=input.npos&&input.find("nativeDownQueued=9")!=input.npos&&input.find("ack=unsupported")!=input.npos);
 const auto inputCopy=s;s={};assert(p::inputText(inputCopy)==input);
 p::ProtocolSnapshot fh;fh.provider=p::Provider::FogHub;fh.state=p::State::FinalRetained;fh.fh.encryptionKnown=true;fh.fh.command.operation=2;fh.fh.encryption.encryptionSelected=3;
 assert(p::protocolText(fh).find("ackField=resume")!=std::string::npos&&p::protocolText(fh).find("enc_selected=3")!=std::string::npos);
 stream::quality::Controller a,b;a.enable(true,42);b.enable(true,42);stream::quality::NumericEvent q;std::string legacy;assert(a.pollNumericEvent(q)&&b.pollEvent(legacy));assert(stream::quality::numericEventText(q)==legacy);
 stream::quality::Controller bounded;failAlloc=true;for(unsigned i=1;i<=40;++i)bounded.enable(i%2==0,i);unsigned count=0;while(bounded.pollNumericEvent(q)){++count;assert(q.ms>=9);}failAlloc=false;assert(count==32);
 namespace n=stream::telemetry::network;n::NetworkEvent e;e.provider=n::Provider::Mts;e.kind=n::EventKind::RouteSelected;e.transition.route.selected=true;assert(n::networkEventText(e).find("Video RTP event: routeEvent=selected requested=all actual=unknown")==0);
 e.kind=n::EventKind::Quality;e.transition.setQuality(q);assert(n::networkEventText(e).find("Video RTP event: qualityEvent=enabled ms=40")==0);assert(n::networkEventText(e,true).find("Stream final event: qualityEvent=enabled ms=40")==0);
 q.sampleFps=1e24;q.decodedFps=1e24;q.videoWorkMs=1e24;q.videoBusy=1e24;q.uiVideoMs=1e24;q.decoderVideoMs=1e24;q.uiVideoBusy=1e24;q.decoderVideoBusy=1e24;q.nextUpMs=q.goodMs=q.evidenceMs=std::numeric_limits<std::uint64_t>::max();const auto maximum=stream::quality::numericEventText(q);assert(maximum.size()<1400&&maximum.find("decoderVideoBusy=")!=maximum.npos);q.sampleFps=std::numeric_limits<double>::max();assert(stream::quality::numericEventText(q)=="qualityEvent=unknown unavailable=invalid-metrics");
 std::cout<<"event="<<sizeof(e)<<" batch32raw="<<sizeof(e)*32<<'\n';
 std::cout<<"Protocol enum/privacy boundaries, explicit availability, exact mutex/buffer parity, immutable allocation-free numeric copies PASS; snapshot="<<sizeof(p::ProtocolSnapshot)<<'\n';
}
