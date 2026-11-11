#pragma once
#include "features/streaming/application/network_diagnostics_formatter.hpp"
namespace stream::telemetry::network {
// true is local admission with healthy sinks, not durable or remote delivery.
inline bool enqueueNetwork(fog::RunLog& log,std::uint64_t elapsedMs,Line line,const NetworkSnapshot& value)noexcept{
    try{return log.appendDeferred(std::make_unique<NetworkRecord>(elapsedMs,line,value));}
    catch(...){log.appendDeferred({});return false;}
}
struct DrainResult{unsigned accepted=0,uncertain=0,sourceOverwritten=0;};
// No free-slot prediction. appendDeferred is authoritative; false includes
// rejected or unhealthy-sink admission, so retire once as explicitly uncertain
// and stop. Retrying a potentially accepted record could duplicate a B1 event.
inline DrainResult drainNetworkEvents(StreamNetworkTelemetrySource& source,fog::RunLog& log,std::uint64_t elapsedMs,unsigned budget=4,bool final=false)noexcept{
    budget=std::min(budget,32u);
    DrainResult result;NetworkEvent event;
    for(unsigned i=0;i<budget&&source.peekNetworkEvent(event);++i){
        bool accepted=false;
        try{accepted=log.appendDeferred(std::make_unique<EventRecord>(elapsedMs,event,final));}
        catch(...){log.appendDeferred({});}
        if(!source.retireNetworkEvent(event.journalSequence,accepted))++result.sourceOverwritten;
        if(accepted)++result.accepted;else{++result.uncertain;break;}
    }
    return result;
}
// Explicit final/drop path. No event is reported delivered merely because the
// transport owner is closing; both source and log ledgers record uncertainty.
inline unsigned discardNetworkEvents(StreamNetworkTelemetrySource& source,fog::RunLog& log,unsigned budget=32)noexcept{
    unsigned dropped=0;NetworkEvent event;budget=std::min(budget,32u);
    while(dropped<budget&&source.peekNetworkEvent(event)){
        log.appendDeferred({});source.retireNetworkEvent(event.journalSequence,false);++dropped;
    }
    return dropped;
}

}
