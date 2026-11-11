#pragma once
#include "features/streaming/domain/control_discovery.hpp"
#include "features/streaming/domain/network_telemetry.hpp"
#include "features/streaming/application/control_discovery_formatter.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <string>
#include <string_view>
namespace stream {
// Existing caller serializes receive/peek/retire with telemetryMutex. Only
// bounded fixed-name tokens and allowlisted scalar values cross the boundary.
class IncomingControlDiagnostics {
public:
    using Description=telemetry::network::ControlDiscoveryDescription;
    using Event=telemetry::network::ControlDiscoveryEvent;
    using Shape=telemetry::network::DiscoveryShape;
    using Token=telemetry::network::DiscoveryToken;
    using K=telemetry::network::DiscoveryTokenKind;
    static Description specialDescription(std::uint64_t bytes,bool binary){Description d;d.bytes=bytes;d.binary=binary;d.oversized=!binary;return d;}
    static Description describe(const nlohmann::json& data,std::size_t bytes){
        Description d;d.bytes=bytes;
        if(data.is_object()){const auto found=data.find("id");if(found!=data.end()){
            if(!found->is_string())d.id=1;
            else {const auto& value=found->get_ref<const std::string&>();if(!validLabel(value))d.id=1;else {d.id=2;for(unsigned i=3;i<telemetry::network::DiscoveryIds.size();++i)if(value==telemetry::network::DiscoveryIds[i]){d.id=i;break;}if(d.id==2)++d.unknownNames;}}
        }}
        std::size_t budget=12;unsigned ignored=0;shape(d.schema,data,0,budget,false,false,false,"",ignored);
        budget=(d.id==9||d.id==10)?32:12;shape(d.sample,data,0,budget,true,d.id==8,d.id==9||d.id==10,"",d.unknownNames);return d;
    }
    void receive(const Description& d,std::uint64_t now){++received_;for(std::size_t i=0;i<size_;++i){auto& b=buckets_[i];if(!sameSchema(b.description,d))continue;if(b.count!=b.reported)++coalesced_;++b.count;b.last=now;b.description=d;return;}
        if(size_==buckets_.size()){++overflow_;return;}auto& b=buckets_[size_++];b.description=d;b.count=1;b.first=b.last=now;
    }
    std::size_t peekBatch(Event* out,std::size_t capacity,std::uint64_t now,bool force=false){if(!out||!capacity)return 0;std::size_t result=0;
        for(std::size_t n=0;n<size_&&result<capacity;++n){auto& b=buckets_[(cursor_+n)%size_];if(b.count==b.reported||!due(b.emitted,b.emittedAt,now,force))continue;
            if(!b.published.sequence){b.published={};b.published.sequence=++sequence_;b.published.description=b.description;b.published.count=b.count;b.published.pending=b.count-b.reported;b.published.firstMs=b.first;b.published.lastMs=b.last;b.published.peekedAtMs=now;b.published.discarded=discarded_;}
            out[result++]=b.published;
        }
        if(result<capacity&&overflow_!=overflowReported_&&due(overflowEmitted_,overflowAt_,now,force)){
            if(!overflowPublished_.sequence){overflowPublished_={};overflowPublished_.sequence=++sequence_;overflowPublished_.overflow=true;overflowPublished_.count=overflow_;overflowPublished_.pending=overflow_-overflowReported_;overflowPublished_.schemas=32;overflowPublished_.peekedAtMs=now;overflowPublished_.discarded=discarded_;}out[result++]=overflowPublished_;
        }return result;
    }
    bool retire(std::uint64_t sequence,bool admitted){if(!sequence){return false;}for(std::size_t i=0;i<size_;++i){auto& b=buckets_[i];if(b.published.sequence!=sequence)continue;b.reported=std::max(b.reported,b.published.count);b.emittedAt=b.published.peekedAtMs;++b.emitted;cursor_=(i+1)%size_;b.published={};if(!admitted)++discarded_;return true;}
        if(overflowPublished_.sequence!=sequence){return false;}overflowReported_=std::max(overflowReported_,overflowPublished_.count);overflowAt_=overflowPublished_.peekedAtMs;++overflowEmitted_;overflowPublished_={};if(!admitted)++discarded_;return true;
    }
    bool poll(std::string& out,std::uint64_t now,bool force=false){Event e;if(!peekBatch(&e,1,now,force))return false;out=telemetry::network::controlDiscoveryText(e);return retire(e.sequence,true);}
    telemetry::network::IncomingSchemaSnapshot numericSnapshot()const noexcept {telemetry::network::IncomingSchemaSnapshot out;out.received=received_;out.coalesced=coalesced_;out.overflow=overflow_;out.overflowReported=overflowReported_;out.schemas=size_;for(std::size_t i=0;i<size_;++i){const auto& b=buckets_[i];if(b.count!=b.reported){++out.pendingSchemas;out.pendingEvents+=b.count-b.reported;}}return out;}
    std::string diagnostics()const{return " incomingEvents="+std::to_string(received_)+" incomingSchemas="+std::to_string(size_)+" incomingCoalesced="+std::to_string(coalesced_)+" incomingSchemaOverflow="+std::to_string(overflow_);}
private:
    struct Bucket {Description description{};std::uint64_t count=0,reported=0,first=0,last=0,emittedAt=0,emitted=0;Event published{};};
    std::array<Bucket,32> buckets_{};std::size_t size_=0,cursor_=0;std::uint64_t received_=0,coalesced_=0,overflow_=0,overflowReported_=0,overflowAt_=0,overflowEmitted_=0,sequence_=0,discarded_=0;Event overflowPublished_{};
    static bool due(std::uint64_t emitted,std::uint64_t at,std::uint64_t now,bool force){return force||emitted<3||(now>=at&&now-at>=5000);}
    static bool sameSchema(const Description& a,const Description& b){if(a.classification!=b.classification||a.id!=b.id||a.binary!=b.binary||a.oversized!=b.oversized||a.schema.count!=b.schema.count||a.schema.truncated!=b.schema.truncated)return false;for(unsigned i=0;i<a.schema.count;++i)if(a.schema.tokens[i].kind!=b.schema.tokens[i].kind||a.schema.tokens[i].code!=b.schema.tokens[i].code)return false;return true;}
    static bool validLabel(const std::string& value){if(value.empty()||value.size()>64)return false;for(unsigned char ch:value)if(!((ch>='a'&&ch<='z')||(ch>='A'&&ch<='Z')||(ch>='0'&&ch<='9')||ch=='_'||ch=='-'||ch=='.'||ch==':'))return false;return true;}
    static bool sensitive(const std::string& key) {
        std::string lower=key;std::transform(lower.begin(),lower.end(),lower.begin(),[](unsigned char ch){return ch>='A'&&ch<='Z'?static_cast<char>(ch+('a'-'A')):static_cast<char>(ch);});
        for(const auto* word:{"token","cookie","password","auth","url","sdp","candidate","ip","address","host","session","hash","email","phone","pin","text","clipboard","body"}){if(lower.find(word)!=std::string::npos)return true;}return false;
    }

