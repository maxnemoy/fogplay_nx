#pragma once
#include "features/streaming/domain/stream_telemetry.hpp"
#include <array>
namespace stream::telemetry {
struct Playback {
    std::array<Timing,8> stages{}; // drain,present,transportPump,videoFeed,decoderReset,audioFeed,prepare,logEnqueue
    std::uint64_t pumpGapMaxUs=0,outsideMeasuredPumpMaxUs=0,videoQueueWaitMaxMs=0,audioQueueWaitMaxMs=0;
    std::uint64_t slowCycles=0,batchVideoTotal=0,batchVideoMax=0,frameBudgetYields=0,budgetYields=0,catchupWindows=0;
    std::uint64_t logQueued=0,logDropped=0,logWritten=0,logBatches=0,logBatchLastUs=0,logBatchMaxUs=0;
};
// Accepts the existing owner timing and RunLog value types without depending on
// either owner/header. Does not retain references or change the measurement window.
template<class T, class L>
Playback capturePlayback(const T& drain,const T& present,const T& transport,const T& video,const T& reset,
    const T& audio,const T& prepare,const T& enqueue,const L& log,
    std::uint64_t gap,std::uint64_t outside,std::uint64_t videoWait,std::uint64_t audioWait,
    std::uint64_t slow,std::uint64_t total,std::uint64_t maximum,std::uint64_t frameYields,
    std::uint64_t yields,std::uint64_t catchup) {
    Playback result;
    const T* stages[]={&drain,&present,&transport,&video,&reset,&audio,&prepare,&enqueue};
    for (std::size_t i=0;i<8;++i) result.stages[i]={stages[i]->calls,stages[i]->totalUs,stages[i]->maxUs,0};
    result.pumpGapMaxUs=gap;result.outsideMeasuredPumpMaxUs=outside;
    result.videoQueueWaitMaxMs=videoWait;result.audioQueueWaitMaxMs=audioWait;result.slowCycles=slow;
    result.batchVideoTotal=total;result.batchVideoMax=maximum;result.frameBudgetYields=frameYields;
    result.budgetYields=yields;result.catchupWindows=catchup;
    result.logQueued=log.queuedLines;result.logDropped=log.droppedLines;result.logWritten=log.writtenLines;
    result.logBatches=log.batches;result.logBatchLastUs=log.lastBatchUs;result.logBatchMaxUs=log.maxBatchUs;
    return result;
}
}
