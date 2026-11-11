#pragma once
#include "features/streaming/application/ui_diagnostics_formatter.hpp"
#include <optional>

namespace fog::stream::diagnostics {
// One periodic UI queue reservation, even when a process-global thread chunk
// accompanies this window. Each source keeps its own original capture clock.
class OutputRecord final : public ::fog::RunLog::DeferredRecord {
public:
    OutputRecord(::stream::telemetry::ui::LoopSnapshot loop,std::optional<::stream::telemetry::ui::ThreadChunk> threads)
        :loop_(loop),threads_(std::move(threads)) {
        if(threads_) {
            const auto& t=*threads_;
            if(t.size>64||t.total>128||t.first>t.total||t.size>t.total-t.first||!::stream::telemetry::ui::threadLineCount(t))
                throw std::length_error("invalid output thread chunk");
        }
    }
    std::size_t rawBytes()const noexcept override{return sizeof(*this);}
    std::size_t lineCount()const noexcept override{return 2+(threads_?::stream::telemetry::ui::threadLineCount(*threads_):0);}
    std::size_t maxOutputBytes()const noexcept override{return 8192+(threads_?::stream::telemetry::ui::threadLineCount(*threads_)*512:0);}
    void format(::fog::RunLog::DeferredSink& sink)const override{
        namespace ui=::stream::telemetry::ui;
        sink.append(ui::formatLoopLine(loop_,0));sink.nextLine();sink.append(ui::formatLoopLine(loop_,1));
        if(threads_)for(std::size_t i=0;i<ui::threadLineCount(*threads_);++i){sink.nextLine();sink.append(ui::formatThreadLine(*threads_,i));}
    }
private:
    const ::stream::telemetry::ui::LoopSnapshot loop_;
    const std::optional<::stream::telemetry::ui::ThreadChunk> threads_;
};
static_assert(sizeof(OutputRecord)<=::fog::RunLog::maxDeferredRawBytes);
}