    static unsigned keyCode(std::string_view key){for(unsigned i=1;i<telemetry::network::DiscoveryKeys.size();++i)if(key==telemetry::network::DiscoveryKeys[i])return i;return 0;}
    static void append(Shape& out,K kind,unsigned code=0,std::int64_t integer=0,double number=0){if(out.count==out.tokens.size()){out.truncated=true;return;}auto& token=out.tokens[out.count++];token.kind=kind;token.code=static_cast<unsigned char>(code);if(kind==K::FloatValue)token.number=number;else token.integer=integer;}
    static K scalarKind(const nlohmann::json& v){if(v.is_discarded())return K::Invalid;if(v.is_null())return K::Null;if(v.is_boolean())return K::Bool;if(v.is_number_integer())return K::Integer;if(v.is_number())return K::Number;if(v.is_string())return K::String;return K::Binary;}
    static void value(Shape& out,const nlohmann::json& v){if(v.is_boolean()){append(out,K::BooleanValue,0,v.get<bool>());return;}if(v.is_number_unsigned()){const auto n=v.get<std::uint64_t>();if(n<=1000000000)append(out,K::UnsignedValue,0,n);else append(out,K::Integer);return;}if(v.is_number_integer()){const auto n=v.get<std::int64_t>();if(n>=-1000000000&&n<=1000000000)append(out,K::SignedValue,0,n);else append(out,K::Integer);return;}if(v.is_number_float()){const auto n=v.get<double>();if(n>=-1e9&&n<=1e9)append(out,K::FloatValue,0,0,n);else append(out,K::Number);return;}if(v.is_string()){static constexpr const char* masks[]={"random","bursty","kFecMaskRandom","kFecMaskBursty"};for(unsigned i=0;i<4;++i)if(v==masks[i]){append(out,K::FecMaskValue,i);return;}}append(out,scalarKind(v));}
    static bool allowedValue(const std::string& key,const std::string& parent,const nlohmann::json& v){if(v.is_boolean())return parent=="flags"||key=="useUnequalProtection";if(v.is_number())return key=="fecfactor"||key.rfind("recovery_",0)==0||key.rfind("metric_",0)==0;if(v.is_string()&&key=="fecMaskType")return v=="random"||v=="bursty"||v=="kFecMaskRandom"||v=="kFecMaskBursty";return false;}
    static void shape(Shape& out,const nlohmann::json& v,unsigned depth,std::size_t& budget,bool sample,bool rumble,bool values,const std::string& parent,unsigned& unknown){
        if(out.truncated)return;
        if(v.is_object()){if(depth>=3){append(out,K::ObjectDepth);return;}append(out,K::ObjectBegin);bool first=true;for(auto it=v.begin();it!=v.end();++it){if(!budget){if(!first)append(out,K::Comma);append(out,K::Ellipsis);break;}--budget;if(!first)append(out,K::Comma);first=false;const auto code=keyCode(it.key());if(!code)++unknown;append(out,K::Key,code);
            if(!validLabel(it.key())||sensitive(it.key()))append(out,K::Redacted);
            else if(sample&&rumble&&depth==0&&(it.key()=="index"||it.key()=="large_motor"||it.key()=="small_motor")&&it->is_number_integer()&&*it>=0&&*it<=65535)append(out,K::UnsignedValue,0,it->get<unsigned>());
            else if(sample&&values&&!it->is_structured()&&allowedValue(it.key(),parent,it.value())&&code)value(out,it.value());
            else shape(out,it.value(),depth+1,budget,sample,rumble,values,it.key(),unknown);
            if(out.truncated)break;
        }append(out,K::ObjectEnd);return;}
        if(v.is_array()){if(depth>=3){append(out,K::ArrayDepth);return;}append(out,K::ArrayBegin);for(std::size_t i=0;i<std::min<std::size_t>(2,v.size());++i){if(i)append(out,K::Comma);shape(out,v[i],depth+1,budget,sample,rumble,values,parent,unknown);}if(v.size()>2){append(out,K::Comma);append(out,K::Ellipsis);}append(out,K::ArrayEnd);return;}append(out,scalarKind(v));
    }
};
}
