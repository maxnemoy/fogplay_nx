#pragma once
#include "features/streaming/application/ui_diagnostics_formatter.hpp"
#include "features/playstation/application/input_telemetry.hpp"
#include <optional>

namespace fogplay::diagnostics {
// Cross-feature composition belongs to the application. One final record owns
// the window, lifetime and optional PS evidence at their original clocks.
class FinalOutputRecord final : public fog::RunLog::DeferredRecord {
public:
    using Loop=stream::telemetry::ui::LoopSnapshot;
    using PlayStation=fog::playstation::telemetry::Snapshot;
    static constexpr std::size_t BaseOutputBytes=16384;
    static constexpr std::size_t MaximumOutputBytes=BaseOutputBytes+1+fog::playstation::telemetry::Record::MaximumOutputBytes;
    FinalOutputRecord(Loop window,Loop lifetime,std::optional<PlayStation> ps)
        :window_(window),lifetime_(lifetime),ps_(std::move(ps)) {
        using Scope=stream::telemetry::ui::LoopScope;
        if(window_.scope!=Scope::FinalWindow||lifetime_.scope!=Scope::FinalLifetime||(ps_&&!ps_->final))
            throw std::invalid_argument("invalid final output scopes");
    }
    std::size_t rawBytes()const noexcept override{return sizeof(*this);}
    std::size_t lineCount()const noexcept override{return 4+(ps_?(ps_->hasTransport?4:3):0);}
    std::size_t maxOutputBytes()const noexcept override{return BaseOutputBytes+(ps_?1+fog::playstation::telemetry::Record(*ps_).maxOutputBytes():0);}
    void format(fog::RunLog::DeferredSink& sink)const override{
        stream::telemetry::ui::LoopRecord(window_,lifetime_).format(sink);
        if(ps_){sink.nextLine();fog::playstation::telemetry::Record(*ps_).format(sink);}
    }
private:
    const Loop window_,lifetime_;
    const std::optional<PlayStation> ps_;
};
static_assert(sizeof(FinalOutputRecord)<=fog::RunLog::maxDeferredRawBytes);
inline bool enqueueFinalOutput(fog::RunLog& log,FinalOutputRecord::Loop window,FinalOutputRecord::Loop lifetime,
        std::optional<FinalOutputRecord::PlayStation> ps)noexcept{
    try{return log.appendDeferred(std::make_unique<FinalOutputRecord>(window,lifetime,std::move(ps)),fog::RunLog::Admission::Terminal);}
    catch(...){log.appendDeferred({});return false;}
}
}
