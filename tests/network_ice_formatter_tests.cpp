#include "features/streaming/infrastructure/ice_telemetry_capture.hpp"
#include "features/streaming/application/network_diagnostics_formatter.hpp"
#include <rtc/icediagnostics.hpp>
#include <cassert>
#include <climits>
#include <iostream>

int main(){
    namespace net=stream::telemetry::network;
    rtc::IceDiagnostics source;
    source.availability=rtc::IceDiagnostics::Availability::Available;
    source.provenance=rtc::IceDiagnostics::Provenance::Live;
    source.backend=rtc::IceDiagnostics::Backend::LibJuice;
    source.failureReason=rtc::IceDiagnostics::FailureReason::StunSend;
    source.recvErrno=INT_MIN;source.socketError=-11;source.stunSendErrno=-12;
    source.socketErrorAvailable=true;source.udpDatagrams=UINT64_MAX;
    const auto captured=net::captureIce(source);
    source.recvErrno=0;source.udpDatagrams=0;
    const auto text=net::iceText(captured,true);
    assert(text.find("recvErrno=-2147483648")!=text.npos);
    assert(text.find("socketError=-11 socketErrorAvailable=1")!=text.npos);
    assert(text.find("stunSendErrno=-12")!=text.npos);
    assert(text.find("udpDatagrams=18446744073709551615")!=text.npos);
    assert(text.find("iceReason=stun-send")!=text.npos);
    for(const auto availability:{rtc::IceDiagnostics::Availability::NotStarted,rtc::IceDiagnostics::Availability::NotSupported,
        rtc::IceDiagnostics::Availability::ReadFailed,rtc::IceDiagnostics::Availability::ClosedWithoutSample}){
        source.availability=availability;
        const auto absent=net::iceText(net::captureIce(source),false);
        assert(absent.find("iceDiagnostics=unavailable") == 0);
        assert(absent.find("udpDatagrams=")==absent.npos);
    }
    source.availability=static_cast<rtc::IceDiagnostics::Availability>(255);
    source.failureReason=static_cast<rtc::IceDiagnostics::FailureReason>(254);
    const auto unknown=net::captureIce(source);
    assert(unknown.availability==net::IceCounters::Availability::ReadFailed);
    assert(unknown.failureReason==net::IceCounters::FailureReason::Unknown);
    std::uint64_t sourceAt=0;bool clockKnown=false;
    net::updateIceSourceClock(net::IceCounters::Provenance::FinalRetained,900,sourceAt,clockKnown);assert(!clockKnown&&sourceAt==0);
    net::updateIceSourceClock(net::IceCounters::Provenance::Live,1000,sourceAt,clockKnown);assert(clockKnown&&sourceAt==1000);
    net::updateIceSourceClock(net::IceCounters::Provenance::FinalRetained,2000,sourceAt,clockKnown);assert(clockKnown&&sourceAt==1000);
    net::NetworkSnapshot snapshot;snapshot.provider=net::Provider::Mts;
    snapshot.mtsIce=captured;snapshot.ice.hasValue=true;snapshot.ice.state=net::State::FinalRetained;
    net::NetworkRecord record(42,net::Line::IceFinal,snapshot);
    fog::RunLog::DeferredSink sink(record.maxOutputBytes());record.format(sink);sink.finish();
    assert(sink.text().find("[42 ms] Stream ICE final: iceState=")==0);
    assert(sink.text().find("snapshotState=final")!=sink.text().npos);
    std::cout<<"Numeric ICE signed values, immutable capture, unavailable/unknown and pure final formatter PASS; snapshot="<<sizeof(snapshot)<<'\n';
}
