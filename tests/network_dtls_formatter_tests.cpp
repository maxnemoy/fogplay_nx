#include "features/streaming/application/network_diagnostics_formatter.hpp"
#include "features/streaming/infrastructure/network_telemetry_capture.hpp"
#include "features/streaming/infrastructure/dtls_diagnostics.hpp"
#include <cassert>
#include <iostream>
#include <limits>
int main(){
    namespace n=stream::telemetry::network;
    rtc::DtlsDiagnostics source{};
    assert(n::dtlsText(n::captureDtls(source))==stream::dtlsDiagnosticText(source));
    source.available=true;
#define FOG_MAX(name) source.name=std::numeric_limits<std::uint64_t>::max();
    FOG_NETWORK_DTLS_FIELDS(FOG_MAX)
#undef FOG_MAX
    source.dequeued=source.recvScheduleCount=source.recvRuns=source.sendSslLockCount=source.recvSslLockCount=1;
    auto value=n::captureDtls(source);auto text=n::dtlsText(value);
    assert(text==stream::dtlsDiagnosticText(source));
    n::NetworkSnapshot snapshot;snapshot.provider=n::Provider::Mts;snapshot.mtsDtls=value;snapshot.dtls.hasValue=true;
    snapshot.dtls.state=n::State::FinalRetained;
    auto rendered=n::networkText(snapshot,n::Line::DtlsFinal);
    assert(rendered.size()<4096);n::NetworkRecord record(80,n::Line::DtlsFinal,snapshot);
    fog::RunLog::DeferredSink sink(record.maxOutputBytes());record.format(sink);
    assert(sink.text().find("[80 ms] Stream DTLS final: available=1 scope=transport-lifetime")==0);
    std::cout<<"DTLS exact legacy parity, max numeric group bytes="<<rendered.size()<<" raw="<<sizeof(n::NetworkRecord)<<" PASS\n";
}
