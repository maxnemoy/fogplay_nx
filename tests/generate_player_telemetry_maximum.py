"""Generate field-complete numeric maximum fixtures from bounded DTO declarations.
Unknown declaration syntax fails rather than skipping a new field. No memset.
"""
import re
import sys
from pathlib import Path
root=Path(__file__).resolve().parents[1]
base=root/'src/features/streaming'
files=[
 ('domain/audio_rtp_cadence.hpp','stream'),
 ('domain/udp_poll_telemetry.hpp','stream::telemetry::network'),
 ('application/input_gate_diagnostics.hpp','fog::stream'),
 ('domain/network_telemetry.hpp','stream::telemetry::network'),
 ('domain/auto_quality_snapshot.hpp','stream::quality'),
 ('domain/auto_quality_event.hpp','stream::quality'),
 ('domain/network_snapshot.hpp','stream::telemetry::network'),
 ('domain/protocol_telemetry.hpp','stream::telemetry::protocol'),
 ('domain/ice_telemetry.hpp','stream::telemetry::network'),
 ('domain/stream_telemetry.hpp','stream::telemetry'),
 ('application/playback_telemetry.hpp','stream::telemetry'),
 ('domain/media_telemetry.hpp','fog::media::telemetry'),
 ('domain/rumble_telemetry.hpp','stream::telemetry::rumble'),
]
objects=[]
for file,namespace in files:
    text=re.sub(r'/\*.*?\*/|//[^\n]*','',(base/file).read_text(),flags=re.S)
    for match in re.finditer(r'\bstruct\s+(\w+)\s*\{',text):
        name=match.group(1)
        if name in ('Record','NetworkEvent','CoreEvent','TransitionPayload','FixedEvent'):continue
        begin=match.end();depth=1;end=begin
        while depth:
            if text[end]=='{':depth+=1
            elif text[end]=='}':depth-=1
            end+=1
        body=text[begin:end-1]
        body=re.sub(r'enum class\b[^{}]*\{[^{}]*\}\s*;','',body)
        body='\n'.join(x for x in body.splitlines() if not x.lstrip().startswith('#'))
        fields=[]
        if name not in ('CoreCounters','DtlsCounters'):
            for declaration in body.split(';'):
                declaration=declaration.strip()
                if not declaration or declaration.startswith('static '):continue
                match_type=re.match(r'(?:std::array<[^>]+>|[\w:]+)\s+(.+)$',declaration,re.S)
                if not match_type:raise ValueError((file,name,declaration))
                declarators=match_type.group(1)
                while '{' in declarators:
                    revised=re.sub(r'\{[^{}]*\}','',declarators)
                    if revised==declarators:raise ValueError((file,name,declarators))
                    declarators=revised
                for item in declarators.split(','):
                    field=re.split(r'[={]',item.strip(),maxsplit=1)[0].strip()
                    if not re.fullmatch(r'\w+',field):raise ValueError((file,name,field))
                    fields.append(field)
        objects.append((namespace+'::'+name,fields))
header='''#pragma once
#include <limits>
#include <type_traits>
namespace player_maximum {
'''
for name,_ in objects:header+='inline void maximize('+name+'&);\n'
header+='''template<class T> void maximize(T& value){
    if constexpr(std::is_same_v<T,bool>)value=true;
    else if constexpr(std::is_integral_v<T>)value=std::numeric_limits<T>::is_signed?std::numeric_limits<T>::min():std::numeric_limits<T>::max();
    else if constexpr(std::is_floating_point_v<T>)value=std::numeric_limits<T>::max();
    else static_assert(std::is_enum_v<T>,"Unhandled numeric DTO type");
}
template<class T,std::size_t N> void maximize(std::array<T,N>& value){for(auto& field:value)maximize(field);}
'''
for name,fields in objects:
    header+='inline void maximize('+name+'& value){\n'
    if name.endswith('::CoreCounters'):
        header+='#define FOG_TEST_MAX(name) maximize(value.name);\nFOG_NETWORK_FH_FIELDS(FOG_TEST_MAX)\n#undef FOG_TEST_MAX\n'
    elif name.endswith('::DtlsCounters'):
        header+='maximize(value.available);\n#define FOG_TEST_MAX(name) maximize(value.name);\nFOG_NETWORK_DTLS_FIELDS(FOG_TEST_MAX)\n#undef FOG_TEST_MAX\n'
    else:
        for field in fields:header+='maximize(value.'+field+');\n'
    if name=='stream::telemetry::network::HistorySnapshot':header+='value.count=value.entries.size();\n'
    header+='}\n'
header+='}\n'
Path(sys.argv[1]).write_text(header)