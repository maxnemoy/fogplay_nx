#include "features/streaming/infrastructure/incoming_control_diagnostics.hpp"
#include "features/streaming/application/control_discovery_formatter.hpp"
#include <cassert>
#include <cstring>
#include <iostream>
using Json=nlohmann::json;using D=stream::IncomingControlDiagnostics;
namespace n=stream::telemetry::network;
int main(){
 const auto rumble=D::describe(Json{{"id","gamepad_notification"},{"index",0},{"large_motor",255},{"small_motor",128}},100);
 const auto golden=n::discoverySampleText(rumble);assert(golden=="id=gamepad_notification bytes=100 fields={id:string,index:0,large_motor:255,small_motor:128}");
 const auto stop=D::describe(Json{{"id","gamepad_notification"},{"index",0},{"large_motor",0},{"small_motor",0}},101);assert(n::discoverySchemaText(rumble)==n::discoverySchemaText(stop));assert(n::discoverySampleText(stop)!=golden);
 const auto params=D::describe(Json{{"id","params"},{"fecMaskType","bursty"},{"fecfactor",30},{"useUnequalProtection",false}},76);
 assert(n::discoverySampleText(params)=="id=params bytes=76 fields={fecMaskType:bursty,fecfactor:30,id:string,useUnequalProtection:false}");
 const auto features=D::describe(Json{{"id","network_features_state"},{"network_features",{{"flags",{{"packet_recovery",true},{"fresh_metrics",false}}},{"parameters",{{"metric_window_ms",2000},{"recovery_max_hold_ms",120}}}}}},393);
 const auto featureText=n::discoverySampleText(features);assert(featureText.find("packet_recovery:true")!=std::string::npos&&featureText.find("metric_window_ms:2000")!=std::string::npos&&featureText.find("recovery_max_hold_ms:120")!=std::string::npos);
 const auto hidden=D::describe(Json{{"id","innocent_Identifier_7e2b9"},{"opaqueIdentifier918273","a-value"},{"accessToken","TOKEN-CANARY"},{"params",{{"candidate", "192.0.2.1"}}}},456);
 const auto hiddenText=n::discoverySampleText(hidden);assert(hidden.unknownNames>=2);for(auto* secret:{"innocent_Identifier_7e2b9","opaqueIdentifier918273","a-value","TOKEN-CANARY","192.0.2.1"}){assert(hiddenText.find(secret)==std::string::npos);assert(std::string(reinterpret_cast<const char*>(&hidden),sizeof(hidden)).find(secret)==std::string::npos);}assert(hiddenText.find("unknown:string")!=std::string::npos);
 for(const auto& bad:{std::string("bad\nid"),std::string(65,'x'),std::string("with spaces")}){const auto d=D::describe(Json{{"id",bad},{bad,"secret"}},1);assert(n::discoverySchemaText(d).find("id=<invalid>")==0);assert(n::discoverySampleText(d).find("secret")==std::string::npos);}
 assert(n::discoverySchemaText(D::describe(Json::parse("{",nullptr,false),1))=="id=<missing> schema=invalid");
 D source;assert(!source.retire(0,true));for(unsigned i=0;i<1000;++i)source.receive(rumble,100+i);
 n::ControlDiscoveryEvent e,again;assert(source.peekBatch(&e,1,1100)==1&&source.peekBatch(&again,1,1101)==1);assert(e.sequence==again.sequence&&e.count==1000&&e.firstMs==100&&e.lastMs==1099);
 // A real new arrival while an immutable record waits must survive retirement.
 source.receive(stop,1102);assert(source.retire(e.sequence,true));assert(source.numericSnapshot().pendingEvents==1);assert(source.peekBatch(&again,1,1103)==1&&again.count==1001&&again.pending==1);assert(n::controlDiscoveryText(e).find("large_motor:255")!=std::string::npos&&n::controlDiscoveryText(again).find("large_motor:0")!=std::string::npos);assert(source.retire(again.sequence,false));
 source.receive(rumble,1104);assert(source.peekBatch(&again,1,1104)==1&&again.discarded==1);source.retire(again.sequence,true);
 source.receive(rumble,1105);assert(source.peekBatch(&again,1,6103)==0);assert(source.peekBatch(&again,1,0)==0);assert(source.peekBatch(&again,1,6104)==1);source.retire(again.sequence,true);
 D retained=source;retained.receive(stop,6105);assert(retained.peekBatch(&e,1,6105,true)==1);source={};assert(n::controlDiscoveryText(e).find("discoverySchema=2")!=std::string::npos);assert(retained.retire(e.sequence,false));assert(retained.numericSnapshot().pendingEvents==0);
 D collapsed;for(unsigned i=0;i<1000;++i)collapsed.receive(D::describe(Json{{"id","unknown"+std::to_string(i)},{"field",1}},1),i);assert(collapsed.numericSnapshot().schemas==1&&collapsed.numericSnapshot().overflow==0);
 // Overflow still accounts truly distinct fixed-key schemas and coalesces all
 // excess unknown names rather than keeping identifiers in a dictionary.
 D overflow;for(unsigned i=0;i<40;++i){Json object{{"id","stats"}};for(unsigned k=1;k<=6;++k)if(i&(1u<<(k-1)))object[n::DiscoveryKeys[k]]=1;overflow.receive(D::describe(object,1),i);}assert(overflow.numericSnapshot().schemas==32&&overflow.numericSnapshot().overflow==8);
 unsigned emitted=0;std::string text;while(overflow.poll(text,40,true)){++emitted;assert(text.size()<1400);}assert(emitted==33);
 std::cout<<"Numeric discovery known-schema golden parity, fixed-name privacy, schema2 collapse, immutable peek/admission, concurrent newer counts, throttling, overflow/final/drop PASS; event="<<sizeof(e)<<'\n';
}
