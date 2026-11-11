#pragma once
#include "app/app_diagnostics.hpp"
#include "features/streaming/application/stream_diagnostics_formatter.hpp"
#include "features/streaming/application/player_diagnostics_formatter.hpp"
#include "features/streaming/application/output_diagnostics_formatter.hpp"
namespace fogplay::diagnostics {
inline bool diagnosticPlayer(std::uint64_t elapsedMs,fog::stream::PlayerDiagnosticPhase phase,const fog::stream::PlayerDiagnosticSnapshot& value,stream::telemetry::Playback playback,fog::stream::diagnostics::OutputInput input={}) noexcept {
    try {
        using Phase=fog::stream::PlayerDiagnosticPhase;
        const auto admission=phase==Phase::Final||phase==Phase::Closed?fog::RunLog::Admission::Terminal:fog::RunLog::Admission::Ordinary;
        return deferredDiagnostic(std::make_unique<fog::stream::diagnostics::PlayerRecord>(elapsedMs,phase,value,playback,input),admission);
    }
    catch(...) {runLog.appendDeferred({});return false;}
}
inline bool diagnosticOutput(stream::telemetry::ui::LoopSnapshot loop,std::optional<stream::telemetry::ui::ThreadChunk> threads) noexcept {
    try {return deferredDiagnostic(std::make_unique<fog::stream::diagnostics::OutputRecord>(loop,std::move(threads)));}
    catch(...) {runLog.appendDeferred({});return false;}
}
inline bool diagnosticMenuImage(const fog::media::telemetry::Snapshot& value) noexcept {
    try {return deferredDiagnostic(std::make_unique<fog::stream::diagnostics::MenuImageRecord>(diagnosticElapsedMs(),value));}
    catch(...) {runLog.appendDeferred({});return false;}
}
inline bool diagnosticMediaStall(const fog::media::telemetry::StallEvent& event) noexcept {
    try {return deferredDiagnostic(std::make_unique<fog::stream::diagnostics::StallRecord>(diagnosticElapsedMs(),event));}
    catch(...) {runLog.appendDeferred({});return false;}
}
inline bool diagnosticControlDiscovery(std::uint64_t elapsedMs,const stream::telemetry::network::ControlDiscoveryEvent* values,std::size_t count) noexcept {
    try {return deferredDiagnostic(std::make_unique<fog::stream::diagnostics::ControlDiscoveryRecord>(elapsedMs,values,count));}
    catch(...) {runLog.appendDeferred({});return false;}
}
inline bool diagnosticNetworkEvents(std::uint64_t elapsedMs,const stream::telemetry::network::NetworkEvent* values,std::size_t count,bool final) noexcept {
    try { return deferredDiagnostic(std::make_unique<fog::stream::diagnostics::NetworkEventBatchRecord>(elapsedMs,values,count,final)); }
    catch(...) {runLog.appendDeferred({});return false;}
}
inline void diagnosticTransport(const stream::StreamTelemetrySource& source, stream::telemetry::Line kind) noexcept {
    try {
        const auto value=source.captureTelemetry(stream::telemetry::Capture::Transport);
        deferredDiagnostic(std::make_unique<stream::telemetry::Record>(diagnosticElapsedMs(),kind,value));
    }catch(...){runLog.appendDeferred({});}
}
inline void diagnosticInput(const stream::StreamTelemetrySource& source) noexcept {
    try {
        const auto value=source.captureTelemetry(stream::telemetry::Capture::Input);
        deferredDiagnostic(std::make_unique<stream::telemetry::Record>(diagnosticElapsedMs(),stream::telemetry::Line::Input,value));
    }catch(...){runLog.appendDeferred({});}
}
inline void diagnosticPlayback(stream::telemetry::Playback value) noexcept {
    try {deferredDiagnostic(std::make_unique<stream::telemetry::Record>(diagnosticElapsedMs(),value));}
    catch(...){runLog.appendDeferred({});}
}
}
