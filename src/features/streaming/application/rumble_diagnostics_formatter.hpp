#pragma once
#include "features/streaming/domain/rumble_telemetry.hpp"
#include <string>
namespace stream::telemetry::rumble {
inline const char* mappingName(Mapping value)noexcept{
    switch(value){case Mapping::Balanced:return "balanced";case Mapping::DualBand:return "dual-band";
        case Mapping::Chromium:return "chromium";case Mapping::Disabled:return "disabled";default:return "invalid";}
}
inline std::string rumbleText(const Snapshot& value){
    return "Rumble received="+std::to_string(value.received)+" applied="+std::to_string(value.applied)+
        " stops="+std::to_string(value.stops)+" failures="+std::to_string(value.failures)+
        " result="+std::to_string(value.result)+" npad="+std::to_string(value.npad)+
        " style="+std::to_string(value.style)+" handles="+std::to_string(value.handles)+" mapping="+mappingName(value.mapping);
}
}
