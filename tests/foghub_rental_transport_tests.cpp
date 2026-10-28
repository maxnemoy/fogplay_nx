#include "features/providers/infrastructure/foghub_transport.hpp"
#include <curl/curl.h>
#include <cassert>
#include <cstdarg>
#include <iostream>
#include <map>

using namespace fog::foghub;
namespace {
std::string url,method,capturedForm;
const char* form=nullptr;
curl_off_t formSize=0;
std::map<CURLoption,long> options;
unsigned performed=0;
}
extern "C" CURLcode __real_curl_easy_setopt(CURL*,CURLoption,...);
extern "C" CURLcode __wrap_curl_easy_setopt(CURL* handle,CURLoption option,...) {
    va_list args; va_start(args,option);
    CURLcode result;
    if (option==CURLOPT_WRITEFUNCTION || option==CURLOPT_HEADERFUNCTION) {
        const auto value=va_arg(args,curl_write_callback);
        result=__real_curl_easy_setopt(handle,option,value);
    } else if (option==CURLOPT_XFERINFOFUNCTION) {
        const auto value=va_arg(args,curl_xferinfo_callback);
        result=__real_curl_easy_setopt(handle,option,value);
    } else if (option==CURLOPT_WRITEDATA || option==CURLOPT_HEADERDATA || option==CURLOPT_XFERINFODATA) {
        auto* value=va_arg(args,void*);
        result=__real_curl_easy_setopt(handle,option,value);
    } else if (option==CURLOPT_POSTFIELDSIZE_LARGE) {
        formSize=va_arg(args,curl_off_t);
        result=__real_curl_easy_setopt(handle,option,formSize);
    } else if (option<CURLOPTTYPE_OBJECTPOINT) {
        const auto value=va_arg(args,long); options[option]=value;
        if (option==CURLOPT_HTTPGET && value==1) method="GET";
        if (option==CURLOPT_POST && value==1) method="POST";
        result=__real_curl_easy_setopt(handle,option,value);
    } else {
        assert(option<CURLOPTTYPE_FUNCTIONPOINT);
        const auto* value=va_arg(args,const char*);
        if (option==CURLOPT_URL) {
            url=value; method.clear(); form=nullptr; formSize=0; options.clear();
        }
        if (option==CURLOPT_POSTFIELDS) form=value;
        result=__real_curl_easy_setopt(handle,option,value);
    }
    va_end(args); return result;
}
// Every transfer is intercepted before DNS/socket activity. A lost response is
// deliberate: the transport must not follow a redirect or retry either command.
extern "C" CURLcode __wrap_curl_easy_perform(CURL*) {
    ++performed;
    assert(options.at(CURLOPT_SSL_VERIFYPEER)==1 && options.at(CURLOPT_SSL_VERIFYHOST)==2 &&
        options.at(CURLOPT_FOLLOWLOCATION)==0 && options.at(CURLOPT_MAXREDIRS)==0);
    capturedForm=form ? std::string(form,static_cast<std::size_t>(formSize)) : "";
    return CURLE_OPERATION_TIMEDOUT;
}
template<class F> void rejects(TransportFailure expected,F&& action) {
    bool rejected=false;
    try { action(); } catch (const TransportError& error) { rejected=true; assert(error.kind()==expected); }
    assert(rejected);
}
int main() {
    auto transport=makeWebTransport();
    for (const auto& path : {"/pc/play?id=42","/pc/play?id=42&resume=0","/pc/play-success?id=42&withResume=0"}) {
        const auto before=performed;
        rejects(TransportFailure::Network,[&]{transport->get(path);});
        assert(performed==before+1 && method=="GET" && capturedForm.empty());
    }
    const auto reads=performed;
    for (const auto& path : {"/pc/play", "/pc/play?id=0", "/pc/play?id=042", "/pc/play?id=%34%32", "/pc/play?id=42&extra=1",
        "/pc/play?id=42&resume=1", "/pc/play?id=42&resume=0&extra=1", "/pc/play?resume=0&id=42",
        "/pc/play-success?id=42", "/pc/play-success?id=42&withResume=1", "/pc/play-success?withResume=0&id=42",
        "/pc/play-success?id=42&withResume=0&extra=1", "/session/close?id=9000042"})
        rejects(TransportFailure::InvalidRequest,[&]{transport->get(path);});
    assert(performed==reads);
    const fog::Query fields{{"_csrf","synthetic&a=b"},{"moonlight_url","192.0.2.42:47989"},{"Pc[pin]","0123"}};
    rejects(TransportFailure::Network,[&]{transport->submitPairing("42",fields);});
    assert(performed==reads+1 && url=="https://foghub.ru/pc/play?id=42" && method=="POST");
    assert(capturedForm=="_csrf=synthetic%26a%3Db&moonlight_url=192.0.2.42%3A47989&Pc%5Bpin%5D=0123");
    rejects(TransportFailure::Network,[&]{transport->submitPairing("42",fields,true);});
    assert(performed==reads+2 && url=="https://foghub.ru/pc/play?id=42&resume=0" && method=="POST");
    auto bonusFields=fields; bonusFields.emplace_back("Pc[use_bonus]","0");
    rejects(TransportFailure::Network,[&]{transport->submitPairing("42",bonusFields);});
    assert(performed==reads+3 && method=="POST" &&
        capturedForm=="_csrf=synthetic%26a%3Db&moonlight_url=192.0.2.42%3A47989&Pc%5Bpin%5D=0123&Pc%5Buse_bonus%5D=0");
    rejects(TransportFailure::Network,[&]{transport->submitPairing("42",bonusFields,true);});
    assert(performed==reads+4 && url=="https://foghub.ru/pc/play?id=42&resume=0" &&
        capturedForm.find("Pc%5Buse_bonus%5D=0")!=std::string::npos);
    const auto posts=performed;
    for (const auto& id : {"", "0", "042", "42&other=1"})
        rejects(TransportFailure::InvalidRequest,[&]{transport->submitPairing(id,fields);});
    for (const auto& invalid : {fog::Query{{"_csrf","x"},{"_csrf","x"},{"Pc[pin]","0123"}},
        fog::Query{{"_csrf","x"},{"moonlight_url","host:1"},{"Pc[pin]","abcd"}},
        fog::Query{{"_csrf","x"},{"moonlight_url","host:1"},{"Pc[pin]","0123"},{"extra","1"}},
        fog::Query{{"_csrf","x"},{"moonlight_url","host:1"},{"Pc[pin]","0123"},{"Pc[use_bonus]","1"}},
        fog::Query{{"_csrf","x"},{"moonlight_url","host:1"},{"Pc[use_bonus]","0"}},
        fog::Query{{"_csrf","x"},{"Pc[pin]","0123"},{"Pc[use_bonus]","0"}},
        fog::Query{{"_csrf","x"},{"moonlight_url","host:1"},{"Pc[pin]","0123"},{"Pc[use_bonus]","0"},{"Pc[use_bonus]","0"}}})
        rejects(TransportFailure::InvalidRequest,[&]{transport->submitPairing("42",invalid);});
    assert(performed==posts);
    rejects(TransportFailure::Network,[&]{transport->closeRental("9000042");});
    assert(performed==posts+1 && url=="https://foghub.ru/session/close?id=9000042" && method=="GET" && capturedForm.empty());
    rejects(TransportFailure::InvalidRequest,[&]{transport->closeRental("9000042");});
    rejects(TransportFailure::InvalidRequest,[&]{transport->closeRental("9000042&other=1");});
    assert(performed==posts+1);
    const auto reviewStart=performed;
    rejects(TransportFailure::Network,[&]{transport->get("/session/review?id=9000042");});
    assert(performed==reviewStart+1 && url=="https://foghub.ru/session/review?id=9000042" && method=="GET");
    for(const auto& path:{"/session/review","/session/review?id=0","/session/review?id=09000042",
        "/session/review?id=%39","/session/review?id=9000042&fromPcId=42","/session/review?id=9000042&"})
        rejects(TransportFailure::InvalidRequest,[&]{transport->get(path);});
    assert(performed==reviewStart+1);
    const fog::Query rating{{"_csrf","synthetic&a=b"},{"Review[rating]","4"},{"Review[message]","line1\nстрока2 &+="}};
    rejects(TransportFailure::Network,[&]{transport->submitSessionRating("9000042",rating);});
    assert(performed==reviewStart+2 && url=="https://foghub.ru/session/review?id=9000042" && method=="POST");
    assert(capturedForm=="_csrf=synthetic%26a%3Db&Review%5Brating%5D=4&Review%5Bmessage%5D=line1%0A%D1%81%D1%82%D1%80%D0%BE%D0%BA%D0%B02%20%26%2B%3D");
    for(const auto& id:{"","0","09000042","9000042&fromPcId=42"})
        rejects(TransportFailure::InvalidRequest,[&]{transport->submitSessionRating(id,rating);});
    for(const auto& invalid:{fog::Query{{"_csrf","x"},{"Review[rating]","0"},{"Review[message]",""}},
        fog::Query{{"_csrf","x"},{"Review[rating]","6"},{"Review[message]",""}},
        fog::Query{{"_csrf",""},{"Review[rating]","4"},{"Review[message]",""}},
        fog::Query{{"_csrf","x"},{"Review[rating]","4"}},
        fog::Query{{"_csrf","x"},{"Review[rating]","4"},{"_method","POST"}},
        fog::Query{{"_csrf","x"},{"Review[rating]","4"},{"Review[rating]","5"}},
        fog::Query{{"_csrf","x"},{"Review[rating]","4"},{"Review[message]",std::string(2001,'a')}},
        fog::Query{{"_csrf","x"},{"Review[rating]","4"},{"Review[message]",std::string("a\0b",3)}}})
        rejects(TransportFailure::InvalidRequest,[&]{transport->submitSessionRating("9000042",invalid);});
    assert(performed==reviewStart+2);
    std::cout << "FogHub rental transport offline tests passed\n";
}
